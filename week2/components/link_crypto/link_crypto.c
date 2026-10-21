/*
 * AES-128-CCM packet encryption - implementation.
 *
 * Nonce (13 bytes), built the same way on both sides:
 *
 *   [ sender node ID (1) | counter (4, big endian) | 0 x 8 ]
 *
 *  - The sender ID keeps the two directions apart: both boards use the
 *    same key and may use the same counter values, but their nonces differ.
 *  - The counter goes up by one for every packet. Its upper 16 bits are a
 *    boot counter kept in NVS, so after a reset the counter continues from
 *    a fresh range instead of restarting at 0 (reusing a nonce with the
 *    same key would break CCM's security).
 */
#include "link_crypto.h"

#include <stdbool.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "mbedtls/ccm.h"
#include "nvs.h"

static const char *TAG = "link_crypto";

#define NONCE_LEN   13
#define NVS_NS      "link_crypto"
#define NVS_KEY     "boot"

static mbedtls_ccm_context s_ccm;
static uint8_t s_own_id;
static uint8_t s_peer_id;

static uint32_t s_tx_counter;   /* counter of the last packet we sent */
static uint32_t s_rx_last;      /* highest counter accepted from the peer */
static bool s_rx_seen;          /* false until the first valid packet */

/* ---------- helpers ---------- */

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "0011..ff" (32 chars) -> 16 bytes */
static esp_err_t parse_key(const char *hex, uint8_t key[LINK_CRYPTO_KEY_LEN])
{
    if (strlen(hex) != 2 * LINK_CRYPTO_KEY_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < LINK_CRYPTO_KEY_LEN; i++) {
        int hi = hex_value(hex[2 * i]);
        int lo = hex_value(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        key[i] = (uint8_t)(hi << 4 | lo);
    }
    return ESP_OK;
}

static void make_nonce(uint8_t nonce[NONCE_LEN], uint8_t sender_id, uint32_t counter)
{
    memset(nonce, 0, NONCE_LEN);
    nonce[0] = sender_id;
    nonce[1] = counter >> 24;
    nonce[2] = counter >> 16;
    nonce[3] = counter >> 8;
    nonce[4] = counter;
}

/* Increment the boot counter in NVS and start the TX counter at the
 * beginning of that boot's range: (boot << 16). Each range has room for
 * 65535 packets. */
static esp_err_t reserve_counter_range(void)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &nvs), TAG, "nvs_open");

    uint16_t boot = 0;
    nvs_get_u16(nvs, NVS_KEY, &boot);   /* stays 0 the very first time */
    boot++;
    esp_err_t err = nvs_set_u16(nvs, NVS_KEY, boot);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    ESP_RETURN_ON_ERROR(err, TAG, "saving boot counter");

    if (boot == 0xFFFF) {
        ESP_LOGW(TAG, "boot counter is about to wrap - change the key!");
    }
    s_tx_counter = (uint32_t)boot << 16;
    return ESP_OK;
}

/* ---------- public API ---------- */

esp_err_t link_crypto_init(const char *key_hex, uint8_t own_id, uint8_t peer_id)
{
    uint8_t key[LINK_CRYPTO_KEY_LEN];
    if (parse_key(key_hex, key) != ESP_OK) {
        ESP_LOGE(TAG, "key must be exactly 32 hex characters");
        return ESP_ERR_INVALID_ARG;
    }

    s_own_id = own_id;
    s_peer_id = peer_id;
    s_rx_seen = false;
    ESP_RETURN_ON_ERROR(reserve_counter_range(), TAG, "counter init failed");

    mbedtls_ccm_init(&s_ccm);
    int ret = mbedtls_ccm_setkey(&s_ccm, MBEDTLS_CIPHER_ID_AES, key, 8 * LINK_CRYPTO_KEY_LEN);
    memset(key, 0, sizeof(key));    /* don't leave the key on the stack */
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_ccm_setkey failed: -0x%04X", -ret);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "AES-128-CCM ready, TX counter starts at 0x%08lX", s_tx_counter + 1);
    return ESP_OK;
}

size_t link_crypto_seal(const uint8_t *msg, size_t len, uint8_t *pkt)
{
    /* Next counter value. If this boot's range is used up, take a new one
     * so we never step into the range of the next boot. */
    s_tx_counter++;
    if ((s_tx_counter & 0xFFFF) == 0) {
        ESP_ERROR_CHECK(reserve_counter_range());
        s_tx_counter++;
    }

    /* [counter][ciphertext][tag] */
    uint8_t *ctr = pkt;
    uint8_t *ciphertext = pkt + LINK_CRYPTO_CTR_LEN;
    uint8_t *tag = ciphertext + len;

    ctr[0] = s_tx_counter >> 24;
    ctr[1] = s_tx_counter >> 16;
    ctr[2] = s_tx_counter >> 8;
    ctr[3] = s_tx_counter;

    uint8_t nonce[NONCE_LEN];
    make_nonce(nonce, s_own_id, s_tx_counter);
    ESP_ERROR_CHECK(mbedtls_ccm_encrypt_and_tag(&s_ccm, len, nonce, NONCE_LEN, NULL, 0,
                                                msg, ciphertext, tag, LINK_CRYPTO_TAG_LEN));
    return len + LINK_CRYPTO_OVERHEAD;
}

link_crypto_result_t link_crypto_open(const uint8_t *pkt, size_t pkt_len,
                                      uint8_t *msg, size_t *msg_len)
{
    if (pkt_len < LINK_CRYPTO_OVERHEAD) {
        return LINK_CRYPTO_ERR_SHORT;
    }

    size_t len = pkt_len - LINK_CRYPTO_OVERHEAD;
    const uint8_t *ciphertext = pkt + LINK_CRYPTO_CTR_LEN;
    const uint8_t *tag = ciphertext + len;
    uint32_t counter = (uint32_t)pkt[0] << 24 | (uint32_t)pkt[1] << 16 |
                       (uint32_t)pkt[2] << 8 | pkt[3];

    /* Replay check: counters from the peer must keep going up */
    if (s_rx_seen && counter <= s_rx_last) {
        return LINK_CRYPTO_ERR_REPLAY;
    }

    /* Decrypt and verify the tag. On failure mbedTLS wipes `msg`. */
    uint8_t nonce[NONCE_LEN];
    make_nonce(nonce, s_peer_id, counter);
    if (mbedtls_ccm_auth_decrypt(&s_ccm, len, nonce, NONCE_LEN, NULL, 0,
                                 ciphertext, msg, tag, LINK_CRYPTO_TAG_LEN) != 0) {
        return LINK_CRYPTO_ERR_AUTH;
    }

    /* Only remember the counter once the packet is known to be genuine,
     * otherwise a forged packet with a huge counter could block us. */
    s_rx_last = counter;
    s_rx_seen = true;
    *msg_len = len;
    return LINK_CRYPTO_OK;
}
