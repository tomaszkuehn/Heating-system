/**
 * DS18x20 sensor node application entry point.
 *
 * Discovers DS18B20 probes on the 1-Wire bus, reads temperatures on a
 * fixed cycle, packages them into text lines ("TT.TTT T<id>&") and sends
 * them over LoRa. A watchdog guards the whole loop; a status counter
 * drives power management and forced reboots exactly like the original
 * sketch, but non-blocking and with esp_restart() instead of a 20 s delay.
 */
#include "app_config.h"
#include "onewire.h"
#include "ds18b20.h"
#include "lora.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"

static const char *TAG = "main";

static ds18b20_reading_t g_probes[HE_MAX_SENSORS];
static int g_probe_count = 0;

/* ---- Paired radio id (1..6, 0 = unpaired / awaiting assignment) ----
 * Persisted in NVS under "node_id". An unpaired node announces itself with
 * frames "TT.TTT T00&" and listens for "PAIR <n>&" from the controller; on
 * receipt it stores the id and switches to normal operation. */
static uint8_t g_node_id = 0;

static void node_id_load(void)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, "node_id", &v) == ESP_OK && v >= 1 && v <= HE_MAX_SENSORS)
            g_node_id = v;
        nvs_close(h);
    }
    ESP_LOGI(TAG, "paired node id: %d", g_node_id);
}

static bool node_id_save(uint8_t id)
{
    if (id < 1 || id > HE_MAX_SENSORS) return false;
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_u8(h, "node_id", id);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK) g_node_id = id;
    return e == ESP_OK;
}

/* Parse "PAIR <n>" (controller -> node). Returns id 1..6 or 0 if not a PAIR. */
static int parse_pair(const char *line)
{
    if (strncmp(line, "PAIR", 4) != 0) return 0;
    const char *p = line + 4;
    while (*p == ' ') p++;
    int n = atoi(p);
    return (n >= 1 && n <= HE_MAX_SENSORS) ? n : 0;
}

/* ---- Factory reset: hold BOOT (GPIO0) for >=5 s ----
 * Reaching the threshold lights the on-board LED (GPIO2) as a visible
 * "reset armed" hint; the id is erased and the node reboots AFTER the
 * button is released (so the user sees the LED before the reboot). */
static void factory_reset_check(void)
{
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << HE_GPIO_RESET_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&in);

    int held = 0;
    bool armed = false;
    for (;;) {
        if (gpio_get_level(HE_GPIO_RESET_BTN) != 0) {
            /* Released. */
            if (armed) {
                /* Threshold was reached while held: erase + reboot now. */
                ESP_LOGW(TAG, "button held %d s — factory reset (erasing node_id)", held);
                nvs_handle_t h;
                if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) {
                    nvs_erase_key(h, "node_id");
                    nvs_commit(h);
                    nvs_close(h);
                }
                gpio_set_level(HE_GPIO_RESET_LED, 0);   /* LED off */
                vTaskDelay(pdMS_TO_TICKS(200));         /* let logs flush */
                esp_restart();
            }
            return;
        }
        /* Still held. */
        held++;
        if (held >= HE_RESET_HOLD_SEC && !armed) {
            /* Light the LED: reset armed, keep watching until release. */
            gpio_config_t led = {
                .pin_bit_mask = 1ULL << HE_GPIO_RESET_LED,
                .mode = GPIO_MODE_OUTPUT,
                .pull_up_en   = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type    = GPIO_INTR_DISABLE,
            };
            gpio_config(&led);
            gpio_set_level(HE_GPIO_RESET_LED, 1);
            ESP_LOGW(TAG, "reset armed (LED on) — release BOOT to apply");
            armed = true;
        }
        esp_task_wdt_reset();   /* holding the button may take long */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* Parse "REPAIR <old> <new>" (controller -> node): addressed re-pairing.
 * Only the node whose current radio id == old accepts it. Returns the new
 * id (1..6) or 0. */
static int parse_repair(const char *line, uint8_t current_id)
{
    if (strncmp(line, "REPAIR", 6) != 0) return 0;
    const char *p = line + 6;
    while (*p == ' ') p++;
    long old_id = strtol(p, (char **)&p, 10);
    if (old_id != (long)current_id) return 0;   /* not for this node */
    while (*p == ' ') p++;
    int n = atoi(p);
    return (n >= 1 && n <= HE_MAX_SENSORS && n != current_id) ? n : 0;
}

/* Detect "RESET&" (controller -> node): erase the paired id and reboot
 * into unpaired (factory-default) mode. Returns true on match. */
static bool is_reset_cmd(const char *line)
{
    return strncmp(line, "RESET", 5) == 0;
}

/* Erase the persisted radio id (factory default = unpaired T00). */
static void node_id_erase(void)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "node_id");
    nvs_commit(h);
    nvs_close(h);
    g_node_id = 0;
}

/* Unpaired mode: announce with T00 frames, wait for PAIR assignment. */
static void pairing_loop(void)
{
    char line[HE_LORA_LINE_MAX];
    for (;;) {
        esp_task_wdt_reset();

        if (g_probe_count == 0)
            g_probe_count = ds18b20_enumerate(g_probes, HE_MAX_SENSORS);
        ds18b20_read_all(g_probes, g_probe_count);

        float t = (g_probe_count > 0 && g_probes[0].centi != INT16_MIN)
                      ? g_probes[0].centi / 100.0f : 0.0f;
        char frame[24];
        snprintf(frame, sizeof(frame), "%02.3f T00&", t);
        ESP_LOGI(TAG, "unpaired, announcing: %s", frame);
        lora_write((const uint8_t *)frame, strlen(frame));
        lora_write((const uint8_t *)"\r\n", 2);

        /* Wait for the controller's assignment. */
        int rc = lora_receive_line(HE_LORA_RX_WINDOW_MS, line, sizeof(line));
        if (rc > 0) {
            if (is_reset_cmd(line)) continue;   /* already factory default */
            int new_id = parse_pair(line);
            if (new_id > 0 && node_id_save((uint8_t)new_id)) {
                char ack[16];
                snprintf(ack, sizeof(ack), "OK T%02d&", new_id);
                lora_write((const uint8_t *)ack, strlen(ack));
                lora_write((const uint8_t *)"\r\n", 2);
                ESP_LOGI(TAG, "paired as T%02d", new_id);
                return;   /* back to normal operation */
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---- ACK handling (non-blocking receive window) ---- */
typedef struct {
    bool got_ack;     /* 'X' seen from controller */
} rx_ctx_t;

static bool on_byte(uint8_t b, void *user)
{
    rx_ctx_t *ctx = (rx_ctx_t *)user;
    fputc(b, stdout);          /* mirror to console like the original */
    if (b == 'X') {
        ctx->got_ack = true;
        return true;
    }
    return false;
}

static int check_message(int status)
{
    /* If we're in a low-power window and a fresh ACK arrives, go back to high. */
    if (status > 30 && status < 40 && lora_power_status() != HE_LORA_PWR_HIGH) {
        lora_set_power(HE_LORA_PWR_HIGH);
    }
    ESP_LOGI(TAG, "ACK confirmed");
    return HE_STATUS_OK;
}

static void send_temperature_frame(void)
{
    /* Build one text line per probe: "TT.TTT T<id>&", matching the
     * original sketch format (single-probe variant was "%02.3f T01&").
     * The radio id is the paired node id + probe index (a node can carry
     * several probes on one bus: base, base+1, ...). */
    int sent = 0;
    for (int i = 0; i < g_probe_count; i++) {
        int16_t c = g_probes[i].centi;
        if (c == INT16_MIN) {
            ESP_LOGW(TAG, "probe %d read failure", g_probes[i].id);
            continue;
        }
        float t = c / 100.0f;
        if (t < HE_TEMP_MIN_LOGICAL || t > HE_TEMP_MAX_LOGICAL) {
            ESP_LOGW(TAG, "probe %d out of range: %.2f C", g_probes[i].id, t);
            continue;
        }

        char line[20];
        int rid = g_node_id + i;   /* radio id = paired base + probe index */
        if (rid > HE_MAX_SENSORS) rid = HE_MAX_SENSORS;
        snprintf(line, sizeof(line), "%02.3f T%02d&", t, rid);
        ESP_LOGI(TAG, "T%d = %.2f C", rid, t);
        lora_write((const uint8_t *)line, strlen(line));
        lora_write((const uint8_t *)"\r\n", 2);
        sent++;
    }

    if (sent == 0) {
        ESP_LOGW(TAG, "no valid probes — sending ERR marker");
        const char *err = "ERR T&\r\n";
        lora_write((const uint8_t *)err, strlen(err));
    }
}

/* ---- Main task ---- */
static void sensor_task(void *arg)
{
    static int status = HE_STATUS_OK;

    for (;;) {
        esp_task_wdt_reset();

        /* 1-Wire: discover + read. */
        if (g_probe_count == 0) {
            g_probe_count = ds18b20_enumerate(g_probes, HE_MAX_SENSORS);
        }
        ds18b20_read_all(g_probes, g_probe_count);

        send_temperature_frame();

        /* LoRa ACK window (non-blocking). */
        status--;
        ESP_LOGI(TAG, "status=%d pwr=%d", status, lora_power_status());

        /* Listen for ACK; also handle re-pairing commands sent to a
         * paired node ("PAIR <n>&" changes the radio id in place). */
        rx_ctx_t ctx = { .got_ack = false };
        char cmd[HE_LORA_LINE_MAX];
        int rc = lora_receive_line(HE_LORA_RX_WINDOW_MS, cmd, sizeof(cmd));
        if (rc > 0) {
            if (is_reset_cmd(cmd)) {
                ESP_LOGW(TAG, "RESET command — factory default");
                node_id_erase();
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
            /* A paired node keeps its id: only a REPAIR addressed to it may
             * change it. Plain "PAIR <n>" is for unpaired (T00) nodes only
             * and is ignored here. */
            int new_id = parse_repair(cmd, g_node_id);
            if (new_id > 0 && new_id != g_node_id && node_id_save((uint8_t)new_id)) {
                char ack[16];
                snprintf(ack, sizeof(ack), "OK T%02d&", new_id);
                lora_write((const uint8_t *)ack, strlen(ack));
                lora_write((const uint8_t *)"\r\n", 2);
                ESP_LOGI(TAG, "re-paired as T%02d", new_id);
            } else {
                /* Not a PAIR command: honour plain ACK bytes ('X'). */
                for (int k = 0; k < rc; k++)
                    if (cmd[k] == 'X') { ctx.got_ack = true; break; }
            }
        }
        if (ctx.got_ack) {
            status = check_message(status);
        }

        /* Power management: drop to low power when link degrades. */
        if (status == HE_STATUS_PWR_DOWN) {
            lora_set_power(HE_LORA_PWR_LOW);
        }

        /* Last resort: force a reboot. */
        if (status == HE_STATUS_REBOOT) {
            ESP_LOGW(TAG, "link dead — rebooting");
            status = HE_STATUS_OK;
            vTaskDelay(pdMS_TO_TICKS(200));   /* let logs flush */
            esp_restart();
        }

        /* Factory reset check: EN/BOOT held for >5 s erases the id. */
        factory_reset_check();

        /* Period between cycles. */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---- Setup ---- */
void app_main(void)
{
    ESP_LOGI(TAG, "DS18x20 sensor node starting (boot %llu ms)",
             (unsigned long long)(esp_timer_get_time() / 1000));

    /* Watchdog. */
    esp_task_wdt_deinit();
    esp_task_wdt_config_t wdt = {
        .timeout_ms     = HE_WDT_TIMEOUT_SEC * 1000,
        .idle_core_mask  = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic   = true,
    };
    ESP_ERROR_CHECK(esp_task_wdt_init(&wdt));
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    /* NVS (paired node id). */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND ||
        nvs_err == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    node_id_load();

    /* Factory reset: BOOT (GPIO0) held for >5 s lights the LED, then erases
     * the id on release and reboots unpaired. */
    factory_reset_check();

    /* 1-Wire bus. */
    ow_init(HE_GPIO_ONEWIRE);

    /* LoRa radio. */
    lora_init();

    /* Unpaired node: block in the pairing loop until the controller
     * assigns an id. Sensor task starts only after pairing. */
    if (g_node_id == 0) pairing_loop();

    /* Run the sensor loop on its own task so app_main can return. */
    xTaskCreate(sensor_task, "sensor", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "ready");
}