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
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

static const char *TAG = "onewire";

static gpio_num_t s_pin = GPIO_NUM_NC;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Flag-gated write-slot diagnostics: when set, each write-1 slot samples the
 * line 10 us and 40 us after release so rise-time problems are visible. */
static bool s_wdiag;
static uint8_t s_wdiag_a;
static uint8_t s_wdiag_b;

/* ---- low-level pin helpers ----
 * Mode-switching scheme (mirrors the Arduino OneWire library, which the
 * proven sketch used on this exact hardware): drive = OUTPUT with level,
 * release = INPUT (High-Z, internal pull-up keeps the bus high). */
static inline void pin_output(void)
{
    gpio_set_direction(s_pin, GPIO_MODE_OUTPUT);
}

static inline void pin_input(void)
{
    gpio_set_direction(s_pin, GPIO_MODE_INPUT);
}

static inline void pin_low(void)
{
    gpio_set_level(s_pin, 0);
}

static inline void pin_high(void)
{
    gpio_set_level(s_pin, 1);
}

static inline int pin_read(void)
{
    return gpio_get_level(s_pin);
}

/* Precise microsecond delay that does not yield. esp_rom_delay_us does a
 * calibrated busy-wait from ROM (identical to delayMicroseconds on Arduino
 * ESP32); esp_timer_get_time() spin had jitter from cache misses. */
static inline void delay_us(uint32_t us)
{
    esp_rom_delay_us(us);
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
    pin_output();
    pin_low();
    delay_us(480);             /* RESET low pulse (>= 480 us) */
    pin_input();               /* release: High-Z + weak pull-up */
    delay_us(70);              /* slaves pull low within 15..60 us, hold 60..240 us */
    presence = (pin_read() == 0);
    portEXIT_CRITICAL(&s_lock);

    delay_us(410);              /* let presence pulse complete (total >= 480 us) */
    return presence;
}

/* Slot timing mirrors the proven OneWire (Paul Stoffregen) Arduino library
 * ESP32 implementation — the old DS18x20_Temperature.ino sketch worked with
 * it on this very hardware, so it is the reference for these constants. */
void ow_write_bit(bool bit)
{
    portENTER_CRITICAL(&s_lock);
    pin_output();
    pin_low();
    delay_us(10);              /* write-1: low 10 us, write-0: low 70 us */
    if (bit) {
        pin_input();           /* release early for a 1 */
        if (s_wdiag) {
            delay_us(10); s_wdiag_a = (uint8_t)pin_read();
            delay_us(30); s_wdiag_b = (uint8_t)pin_read();
        }
        portEXIT_CRITICAL(&s_lock);
        delay_us(s_wdiag ? 20 : 60);   /* rest of the 70 us slot + recovery */
    } else {
        portEXIT_CRITICAL(&s_lock);
        delay_us(60);          /* total low 70 us */
        pin_input();           /* release + recovery */
        delay_us(10);
    }
}

bool ow_read_bit(void)
{
    bool bit;

    portENTER_CRITICAL(&s_lock);
    pin_output();
    pin_low();
    delay_us(2);               /* start read slot */
    pin_input();               /* release, let the slave drive */
    delay_us(11);              /* sample at ~13 us, inside the 15 us window */
    bit = (pin_read() != 0);
    portEXIT_CRITICAL(&s_lock);
    delay_us(57);              /* rest of the 70 us slot + recovery */
    return bit;
}

void ow_write_byte(uint8_t byte)
{
    for (int i = 0; i < 8; i++) {
        ow_write_bit((byte >> i) & 1);   /* LSB first */
    }
}

/* Run one READ ROM transaction with write-slot rise-time diagnostics.
 * Logs the command bits as seen on the wire (sampled 10/40 us after release)
 * and the 8-byte response. Called from ow_search as a wiring probe. */
void ow_readrom_diag(void)
{
    s_wdiag = true;
    s_wdiag_a = 0; s_wdiag_b = 0;
    if (!ow_reset()) {
        ESP_LOGW(TAG, "READROM: no presence");
        s_wdiag = false;
        return;
    }
    ow_write_byte(0x33);
    uint8_t cmd_hi10 = s_wdiag_a, cmd_hi40 = s_wdiag_b;
    uint8_t rr[8];
    for (int i = 0; i < 8; i++) rr[i] = ow_read_byte();
    ESP_LOGI(TAG, "READROM: %02x %02x %02x %02x %02x %02x %02x %02x  cmdHi10=%u cmdHi40=%u",
             rr[0], rr[1], rr[2], rr[3], rr[4], rr[5], rr[6], rr[7], cmd_hi10, cmd_hi40);
    s_wdiag = false;
}

/* One-shot diagnostic: pulse a RESET on every plausible free GPIO and report
 * which pins show a presence-like low. Settles the "is the probe really on
 * the pin we think?" question in a single boot. Skips radio pins
 * (12/14/25/26), flash pins (6-11), strapping 0/2 and UART0 1/3. */
void ow_pin_sweep(void)
{
    static const gpio_num_t pins[] = {
        GPIO_NUM_32, GPIO_NUM_33, GPIO_NUM_27, GPIO_NUM_13,
        GPIO_NUM_4,  GPIO_NUM_5,  GPIO_NUM_18, GPIO_NUM_19,
        GPIO_NUM_21, GPIO_NUM_22, GPIO_NUM_23, GPIO_NUM_16, GPIO_NUM_17,
    };
    gpio_num_t saved = s_pin;
    ESP_LOGW(TAG, "SWEEP: probing %d pins for 1-Wire presence...",
             (int)(sizeof(pins) / sizeof(pins[0])));
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        s_pin = pins[i];
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << pins[i]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        vTaskDelay(pdMS_TO_TICKS(2));
        int p70, p200;
        portENTER_CRITICAL(&s_lock);
        pin_output();
        pin_low();
        delay_us(480);
        pin_input();
        delay_us(70);  p70  = pin_read();
        delay_us(130); p200 = pin_read();
        portEXIT_CRITICAL(&s_lock);
        delay_us(300);
        if (p70 == 0)
            ESP_LOGW(TAG, "SWEEP: GPIO%d presence-like low @70us (level@200us=%d)",
                     pins[i], p200);
        else
            ESP_LOGI(TAG, "SWEEP: GPIO%d no presence", pins[i]);
        gpio_reset_pin(pins[i]);
    }
    s_pin = saved;
    if (saved != GPIO_NUM_NC) ow_init(saved);
    ESP_LOGW(TAG, "SWEEP: done");
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

    /* Diagnostics: report whether any slave pulses presence on the bus. */
    if (!ow_reset()) {
        ESP_LOGW(TAG, "search: no presence on reset (wiring/pull-up?)");
        return 0;
    }
    /* Read ROM probe: a single device answers 0x33 with its 8-byte ROM.
     * Logged raw so wiring/timing issues are visible in the console. */
    vTaskDelay(pdMS_TO_TICKS(5));   /* bus settle between back-to-back resets */
    ow_readrom_diag();
    vTaskDelay(pdMS_TO_TICKS(5));
    last_device = false;

    while (!last_device && found < max) {
        if (!ow_reset()) break;

        ow_write_byte(OW_CMD_SEARCH_ROM);

        int last_zero = 0;
        int abort_bit = 0;
        uint8_t dbg[8] = {0};   /* first 8 read-bit pairs for diagnostics */
        for (int bit = 1; bit <= 64; bit++) {
            bool bit_a = ow_read_bit();   /* un-inverted bit */
            bool bit_b = ow_read_bit();   /* complement    */
            if (bit <= 8) dbg[bit - 1] = (uint8_t)((bit_a ? 2 : 0) | (bit_b ? 1 : 0));

            int rom_idx = (bit - 1) / 8;
            int rom_bit = (bit - 1) % 8;

            if (bit_a && bit_b) {
                /* No device responded — abort. */
                last_device = true;
                abort_bit = bit;
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

        if (last_device) {
            ESP_LOGW(TAG, "search: no-response abort at ROM bit %d, first8=%u%u%u%u%u%u%u%u",
                     abort_bit ? abort_bit : 64,
                     dbg[0], dbg[1], dbg[2], dbg[3], dbg[4], dbg[5], dbg[6], dbg[7]);
            break;
        }

        ESP_LOGI(TAG, "search: rom=%02x%02x%02x%02x%02x%02x%02x crc=%02x (exp %02x)",
                 rom[0], rom[1], rom[2], rom[3], rom[4], rom[5], rom[6],
                 ow_crc8(rom, 7), rom[7]);
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