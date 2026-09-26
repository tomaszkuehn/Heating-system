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
#include "lora_sec.h"

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

/* Parse "PAIR <id> <nonce> <tag>" (controller -> node). Verifies the
 * HMAC tag (computed over "PAIR <id> <nonce>" — the part before the last
 * space) and returns the requested id (1..6) or 0 when absent/invalid. */
static int parse_pair(const char *line)
{
    char *sp1 = strchr(line, ' ');
    if (!sp1) return 0;
    char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2) return 0;
    char *sp3 = strchr(sp2 + 1, ' ');
    if (!sp3) return 0;
    if (strncmp(line, "PAIR", 4) != 0) return 0;
    int n = atoi(sp1 + 1);
    if (n < 1 || n > HE_MAX_SENSORS) return 0;
    /* msg excludes the tag: truncate at the last space. */
    *sp3 = '\0';
    bool ok = lora_sec_pair_verify(line, sp3 + 1);
    *sp3 = ' ';
    if (!ok) {
        ESP_LOGW(TAG, "PAIR rejected: bad MAC");
        return 0;
    }
    return n;
}

/* ---- Replay protection: nonce ring in NVS ----
 * Every accepted PAIR consumes one 32-bit nonce; the last 32 are stored
 * so a captured frame cannot be replayed even across node reboots. */
static bool nonce_seen_or_store(uint32_t nonce)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) return true;
    uint8_t ring[128];
    size_t len = sizeof(ring);
    int count = 0;
    if (nvs_get_blob(h, "pair_nv", ring, &len) == ESP_OK && len % 4 == 0)
        count = (int)(len / 4);
    for (int i = 0; i < count; i++) {
        uint32_t v = (uint32_t)ring[i * 4] | ((uint32_t)ring[i * 4 + 1] << 8) |
                     ((uint32_t)ring[i * 4 + 2] << 16) | ((uint32_t)ring[i * 4 + 3] << 24);
        if (v == nonce) { nvs_close(h); return true; }   /* replay */
    }
    int keep = (count < 32) ? count + 1 : 32;
    /* Newest first: shift the ring right, drop the oldest when full. */
    memmove(&ring[4], ring, (size_t)(keep - 1) * 4);
    ring[0] = (uint8_t)(nonce & 0xff);
    ring[1] = (uint8_t)((nonce >> 8) & 0xff);
    ring[2] = (uint8_t)((nonce >> 16) & 0xff);
    ring[3] = (uint8_t)((nonce >> 24) & 0xff);
    esp_err_t e = nvs_set_blob(h, "pair_nv", ring, (size_t)keep * 4);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e != ESP_OK;   /* storage failure: reject rather than trust */
}

/* Extract the 32-bit nonce from "PAIR <id> <nonce8hex> <tag>". */
static bool pair_nonce(const char *line, uint32_t *out)
{
    char *sp1 = strchr(line, ' ');
    if (!sp1) return false;
    char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2) return false;
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        char c = sp2[1 + i];
        int d = (c >= '0' && c <= '9') ? c - '0' :
                (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return true;
}

static void led_init(void)
{
    gpio_config_t led = {
        .pin_bit_mask = 1ULL << HE_GPIO_RESET_LED,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&led);
    gpio_set_level(HE_GPIO_RESET_LED, 0);
}

static void node_id_erase(void);   /* defined below */

/* ---- Factory reset, always-on: hold BOOT (GPIO0) for >=5 s ----
 * Runs as its own task so it works in every state (unpaired pairing_loop,
 * paired sensor_task, even while blocked in a LoRa receive window).
 * On reaching the threshold the on-board LED (GPIO2) lights up and the
 * paired id is erased; the node then reboots (id 0 / T00) on release. */
static void reset_button_task(void *arg)
{
    esp_task_wdt_add(NULL);
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << HE_GPIO_RESET_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&in);

    int held_ms = 0;
    bool armed = false;
    for (;;) {
        esp_task_wdt_reset();
        if (gpio_get_level(HE_GPIO_RESET_BTN) != 0) {
            /* Released. */
            if (armed) {
                ESP_LOGW(TAG, "BOOT released — restarting with id 0");
                gpio_set_level(HE_GPIO_RESET_LED, 0);
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
            held_ms = 0;
        } else {
            held_ms += 50;
            if (!armed && held_ms >= HE_RESET_HOLD_SEC * 1000) {
                ESP_LOGW(TAG, "BOOT held %d s — factory reset (erasing id)",
                         HE_RESET_HOLD_SEC);
                node_id_erase();
                gpio_set_level(HE_GPIO_RESET_LED, 1);
                armed = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
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
    esp_task_wdt_add(NULL);
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
            ESP_LOGI(TAG, "rx frame: \"%s\"", line);
            uint32_t nonce = 0;
            int new_id = parse_pair(line);
            if (new_id > 0 && !pair_nonce(line, &nonce)) new_id = 0;
            if (new_id > 0 && nonce_seen_or_store(nonce) && node_id_save((uint8_t)new_id)) {
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

/* ---- ACK handling ---- */
typedef struct {
    bool got_ack;     /* "ACK <this-id>" frame seen from controller */
} rx_ctx_t;

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
    esp_task_wdt_add(NULL);

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

        /* Listen for the addressed ACK frame "ACK <id>&". Only a frame
         * carrying THIS node's radio id counts as a link confirmation. */
        rx_ctx_t ctx = { .got_ack = false };
        char cmd[HE_LORA_LINE_MAX];
        int rc = lora_receive_line(HE_LORA_RX_WINDOW_MS, cmd, sizeof(cmd));
        if (rc > 0) {
            int acked = -1;
            if (strncmp(cmd, "ACK", 3) == 0 && cmd[3] == ' ')
                acked = atoi(cmd + 4);
            if (acked == g_node_id) {
                ctx.got_ack = true;
            } else if (acked >= 0) {
                ESP_LOGI(TAG, "ACK for other node (T%02d), ignored", acked);
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

        /* Period between cycles. */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---- Setup ---- */
void app_main(void)
{
    ESP_LOGI(TAG, "DS18x20 sensor node starting (boot %llu ms)",
             (unsigned long long)(esp_timer_get_time() / 1000));

    /* Watchdog. Only the worker tasks that actually do work subscribe
     * themselves (esp_task_wdt_add(NULL)); idle cores are NOT watched, so a
     * busy LoRa receive window can never trip a false reset. */
    esp_task_wdt_deinit();
    esp_task_wdt_config_t wdt = {
        .timeout_ms     = HE_WDT_TIMEOUT_SEC * 1000,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    ESP_ERROR_CHECK(esp_task_wdt_init(&wdt));

    /* NVS (paired node id). */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND ||
        nvs_err == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    node_id_load();

    /* LED self-test: blink 3x so the pin/level can be confirmed visually. */
    led_init();
    for (int i = 0; i < 3; i++) {
        gpio_set_level(HE_GPIO_RESET_LED, 1);
        vTaskDelay(pdMS_TO_TICKS(150));
        gpio_set_level(HE_GPIO_RESET_LED, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    /* Always-on factory reset: BOOT held >=5 s erases the id + lights the
     * LED, release reboots into T00. Independent of pairing/LoRa state. */
    xTaskCreate(reset_button_task, "reset_btn", 3072, NULL, 6, NULL);

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