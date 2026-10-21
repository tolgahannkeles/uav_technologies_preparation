/*
 * Simple two-way link with the Ebyte E01-ML01DP5 (nRF24L01P + PA/LNA).
 *
 * Flash the same firmware on two boards, one with Node ID 0 and the other
 * with Node ID 1 (idf.py menuconfig -> "nRF24 Transceiver").
 * Type a command in the serial monitor and press Enter: it is sent to the
 * other board, which prints it.
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
#include "sdkconfig.h"
#include "nrf24.h"

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

static const char *TAG = "trx";

/* One command typed by the user. An nRF24 packet carries at most 32 bytes,
 * so a command is limited to 32 characters (+1 for the '\0' terminator). */
typedef struct {
    char text[NRF24_MAX_PAYLOAD + 1];
    uint8_t len;
} command_t;

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
        } else if (cmd.len < NRF24_MAX_PAYLOAD && c >= ' ') {
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

    /* The radio stays in receive mode except while it is sending */
    nrf24_start_listening();
    ESP_LOGI(TAG, "node %d ready. Type a command (max %d chars) and press Enter.",
             NODE_ID, NRF24_MAX_PAYLOAD);

    for (;;) {
        /* 1) Command waiting to be sent? Wait at most 10 ms for one, so we
         *    also get to check for incoming packets regularly. */
        command_t cmd;
        if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(10)) == pdTRUE) {
            /* nrf24_send() switches to TX mode, sends the packet and waits
             * for the other board's ACK (retrying up to 15 times) */
            esp_err_t err = nrf24_send(cmd.text, cmd.len, NULL);
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
            uint8_t buf[NRF24_MAX_PAYLOAD + 1];
            uint8_t n;
            while ((n = nrf24_read(buf, NULL)) > 0) {
                buf[n] = '\0';          /* make it a C string for printf */
                printf("\n[RX] %s\n", (char *)buf);
            }
        }
    }
}

void app_main(void)
{
    /* The queue must exist before either task starts using it */
    s_cmd_queue = xQueueCreate(CMD_QUEUE_LEN, sizeof(command_t));

    /*          function   name    stack  arg   priority  handle */
    xTaskCreate(nrf_task,  "nrf",  4096,  NULL, 5,        NULL);
    xTaskCreate(uart_task, "uart", 3072,  NULL, 4,        NULL);
}
