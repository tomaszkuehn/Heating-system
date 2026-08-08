#include "ds18b20.h"
#include "app_config.h"

#include <string.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "ds18b20";

/* Wait for conversion completion (bus returns high) with bounded timeout. */
static bool wait_conversion(uint32_t timeout_us)
{
    int64_t end = esp_timer_get_time() + timeout_us;
    while (esp_timer_get_time() < end) {
        /* During conversion the slave holds the bus low; when done, read 1. */
        if (ow_read_bit()) return true;
        int64_t tick_end = esp_timer_get_time() + 1000;
        while (esp_timer_get_time() < tick_end) { /* spin 1 ms */ }
    }
    return false;
}

int ds18b20_enumerate(ds18b20_reading_t *out, int max)
{
    ow_rom_t roms[HE_MAX_SENSORS];
    int n = ow_search(roms, max > HE_MAX_SENSORS ? HE_MAX_SENSORS : max);
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (!ow_is_ds18b20(&roms[i])) {
            ESP_LOGW(TAG, "non-DS18B20 family 0x%02X on bus, skipping", roms[i].rom[0]);
            continue;
        }
        out[count].id    = (uint8_t)(count + 1);
        out[count].rom   = roms[i];
        out[count].centi = INT16_MIN;
        count++;
    }
    ESP_LOGI(TAG, "enumerated %d DS18B20 probe(s)", count);
    return count;
}

bool ds18b20_request_all(void)
{
    if (!ow_reset()) {
        ESP_LOGW(TAG, "no presence on RESET (request_all)");
        return false;
    }
    ow_skip_rom();
    ow_write_byte(DS18B20_CMD_CONVERT_T);
    /* 12-bit conversion: up to 750 ms. Poll the bus for completion. */
    return wait_conversion(800000);
}

int16_t ds18b20_read(const ow_rom_t *rom)
{
    if (!rom) return INT16_MIN;

    if (!ow_reset()) return INT16_MIN;
    ow_match_rom(rom->rom);
    ow_write_byte(DS18B20_CMD_READ_SCRATCH);

    /* Scratchpad: 9 bytes (LSB, MSB, ... config, CRC). */
    uint8_t sp[9];
    for (int i = 0; i < 9; i++) sp[i] = ow_read_byte();

    /* Verify scratchpad CRC8 (poly 0x31, same as ROM CRC). */
    if (ow_crc8(sp, 8) != sp[8]) {
        ESP_LOGW(TAG, "scratchpad CRC mismatch");
        return INT16_MIN;
    }

    int16_t raw = (int16_t)((sp[1] << 8) | sp[0]);
    /* 12-bit default: raw is already in 1/16 C. Convert to centi-degrees. */
    return (int16_t)(raw * 100 / 16);
}

int ds18b20_read_all(ds18b20_reading_t *out, int count)
{
    if (!ds18b20_request_all()) {
        for (int i = 0; i < count; i++) out[i].centi = INT16_MIN;
        return 0;
    }
    int ok = 0;
    for (int i = 0; i < count; i++) {
        int16_t c = ds18b20_read(&out[i].rom);
        out[i].centi = c;
        if (c != INT16_MIN) ok++;
    }
    return ok;
}