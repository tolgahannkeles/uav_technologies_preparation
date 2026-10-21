#include "freq_hop.h"

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nrf24.h"

static const char *TAG = "freq_hop";

/* Random wait before searching other channels, so two boards that sent at
 * the same moment (and both failed) do not collide again. */
#define BACKOFF_MAX_MS  30

static const uint8_t *s_channels;   /* the hop table */
static size_t s_count;              /* number of channels in it */
static size_t s_index;              /* where we are in the table */

/* Tune the radio to table entry `index` */
static void tune(size_t index)
{
    nrf24_set_channel(s_channels[index]);
}

void freq_hop_init(const uint8_t *channels, size_t count)
{
    s_channels = channels;
    s_count = count;
    s_index = 0;                /* both boards start at the first entry */
    tune(s_index);
    ESP_LOGI(TAG, "%s, %u channel(s), starting on ch %u",
             count > 1 ? "hopping on" : "hopping off", (unsigned)count, channels[0]);
}

uint8_t freq_hop_channel(void)
{
    return s_channels[s_index];
}

esp_err_t freq_hop_send(const void *data, uint8_t len, uint8_t *channel)
{
    esp_err_t err = ESP_FAIL;

    /* Try the current channel first. If the peer does not answer, it is
     * probably on another entry of the table: try them one by one. */
    for (size_t attempt = 0; attempt < s_count; attempt++) {
        size_t index = (s_index + attempt) % s_count;
        tune(index);
        err = nrf24_send(data, len, NULL);

        if (err == ESP_OK) {
            /* Delivered on table entry `index`. The receiver now moves to
             * index + 1, so we do the same. */
            if (channel) {
                *channel = s_channels[index];
            }
            if (attempt > 0) {
                ESP_LOGI(TAG, "peer found on ch %u, back in sync", s_channels[index]);
            }
            s_index = (index + 1) % s_count;
            break;
        }
        if (err == ESP_ERR_TIMEOUT) {
            break;              /* radio problem: other channels won't help */
        }
        if (attempt == 0 && s_count > 1) {
            ESP_LOGI(TAG, "no answer on ch %u, searching the other channels...",
                     s_channels[index]);
            vTaskDelay(pdMS_TO_TICKS(esp_random() % BACKOFF_MAX_MS) + 1);
        }
    }

    /* Back to receiving, on the channel we expect the next message on */
    tune(s_index);
    nrf24_start_listening();
    return err;
}

void freq_hop_received(void)
{
    s_index = (s_index + 1) % s_count;
    tune(s_index);              /* nrf24_set_channel keeps RX mode on */
}
