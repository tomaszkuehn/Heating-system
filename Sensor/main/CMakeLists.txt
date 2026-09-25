# main/ component: DS18x20 sensor node application.
#
# Self-contained ESP-IDF component: 1-Wire bit-banging driver, DS18B20
# temperature reader, LoRa UART manager, frame codec, and the main
# application task. No external managed components are required.

idf_component_register(
    SRCS
        "main.c"
        "onewire.c"
        "ds18b20.c"
        "lora.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        driver
        nvs_flash
    PRIV_REQUIRES
        esp_timer
)