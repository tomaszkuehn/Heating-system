/**
 * 1-Wire bit-banging driver (ESP-IDF), DS18B20-compatible.
 *
 * The bus master owns a single GPIO. To avoid an external pull-up resistor
 * dependency, the pin is configured open-drain: a write of 0 actively
 * pulls low, a write of 1 releases the line (the internal weak pull-up
 * plus any external 4k7 pulls it high). Reads are done by switching the
 * pin to input for the sample window.
 *
 * Timing is derived from esp_timer_get_time() (microsecond resolution),
 * with short critical sections guarded by a portMUX_TYPE to keep the
 * slots jitter-free on a dual-core ESP32. The whole RESET+slot sequence
 * is short enough (<= 1 ms) that masking interrupts across it is not
 * harmful, but we only mask across the <70 us critical windows to keep
 * the system responsive.
 */
#include "onewire.h"
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static const char *TAG = "onewire";

static gpio_num_t s_pin = GPIO_NUM_NC;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* ---- low-level pin helpers ---- */
static inline void pin_low(void)
{
    gpio_set_level(s_pin, 0);
}

static inline void pin_high(void)
{
    /* Open-drain: write 1 to release the line. */
    gpio_set_level(s_pin, 1);
}

static inline int pin_read(void)
{
    return gpio_get_level(s_pin);
}

/* Precise microsecond delay that does not yield. */
static inline void delay_us(uint32_t us)
{
    int64_t end = esp_timer_get_time() + us;
    while (esp_timer_get_time() < end) { /* spin */ }
}

void ow_init(gpio_num_t pin)
{
    s_pin = pin;
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,   /* open-drain: 0 drives low, 1 floats */
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* internal weak pull-up (4k7 ext. recommended) */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t e = gpio_config(&io);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(e));
    }
    pin_high();
    ESP_LOGI(TAG, "1-Wire on GPIO%d", pin);
}

bool ow_reset(void)
{
    bool presence = false;

    portENTER_CRITICAL(&s_lock);
    pin_low();
    delay_us(480);             /* RESET low pulse (>= 480 us) */
    pin_high();
    delay_us(70);              /* slaves pull low within 15..60 us, hold 60..240 us */
    presence = (pin_read() == 0);
    portEXIT_CRITICAL(&s_lock);

    delay_us(410);              /* let presence pulse complete (total >= 480 us) */
    return presence;
}

void ow_write_bit(bool bit)
{
    portENTER_CRITICAL(&s_lock);
    if (bit) {
        /* Write-1: pull low <= 15 us, then release for the rest of the slot. */
        pin_low();
        delay_us(6);
        pin_high();
        portEXIT_CRITICAL(&s_lock);
        delay_us(64);          /* rest of the 70 us slot + recovery */
    } else {
        /* Write-0: hold low 60..120 us. */
        pin_low();
        delay_us(60);
        portEXIT_CRITICAL(&s_lock);
        delay_us(10);          /* recovery */
    }
}

bool ow_read_bit(void)
{
    bool bit;

    portENTER_CRITICAL(&s_lock);
    pin_low();
    delay_us(6);               /* master pulls low to start read slot */
    pin_high();
    delay_us(9);               /* wait for slave to drive, sample near 15 us */
    bit = (pin_read() != 0);
    portEXIT_CRITICAL(&s_lock);
    delay_us(55);              /* rest of the 70 us slot + recovery */
    return bit;
}

void ow_write_byte(uint8_t byte)
{
    for (int i = 0; i < 8; i++) {
        ow_write_bit((byte >> i) & 1);   /* LSB first */
    }
}

uint8_t ow_read_byte(void)
{
    uint8_t byte = 0;
    for (int i = 0; i < 8; i++) {
        if (ow_read_bit()) byte |= (1 << i);
    }
    return byte;
}

void ow_skip_rom(void)    { ow_write_byte(OW_CMD_SKIP_ROM); }
void ow_match_rom(const uint8_t rom[8])
{
    ow_write_byte(OW_CMD_MATCH_ROM);
    for (int i = 0; i < 8; i++) ow_write_byte(rom[i]);
}

/* ---- CRC8, poly 0x31, init 0x00 (Dallas/Maxim 1-Wire CRC) ----
 * Note: this is the *ROM* CRC, distinct from the scratchpad CRC which uses
 * the same polynomial. Both are computed the same way. */
uint8_t ow_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        for (int j = 0; j < 8; j++) {
            uint8_t mix = (crc ^ b) & 0x01;
            crc >>= 1;
            if (mix) crc ^= 0x8C;
            b >>= 1;
        }
    }
    return crc;
}

bool ow_is_ds18b20(const ow_rom_t *r)
{
    /* Family code 0x28 = DS18B20. */
    return r && (r->rom[0] == 0x28);
}

/* ---- ROM search (binary tree walk) ----
 * Implements the standard last-discrepancy algorithm. */
int ow_search(ow_rom_t *out, int max)
{
    if (max <= 0) return 0;

    int found = 0;
    int last_discrepancy = 0;
    uint8_t rom[8] = {0};
    bool last_device = false;

    while (!last_device && found < max) {
        if (!ow_reset()) break;

        ow_write_byte(OW_CMD_SEARCH_ROM);

        int last_zero = 0;
        for (int bit = 1; bit <= 64; bit++) {
            bool bit_a = ow_read_bit();   /* un-inverted bit */
            bool bit_b = ow_read_bit();   /* complement    */

            int rom_idx = (bit - 1) / 8;
            int rom_bit = (bit - 1) % 8;

            if (bit_a && bit_b) {
                /* No device responded — abort. */
                last_device = true;
                break;
            }
            if (!bit_a && !bit_b) {
                /* Discrepancy: choose path. */
                if (bit == last_discrepancy) {
                    rom[rom_idx] |= (1 << rom_bit);   /* take 1 */
                } else if (bit > last_discrepancy) {
                    rom[rom_idx] &= ~(1 << rom_bit);  /* take 0 */
                    last_zero = bit;
                } else {
                    /* Follow existing ROM bit. */
                    if ((rom[rom_idx] >> rom_bit) & 1) {
                        /* keep 1 */
                    } else {
                        last_zero = bit;
                    }
                }
            } else {
                /* All devices agree on this bit. */
                if (bit_a) rom[rom_idx] |= (1 << rom_bit);
                else       rom[rom_idx] &= ~(1 << rom_bit);
            }
            /* Write the chosen bit to direct the search. */
            ow_write_bit((rom[rom_idx] >> rom_bit) & 1);
        }

        if (last_device) break;

        /* Validate ROM CRC. */
        if (ow_crc8(rom, 7) == rom[7]) {
            memcpy(out[found].rom, rom, 8);
            found++;
        }

        if (last_zero == 0) {
            last_device = true;
        } else {
            last_discrepancy = last_zero;
        }
    }
    return found;
}