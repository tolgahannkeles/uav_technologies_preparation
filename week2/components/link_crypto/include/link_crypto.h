/*
 * Authenticated encryption for small radio packets (AES-128-CCM).
 *
 * Every message is encrypted AND gets an authentication tag, so the
 * receiver can tell whether the packet really came from someone who knows
 * the key and was not modified on the way. Old packets that are recorded
 * and sent again (replay attack) are rejected too.
 *
 * Packet layout (fits in one 32-byte nRF24 payload):
 *
 *   +-------------+---------------------+-----------+
 *   | counter (4) | ciphertext (0..20)  |  tag (8)  |
 *   +-------------+---------------------+-----------+
 *
 * The counter is sent in clear text; it is part of the nonce and must
 * never repeat for the same key. Both boards share one key.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define LINK_CRYPTO_KEY_LEN   16    /* AES-128 */
#define LINK_CRYPTO_CTR_LEN   4
#define LINK_CRYPTO_TAG_LEN   8
#define LINK_CRYPTO_OVERHEAD  (LINK_CRYPTO_CTR_LEN + LINK_CRYPTO_TAG_LEN)

typedef enum {
    LINK_CRYPTO_OK,
    LINK_CRYPTO_ERR_SHORT,      /* packet too short to be valid */
    LINK_CRYPTO_ERR_AUTH,       /* wrong key or packet was modified */
    LINK_CRYPTO_ERR_REPLAY,     /* counter already seen: replayed packet */
} link_crypto_result_t;

/* Set up the key and the nonce counter.
 *   key_hex - 32 hex characters, e.g. "000102030405060708090a0b0c0d0e0f"
 *   own_id  - our node ID (goes into the nonce of packets we send)
 *   peer_id - the other node's ID (used to check packets we receive)
 * Needs NVS (call nvs_flash_init() first): a boot counter is stored there
 * so the nonce counter never starts from the same value twice. */
esp_err_t link_crypto_init(const char *key_hex, uint8_t own_id, uint8_t peer_id);

/* Encrypt `len` bytes of `msg` into `pkt`.
 * `pkt` must have room for len + LINK_CRYPTO_OVERHEAD bytes.
 * Returns the packet length. */
size_t link_crypto_seal(const uint8_t *msg, size_t len, uint8_t *pkt);

/* Check and decrypt a received packet into `msg`.
 * `msg` must have room for pkt_len - LINK_CRYPTO_OVERHEAD bytes.
 * On success *msg_len is the message length. */
link_crypto_result_t link_crypto_open(const uint8_t *pkt, size_t pkt_len,
                                      uint8_t *msg, size_t *msg_len);
