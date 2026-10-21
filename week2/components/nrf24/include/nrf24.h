/*
 * Minimal nRF24L01+ driver for ESP-IDF, written for the Ebyte E01-ML01DP5
 * (nRF24L01P + PA + LNA, 100 mW).
 *
 * Module specifics (E01-ML01DP5 user manual, section 4.2):
 *  - The LNA enable is tied to CE: CE=1 -> LNA on. Keep CE high while
 *    listening, and during TX keep CE high until TX_DS / MAX_RT, so the
 *    LNA is on while the chip waits for the auto-ACK.
 *  - Configure registers with the chip powered down / CE low.
 *  - The PA is driven by the chip's RF_PWR setting: NRF24_PA_MAX (0 dBm at
 *    the chip) gives ~20 dBm at the SMA connector.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define NRF24_MAX_PAYLOAD 32
#define NRF24_ADDR_WIDTH  5

typedef enum {
    NRF24_DR_250KBPS,   /* best sensitivity / range (-96 dBm on this module) */
    NRF24_DR_1MBPS,
    NRF24_DR_2MBPS,
} nrf24_data_rate_t;

/* Chip output power; the module's PA adds ~20 dB on top. */
typedef enum {
    NRF24_PA_MIN  = 0,  /* -18 dBm */
    NRF24_PA_LOW  = 1,  /* -12 dBm */
    NRF24_PA_HIGH = 2,  /*  -6 dBm */
    NRF24_PA_MAX  = 3,  /*   0 dBm */
} nrf24_pa_level_t;

typedef struct {
    spi_host_device_t spi_host;
    int pin_sck;
    int pin_mosi;
    int pin_miso;
    int pin_csn;
    int pin_ce;
    int pin_irq;
    uint8_t channel;                  /* 0..125 -> 2400 + ch MHz */
    nrf24_data_rate_t data_rate;
    nrf24_pa_level_t pa_level;
    uint8_t own_addr[NRF24_ADDR_WIDTH];   /* we listen on this (pipe 1) */
    uint8_t peer_addr[NRF24_ADDR_WIDTH];  /* we send to this (and get ACKs on pipe 0) */
    TaskHandle_t notify_task;         /* task woken (xTaskNotifyGive) on IRQ, may be NULL */
} nrf24_config_t;

/* Initialise SPI, GPIOs and the radio. Leaves the radio in standby (CE low).
 * Returns ESP_ERR_NOT_FOUND if the module does not answer on SPI. */
esp_err_t nrf24_init(const nrf24_config_t *cfg);

/* Power down (~1 uA) / back to standby-I. nrf24_init() already powers up. */
void nrf24_power_down(void);
void nrf24_power_up(void);

/* Enter RX mode (PRIM_RX=1, CE=1 -> LNA on). */
void nrf24_start_listening(void);

/* Leave RX mode (CE=0, standby-I). */
void nrf24_stop_listening(void);

/* Change settings at runtime. CE is dropped while the register is written
 * and RX mode is resumed afterwards if it was active. Both nodes must use
 * the same channel. */
void nrf24_set_channel(uint8_t channel);
void nrf24_set_pa_level(nrf24_pa_level_t level);

/* Send one packet (1..32 bytes) with Enhanced ShockBurst auto-ACK.
 * Blocks until ACK, MAX_RT or timeout. Leaves the radio in standby; call
 * nrf24_start_listening() afterwards to receive again.
 *   ESP_OK          - ACK received
 *   ESP_FAIL        - no ACK after all retransmits (MAX_RT)
 *   ESP_ERR_TIMEOUT - no IRQ from the chip (wiring / power problem)
 * *retries (optional) receives the number of retransmissions used. */
esp_err_t nrf24_send(const void *data, uint8_t len, uint8_t *retries);

/* Read one received packet. Returns payload length, or 0 if RX FIFO empty.
 * *pipe (optional) receives the pipe number (1 = addressed to us). */
uint8_t nrf24_read(uint8_t *buf, uint8_t *pipe);

/* True while the IRQ line is asserted (active low). */
bool nrf24_irq_pending(void);

/* Received Power Detector: true if > -64 dBm (chip input) was seen on the
 * channel during the last RX period. */
bool nrf24_carrier_detected(void);

/* Print all registers (debugging). */
void nrf24_dump_registers(void);
