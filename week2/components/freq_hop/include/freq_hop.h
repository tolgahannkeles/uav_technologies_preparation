/*
 * Simple message-based frequency hopping on top of the nrf24 driver.
 *
 * Both boards know the same list of channels (the hop table) and keep an
 * index into it. After every message that gets through, BOTH sides move to
 * the next channel in the table:
 *   - the sender when it gets the ACK,
 *   - the receiver when it reads the packet.
 * So consecutive messages travel on different frequencies, and
 * interference on one channel only affects some of them.
 *
 * If the two sides get out of step (a board was reset, an ACK was lost,
 * both sent at the same moment), the sender tries the message on the other
 * channels of the table until the peer answers, and both continue from
 * there. No shared clock is needed.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Start hopping over `channels` (count entries, max 32). The table is not
 * copied, so it must stay valid (use a static/const array). A table with a
 * single channel means "no hopping". Call after nrf24_init(). */
void freq_hop_init(const uint8_t *channels, size_t count);

/* Channel we are on now (where we listen and where we send next) */
uint8_t freq_hop_channel(void);

/* Send one packet using hopping. Returns like nrf24_send(): ESP_OK when
 * the peer ACKed (on any channel), ESP_FAIL when no channel worked,
 * ESP_ERR_TIMEOUT on a radio problem. *channel (optional) receives the
 * channel the packet was delivered on. Leaves the radio listening. */
esp_err_t freq_hop_send(const void *data, uint8_t len, uint8_t *channel);

/* Call once after reading packets from the radio: they arrived on the
 * current channel, so we move to the next one, like the sender did. */
void freq_hop_received(void);
