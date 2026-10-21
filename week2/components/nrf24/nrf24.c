/*
 * nRF24L01+ driver - implementation.
 *
 * How the chip is controlled
 * --------------------------
 *  - SPI: every transfer starts with a 1-byte command (read register,
 *    write register, write TX payload, ...). While that command byte is
 *    clocked in, the chip clocks out its STATUS register on MISO, so we get
 *    STATUS "for free" on every transfer.
 *  - CE pin: together with the PRIM_RX bit in CONFIG it selects the mode:
 *        PWR_UP=0                 -> power down (~1 uA)
 *        PWR_UP=1, CE=0           -> standby (ready, not on air)
 *        PWR_UP=1, PRIM_RX=1, CE=1 -> receiving
 *        PWR_UP=1, PRIM_RX=0, CE=1 -> transmitting the TX FIFO
 *  - IRQ pin: active low. Goes low when a packet arrives (RX_DR), when a
 *    packet was sent and ACKed (TX_DS) or when all retries failed (MAX_RT).
 *    It stays low until we clear that bit in STATUS by writing 1 to it.
 *
 * Enhanced ShockBurst (auto-ACK)
 * ------------------------------
 * When we send, the receiver answers with a short ACK packet. If the ACK
 * does not come back, the chip retransmits by itself (up to 15 times here).
 * The ACK is sent back to our TX address, which is why the transmitter's
 * pipe 0 must listen on the same address it sends to.
 */
#include "nrf24.h"

#include <string.h>
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "nrf24_regs.h"

static const char *TAG = "nrf24";

/* CONFIG bits we always keep set: CRC on, 2-byte CRC.
 * All three interrupts (RX_DR, TX_DS, MAX_RT) are left unmasked so they
 * all show up on the IRQ pin. */
#define CONFIG_BASE       (CONFIG_EN_CRC | CONFIG_CRCO)

/* SETUP_RETR = (ARD << 4) | ARC
 *   ARD = 5  -> wait (5+1)*250 us = 1500 us for an ACK before retrying
 *   ARC = 15 -> retry at most 15 times
 * 1500 us is enough for an ACK at 250 kbps; worst case send time ~25 ms. */
#define SETUP_RETR_VALUE  ((5 << 4) | 15)

/* Safety limit in nrf24_send(): if neither TX_DS nor MAX_RT shows up in
 * this time, something is wrong with the wiring or the power supply. */
#define TX_TIMEOUT_US     100000

static spi_device_handle_t s_spi;   /* ESP-IDF handle of the SPI device */
static nrf24_config_t s_cfg;        /* copy of the user's configuration */
static bool s_listening;            /* true while in RX mode (CE high) */

/* ========================================================================
 * Low level SPI helpers
 * ======================================================================== */

/* One SPI transaction: send `cmd`, then `len` data bytes.
 *   tx  - bytes to send after the command (NULL -> send NOP bytes, used
 *         when we only want to read)
 *   rx  - where to store the `len` bytes clocked out after the command
 *         (NULL -> ignore them)
 * Returns the STATUS register, which the chip sends during the command. */
static uint8_t spi_xfer(uint8_t cmd, const uint8_t *tx, uint8_t *rx, size_t len)
{
    uint8_t txbuf[1 + NRF24_MAX_PAYLOAD] = {0};
    uint8_t rxbuf[1 + NRF24_MAX_PAYLOAD] = {0};

    txbuf[0] = cmd;
    if (tx) {
        memcpy(&txbuf[1], tx, len);
    } else {
        memset(&txbuf[1], CMD_NOP, len);
    }

    /* CSN is driven low/high automatically by the ESP-IDF SPI driver */
    spi_transaction_t t = {
        .length = 8 * (1 + len),    /* in bits */
        .tx_buffer = txbuf,
        .rx_buffer = rxbuf,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));

    if (rx) {
        memcpy(rx, &rxbuf[1], len);
    }
    return rxbuf[0];    /* STATUS is always shifted out first */
}

/* Read a 1-byte register */
static uint8_t read_reg(uint8_t reg)
{
    uint8_t v;
    spi_xfer(CMD_R_REGISTER | reg, NULL, &v, 1);
    return v;
}

/* Write a 1-byte register */
static void write_reg(uint8_t reg, uint8_t v)
{
    spi_xfer(CMD_W_REGISTER | reg, &v, NULL, 1);
}

/* Write a multi-byte register (the 5-byte address registers) */
static void write_reg_buf(uint8_t reg, const uint8_t *buf, size_t len)
{
    spi_xfer(CMD_W_REGISTER | reg, buf, NULL, len);
}

/* Send a command that has no data bytes (FLUSH_TX, FLUSH_RX, NOP).
 * NOP is handy to just read STATUS. */
static uint8_t command(uint8_t cmd)
{
    return spi_xfer(cmd, NULL, NULL, 0);
}

/* Drive the CE pin. On the E01-ML01DP5, CE also switches the LNA on/off. */
static inline void ce(int level)
{
    gpio_set_level(s_cfg.pin_ce, level);
}

/* The E01 manual asks for CE low while registers are written. These two
 * wrap a register change and restore RX mode afterwards if it was active. */
static void config_begin(void)
{
    ce(0);
}

static void config_end(void)
{
    if (s_listening) {
        ce(1);
        esp_rom_delay_us(T_STBY2A_US);
    }
}

/* ========================================================================
 * IRQ pin
 * ======================================================================== */

/* Runs in interrupt context on the falling edge of IRQ. It must be short,
 * so it only wakes up the task given in the config (if any); that task
 * then talks to the chip over SPI. */
static void IRAM_ATTR irq_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    if (s_cfg.notify_task) {
        vTaskNotifyGiveFromISR(s_cfg.notify_task, &woken);
    }
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

bool nrf24_irq_pending(void)
{
    return gpio_get_level(s_cfg.pin_irq) == 0;     /* active low */
}

/* ========================================================================
 * Initialisation
 * ======================================================================== */

/* Set up the ESP32 side: CE and IRQ GPIOs and the SPI bus */
static esp_err_t init_bus(const nrf24_config_t *cfg)
{
    /* CE: output, start low (standby, LNA off) */
    gpio_config_t ce_conf = {
        .pin_bit_mask = 1ULL << cfg->pin_ce,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&ce_conf), TAG, "CE gpio");
    ce(0);

    /* IRQ: input, active low -> pull-up, interrupt on falling edge */
    gpio_config_t irq_conf = {
        .pin_bit_mask = 1ULL << cfg->pin_irq,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&irq_conf), TAG, "IRQ gpio");
    /* The GPIO ISR service may already be installed by other code: that
     * returns ESP_ERR_INVALID_STATE, which is fine. */
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(cfg->pin_irq, irq_isr, NULL), TAG, "IRQ handler");

    /* SPI bus (SCK, MOSI, MISO). Transfers are at most 33 bytes
     * (command + 32-byte payload), so no DMA is needed. */
    spi_bus_config_t bus = {
        .sclk_io_num = cfg->pin_sck,
        .mosi_io_num = cfg->pin_mosi,
        .miso_io_num = cfg->pin_miso,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 1 + NRF24_MAX_PAYLOAD,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(cfg->spi_host, &bus, SPI_DMA_DISABLED), TAG, "SPI bus");

    /* The nRF24 as a device on that bus, with its own chip select (CSN) */
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 4 * 1000 * 1000,  /* chip allows 10 MHz; 4 MHz is safe on jumper wires */
        .mode = 0,                          /* CPOL=0, CPHA=0 */
        .spics_io_num = cfg->pin_csn,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(cfg->spi_host, &dev, &s_spi), TAG, "SPI device");
    return ESP_OK;
}

esp_err_t nrf24_init(const nrf24_config_t *cfg)
{
    s_cfg = *cfg;
    s_listening = false;

    ESP_RETURN_ON_ERROR(init_bus(cfg), TAG, "bus init failed");

    /* The chip needs up to 100 ms after power-on before it accepts SPI */
    vTaskDelay(pdMS_TO_TICKS(T_POR_US / 1000));

    /* Power down while configuring */
    write_reg(REG_CONFIG, CONFIG_BASE);

    /* Check that the chip is actually there: write two registers and read
     * them back. With a wiring problem we usually read 0x00 or 0xFF. */
    write_reg(REG_SETUP_AW, 0x03);              /* 5-byte addresses */
    write_reg(REG_RF_CH, cfg->channel & 0x7F);  /* frequency = 2400 + ch MHz */
    if (read_reg(REG_SETUP_AW) != 0x03 || read_reg(REG_RF_CH) != (cfg->channel & 0x7F)) {
        ESP_LOGE(TAG, "module not responding on SPI - check wiring and 3.3 V supply");
        return ESP_ERR_NOT_FOUND;
    }

    /* Automatic retransmission (see SETUP_RETR_VALUE above) */
    write_reg(REG_SETUP_RETR, SETUP_RETR_VALUE);

    /* RF_SETUP: air data rate and output power */
    uint8_t rf_setup = RF_SETUP_PWR(cfg->pa_level);
    switch (cfg->data_rate) {
    case NRF24_DR_250KBPS: rf_setup |= RF_SETUP_DR_LOW;  break;
    case NRF24_DR_2MBPS:   rf_setup |= RF_SETUP_DR_HIGH; break;
    case NRF24_DR_1MBPS:   break;                         /* both bits 0 */
    }
    write_reg(REG_RF_SETUP, rf_setup);

    /* Dynamic payload length: packets can be 1..32 bytes and the receiver
     * finds out the length itself (otherwise every packet has a fixed size).
     * Old nRF24L01 (non-plus) clones need ACTIVATE before FEATURE becomes
     * writable; harmless to skip on a real L01P. */
    write_reg(REG_FEATURE, FEATURE_EN_DPL);
    if (read_reg(REG_FEATURE) != FEATURE_EN_DPL) {
        uint8_t key = 0x73;
        spi_xfer(CMD_ACTIVATE, &key, NULL, 1);
        write_reg(REG_FEATURE, FEATURE_EN_DPL);
    }
    write_reg(REG_DYNPD, 0x03);       /* DPL on pipes 0 and 1 */

    /* Addresses ("pipes"). The chip can listen on up to 6 addresses; we use:
     *   pipe 0 = peer address (needed to receive ACKs when we transmit)
     *   pipe 1 = our own address (where the peer sends to)
     * and TX_ADDR = peer address. Auto-ACK is enabled on both pipes. */
    write_reg(REG_EN_AA, 0x03);
    write_reg(REG_EN_RXADDR, 0x03);
    write_reg_buf(REG_RX_ADDR_P0, cfg->peer_addr, NRF24_ADDR_WIDTH);
    write_reg_buf(REG_RX_ADDR_P1, cfg->own_addr, NRF24_ADDR_WIDTH);
    write_reg_buf(REG_TX_ADDR, cfg->peer_addr, NRF24_ADDR_WIDTH);

    /* Start clean: empty both FIFOs and clear all interrupt flags */
    command(CMD_FLUSH_TX);
    command(CMD_FLUSH_RX);
    write_reg(REG_STATUS, STATUS_IRQ_ALL);

    nrf24_power_up();

    ESP_LOGI(TAG, "ready: ch %u (%u MHz), %s, PA level %d",
             cfg->channel, 2400 + cfg->channel,
             cfg->data_rate == NRF24_DR_250KBPS ? "250 kbps" :
             cfg->data_rate == NRF24_DR_1MBPS ? "1 Mbps" : "2 Mbps",
             cfg->pa_level);
    return ESP_OK;
}

/* ========================================================================
 * Power and mode
 * ======================================================================== */

void nrf24_power_up(void)
{
    config_begin();
    write_reg(REG_CONFIG, CONFIG_BASE | CONFIG_PWR_UP);
    /* Oscillator start-up: power down -> standby takes up to ~5 ms */
    vTaskDelay(pdMS_TO_TICKS(T_PD2STBY_US / 1000) + 1);
    s_listening = false;
}

void nrf24_power_down(void)
{
    ce(0);
    write_reg(REG_CONFIG, CONFIG_BASE);     /* PWR_UP = 0 */
    s_listening = false;
}

void nrf24_start_listening(void)
{
    ce(0);
    /* PRIM_RX = 1 selects receive mode */
    write_reg(REG_CONFIG, CONFIG_BASE | CONFIG_PWR_UP | CONFIG_PRIM_RX);
    /* Clear leftover TX flags so they do not keep IRQ low */
    write_reg(REG_STATUS, STATUS_TX_DS | STATUS_MAX_RT);
    ce(1);                              /* RX mode + LNA on */
    esp_rom_delay_us(T_STBY2A_US);      /* 130 us until the receiver is ready */
    s_listening = true;
}

void nrf24_stop_listening(void)
{
    ce(0);                              /* back to standby */
    s_listening = false;
}

/* ========================================================================
 * Runtime settings
 * ======================================================================== */

void nrf24_set_channel(uint8_t channel)
{
    config_begin();
    s_cfg.channel = channel & 0x7F;     /* channel is a 7-bit field */
    write_reg(REG_RF_CH, s_cfg.channel);
    config_end();
}

void nrf24_set_pa_level(nrf24_pa_level_t level)
{
    config_begin();
    s_cfg.pa_level = level;
    /* Read-modify-write: change only the power bits, keep the data rate */
    uint8_t rf_setup = read_reg(REG_RF_SETUP) & ~RF_SETUP_PWR_MASK;
    write_reg(REG_RF_SETUP, rf_setup | RF_SETUP_PWR(level));
    config_end();
}

/* ========================================================================
 * Sending and receiving
 * ======================================================================== */

esp_err_t nrf24_send(const void *data, uint8_t len, uint8_t *retries)
{
    if (len == 0 || len > NRF24_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 1) Standby, switch to TX (PRIM_RX = 0), clear old TX flags */
    ce(0);
    s_listening = false;
    write_reg(REG_CONFIG, CONFIG_BASE | CONFIG_PWR_UP);
    write_reg(REG_STATUS, STATUS_TX_DS | STATUS_MAX_RT);

    /* 2) Put the payload into the (emptied) TX FIFO */
    command(CMD_FLUSH_TX);
    spi_xfer(CMD_W_TX_PAYLOAD, data, NULL, len);

    /* 3) CE high starts the transmission. The nRF24 datasheet only needs a
     *    10 us pulse, but the E01 manual says to keep CE high for the whole
     *    transaction, so that the LNA is on while we wait for the ACK. */
    ce(1);

    /* Drop any IRQ notification left over from RX before we start waiting */
    ulTaskNotifyTake(pdTRUE, 0);

    /* 4) Wait until the chip reports the result:
     *      TX_DS  -> ACK received, packet delivered
     *      MAX_RT -> no ACK after all retries */
    int64_t deadline = esp_timer_get_time() + TX_TIMEOUT_US;
    uint8_t status;
    for (;;) {
        status = command(CMD_NOP);      /* NOP = just read STATUS */
        if (status & (STATUS_TX_DS | STATUS_MAX_RT)) {
            break;
        }
        if (esp_timer_get_time() > deadline) {
            break;
        }
        ulTaskNotifyTake(pdTRUE, 1);    /* sleep until IRQ or 1 tick */
    }
    ce(0);

    /* 5) How many retransmissions were needed (lower 4 bits of OBSERVE_TX) */
    uint8_t observe = read_reg(REG_OBSERVE_TX);
    if (retries) {
        *retries = OBSERVE_ARC_CNT(observe);
    }
    /* Clear the flags (writing 1 clears them), which releases the IRQ pin */
    write_reg(REG_STATUS, STATUS_TX_DS | STATUS_MAX_RT);

    if (status & STATUS_TX_DS) {
        return ESP_OK;
    }
    if (!(status & STATUS_MAX_RT)) {
        /* Timeout: the chip never finished. Read its state to find out why.
         *  - CONFIG lost PWR_UP -> the chip was reset (supply voltage dropped
         *    when the PA switched on)
         *  - payload still in TX FIFO -> it never started sending (CE) */
        uint8_t config = read_reg(REG_CONFIG);
        uint8_t fifo = read_reg(REG_FIFO_STATUS);
        ESP_LOGW(TAG, "TX timeout: CONFIG=0x%02X FIFO_STATUS=0x%02X STATUS=0x%02X",
                 config, fifo, status);
        if (!(config & CONFIG_PWR_UP)) {
            ESP_LOGW(TAG, "-> module was reset: power supply problem (add a capacitor on VCC)");
        } else if (!(fifo & FIFO_TX_EMPTY)) {
            ESP_LOGW(TAG, "-> packet never left the TX FIFO: check the CE wire (GPIO%d)", s_cfg.pin_ce);
        }
    }
    command(CMD_FLUSH_TX);      /* MAX_RT leaves the payload in the FIFO */
    return (status & STATUS_MAX_RT) ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

uint8_t nrf24_read(uint8_t *buf, uint8_t *pipe)
{
    /* Clear RX_DR before looking at the FIFO: a packet that lands after
     * this point sets RX_DR again and produces a new IRQ edge. */
    write_reg(REG_STATUS, STATUS_RX_DR);
    if (read_reg(REG_FIFO_STATUS) & FIFO_RX_EMPTY) {
        return 0;               /* nothing received */
    }

    /* STATUS tells which pipe the packet at the head of the FIFO came on */
    uint8_t status = command(CMD_NOP);

    /* With dynamic payloads we first ask for the length of the packet */
    uint8_t width;
    spi_xfer(CMD_R_RX_PL_WID, NULL, &width, 1);
    if (width == 0 || width > NRF24_MAX_PAYLOAD) {
        /* Corrupt length - datasheet says flush */
        command(CMD_FLUSH_RX);
        return 0;
    }

    /* Read the payload; this also removes it from the RX FIFO */
    spi_xfer(CMD_R_RX_PAYLOAD, NULL, buf, width);
    if (pipe) {
        *pipe = STATUS_RX_P_NO(status);
    }
    return width;
}

bool nrf24_carrier_detected(void)
{
    return read_reg(REG_RPD) & 0x01;
}

void nrf24_dump_registers(void)
{
    for (uint8_t r = 0; r <= REG_FEATURE; r++) {
        if (r == REG_RX_ADDR_P0 || r == REG_RX_ADDR_P1 || r == REG_TX_ADDR) {
            /* 5-byte address registers */
            uint8_t a[NRF24_ADDR_WIDTH];
            spi_xfer(CMD_R_REGISTER | r, NULL, a, sizeof(a));
            ESP_LOGI(TAG, "reg 0x%02X = %02X %02X %02X %02X %02X", r, a[0], a[1], a[2], a[3], a[4]);
        } else if (r <= REG_FIFO_STATUS || r >= REG_DYNPD) {
            /* skip the unused range 0x18..0x1B */
            ESP_LOGI(TAG, "reg 0x%02X = 0x%02X", r, read_reg(r));
        }
    }
}
