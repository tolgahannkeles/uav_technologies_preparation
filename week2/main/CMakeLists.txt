idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES nrf24 link_crypto esp_driver_uart nvs_flash)
