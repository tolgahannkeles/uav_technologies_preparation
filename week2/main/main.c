/*
 * Simple two-way link with the Ebyte E01-ML01DP5 (nRF24L01P + PA/LNA).
 *
 * Flash the same firmware on two boards, one with Node ID 0 and the other
 * with Node ID 1 (idf.py menuconfig -> "nRF24 Transceiver").
 * Type a command in the serial monitor and press Enter: it is encrypted
 * (AES-128-CCM, see components/link_crypto), sent to the other board,
 * decrypted and printed there. Both boards need the same key.
 *
 * The program is split into two FreeRTOS tasks:
 *
 *   uart_task                          nrf_task
 *   ---------                          --------
 *   reads characters from the   ---->  sends each command over the radio
 *   console, builds a command   queue  and prints packets received from
 *   line, puts it in the queue         the other board
 *
 * Only nrf_task talks to the radio, so the two tasks never access the SPI
 * bus at the same time. The queue is the only thing they share.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "nrf24.h"
#include "link_crypto.h"

/* Values chosen in menuconfig (main/Kconfig.projbuild) */
#define NODE_ID       CONFIG_APP_NODE_ID
#define PEER_ID       (1 - CONFIG_APP_NODE_ID)   /* the other board: 0 <-> 1 */
#define CONSOLE_UART  CONFIG_ESP_CONSOLE_UART_NUM /* UART used by idf.py monitor */

#if CONFIG_NRF_DR_250KBPS
#define DATA_RATE NRF24_DR_250KBPS
#elif CONFIG_NRF_DR_1MBPS
#define DATA_RATE NRF24_DR_1MBPS
#else
#define DATA_RATE NRF24_DR_2MBPS
#endif

#define CMD_QUEUE_LEN 8     /* how many commands can wait to be sent */

/* An nRF24 packet carries at most 32 bytes. Encryption adds a 4-byte
 * counter and an 8-byte tag, which leaves 20 bytes for the command. */
#define CMD_MAX_LEN   (NRF24_MAX_PAYLOAD - LINK_CRYPTO_OVERHEAD)

static const char *TAG = "trx";

/* One command typed by the user (+1 for the '\0' terminator) */
typedef struct {
    char text[CMD_MAX_LEN + 1];
    uint8_t len;
} command_t;

/* Print a packet as hex, so we can see what actually goes over the air */
static void print_hex(const char *label, const uint8_t *data, size_t len)
{
    printf("%s", label);
    for (size_t i = 0; i < len; i++) {
        printf(" %02x", data[i]);
    }
    printf("\n");
}

/* uart_task puts commands here, nrf_task takes them out */
static QueueHandle_t s_cmd_queue;

/* ------------------------------------------------------------------------
 * UART task: read the console and build commands
 * ------------------------------------------------------------------------ */
static void uart_task(void *arg)
{
    /* Install the UART driver on the console port so we can read what is
     * typed in the serial monitor (printf output keeps working). */
    ESP_ERROR_CHECK(uart_driver_install(CONSOLE_UART, 256, 0, 0, NULL, 0));

    command_t cmd = {0};

    for (;;) {
        /* Block until one character arrives */
        uint8_t c;
        if (uart_read_bytes(CONSOLE_UART, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }

        if (c == '\r' || c == '\n') {
            /* Enter pressed: hand the finished command over to nrf_task */
            if (cmd.len > 0) {
                cmd.text[cmd.len] = '\0';
                printf("\n");
                if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "queue full, command dropped");
                }
                cmd.len = 0;
            }
        } else if (cmd.len < CMD_MAX_LEN && c >= ' ') {
            /* Printable character: store it and echo it back, otherwise
             * the user would not see what they type */
            cmd.text[cmd.len++] = (char)c;
            putchar(c);
            fflush(stdout);
        }
    }
}

/* ------------------------------------------------------------------------
 * nRF task: owns the radio, sends commands and prints received packets
 * ------------------------------------------------------------------------ */
static void nrf_task(void *arg)
{
    /* Radio settings. Both boards use the same channel and data rate; the
     * addresses are swapped: node 0 listens on "NODE0" and sends to
     * "NODE1", node 1 does the opposite. */
    nrf24_config_t cfg = {
        .spi_host = SPI3_HOST,
        .pin_sck = CONFIG_NRF_PIN_SCK,
        .pin_mosi = CONFIG_NRF_PIN_MOSI,
        .pin_miso = CONFIG_NRF_PIN_MISO,
        .pin_csn = CONFIG_NRF_PIN_CSN,
        .pin_ce = CONFIG_NRF_PIN_CE,
        .pin_irq = CONFIG_NRF_PIN_IRQ,
        .channel = CONFIG_NRF_CHANNEL,
        .data_rate = DATA_RATE,
        .pa_level = CONFIG_NRF_PA_LEVEL,
        .own_addr = {'N', 'O', 'D', 'E', '0' + NODE_ID},
        .peer_addr = {'N', 'O', 'D', 'E', '0' + PEER_ID},
    };
    if (nrf24_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "radio init failed");
        vTaskDelete(NULL);      /* nothing to do without a radio */
    }

    /* Shared AES key from menuconfig; both boards must use the same one */
    if (link_crypto_init(CONFIG_APP_CRYPTO_KEY, NODE_ID, PEER_ID) != ESP_OK) {
        ESP_LOGE(TAG, "crypto init failed");
        vTaskDelete(NULL);
    }

    /* The radio stays in receive mode except while it is sending */
    nrf24_start_listening();
    ESP_LOGI(TAG, "node %d ready. Type a command (max %d chars) and press Enter.",
             NODE_ID, CMD_MAX_LEN);

    for (;;) {
        /* 1) Command waiting to be sent? Wait at most 10 ms for one, so we
         *    also get to check for incoming packets regularly. */
        command_t cmd;
        if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(10)) == pdTRUE) {
            /* Encrypt: [counter][ciphertext][tag] */
            uint8_t pkt[NRF24_MAX_PAYLOAD];
            size_t pkt_len = link_crypto_seal((const uint8_t *)cmd.text, cmd.len, pkt);
            print_hex("[AIR]", pkt, pkt_len);

            /* nrf24_send() switches to TX mode, sends the packet and waits
             * for the other board's ACK (retrying up to 15 times) */
            esp_err_t err = nrf24_send(pkt, pkt_len, NULL);
            nrf24_start_listening();    /* back to RX after sending */
            /* OK      : the other board received it
             * no ACK  : packet was sent, but nobody answered
             * timeout : the radio never finished sending (check CE wiring) */
            printf("[TX] %s -> %s\n", cmd.text,
                   err == ESP_OK ? "OK" : err == ESP_FAIL ? "no ACK" : "timeout");
        }

        /* 2) Packet received? The module pulls its IRQ pin low when a
         *    packet arrives; read until its receive FIFO is empty. */
        if (nrf24_irq_pending()) {
            uint8_t pkt[NRF24_MAX_PAYLOAD];
            uint8_t n;
            while ((n = nrf24_read(pkt, NULL)) > 0) {
                printf("\n");
                print_hex("[AIR]", pkt, n);

                /* Decrypt and check the tag and the counter */
                char text[NRF24_MAX_PAYLOAD + 1];
                size_t len;
                switch (link_crypto_open(pkt, n, (uint8_t *)text, &len)) {
                case LINK_CRYPTO_OK:
                    text[len] = '\0';   /* make it a C string for printf */
                    printf("[RX] %s\n", text);
                    break;
                case LINK_CRYPTO_ERR_AUTH:
                    printf("[RX] rejected: wrong key or modified packet\n");
                    break;
                case LINK_CRYPTO_ERR_REPLAY:
                    printf("[RX] rejected: replayed packet\n");
                    break;
                case LINK_CRYPTO_ERR_SHORT:
                    printf("[RX] rejected: packet too short\n");
                    break;
                }
            }
        }
    }
}

void app_main(void)
{
    /* NVS (flash key-value store) keeps the crypto boot counter */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* The queue must exist before either task starts using it */
    s_cmd_queue = xQueueCreate(CMD_QUEUE_LEN, sizeof(command_t));

    /*          function   name    stack  arg   priority  handle */
    xTaskCreate(nrf_task,  "nrf",  4096,  NULL, 5,        NULL);
    xTaskCreate(uart_task, "uart", 3072,  NULL, 4,        NULL);
}
