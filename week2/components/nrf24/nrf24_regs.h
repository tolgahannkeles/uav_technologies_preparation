/*
 * nRF24L01+ register map and SPI commands (nRF24L01+ Product Specification
 * v1.0, sections 8.3 and 9). Private to the nrf24 component.
 */
#pragma once

/* SPI commands */
#define CMD_R_REGISTER      0x00    /* | register */
#define CMD_W_REGISTER      0x20    /* | register */
#define CMD_R_RX_PAYLOAD    0x61
#define CMD_W_TX_PAYLOAD    0xA0
#define CMD_FLUSH_TX        0xE1
#define CMD_FLUSH_RX        0xE2
#define CMD_REUSE_TX_PL     0xE3
#define CMD_ACTIVATE        0x50    /* nRF24L01 (non-plus) only */
#define CMD_R_RX_PL_WID     0x60
#define CMD_NOP             0xFF

/* Registers */
#define REG_CONFIG          0x00
#define REG_EN_AA           0x01
#define REG_EN_RXADDR       0x02
#define REG_SETUP_AW        0x03
#define REG_SETUP_RETR      0x04
#define REG_RF_CH           0x05
#define REG_RF_SETUP        0x06
#define REG_STATUS          0x07
#define REG_OBSERVE_TX      0x08
#define REG_RPD             0x09
#define REG_RX_ADDR_P0      0x0A
#define REG_RX_ADDR_P1      0x0B
#define REG_TX_ADDR         0x10
#define REG_FIFO_STATUS     0x17
#define REG_DYNPD           0x1C
#define REG_FEATURE         0x1D

/* CONFIG */
#define CONFIG_MASK_RX_DR   (1 << 6)
#define CONFIG_MASK_TX_DS   (1 << 5)
#define CONFIG_MASK_MAX_RT  (1 << 4)
#define CONFIG_EN_CRC       (1 << 3)
#define CONFIG_CRCO         (1 << 2)
#define CONFIG_PWR_UP       (1 << 1)
#define CONFIG_PRIM_RX      (1 << 0)

/* STATUS */
#define STATUS_RX_DR        (1 << 6)
#define STATUS_TX_DS        (1 << 5)
#define STATUS_MAX_RT       (1 << 4)
#define STATUS_RX_P_NO(s)   (((s) >> 1) & 0x07)
#define STATUS_TX_FULL      (1 << 0)
#define STATUS_IRQ_ALL      (STATUS_RX_DR | STATUS_TX_DS | STATUS_MAX_RT)

/* RF_SETUP */
#define RF_SETUP_DR_LOW     (1 << 5)
#define RF_SETUP_DR_HIGH    (1 << 3)
#define RF_SETUP_PWR(p)     (((p) & 0x03) << 1)
#define RF_SETUP_PWR_MASK   (0x03 << 1)

/* OBSERVE_TX */
#define OBSERVE_ARC_CNT(o)  ((o) & 0x0F)

/* FIFO_STATUS */
#define FIFO_TX_EMPTY       (1 << 4)
#define FIFO_RX_EMPTY       (1 << 0)

/* FEATURE */
#define FEATURE_EN_DPL      (1 << 2)

/* Timing (us) */
#define T_POR_US            100000  /* power-on reset */
#define T_PD2STBY_US        5000    /* power down -> standby (1.5 ms, 4.5 ms worst) */
#define T_STBY2A_US         130     /* standby -> TX/RX settling */
