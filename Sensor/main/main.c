/**
 * DS18x20 sensor node application entry point.
 *
 * Discovers DS18B20 probes on the 1-Wire bus, reads temperatures on a fixed
 * cycle and sends them over LoRa in the binary protocol of LoRa.txt:
 * "<0x02 0xE3><payload>T<id>#", where the payload is the temperature
 * rendered as "%02.3f" or "ERR" for a probe failure. The controller (radio
 * id 9) acknowledges each measurement with an "X<id>" frame and pairs
 * factory-id-0 nodes with "PR<id>".
 *
 * A watchdog guards the whole loop. Per LoRa.txt: an ACK missing for 2 s
 * raises the radio output power and the same frame is repeated once; a
 * successful ACK lowers the power back to standard; a link that stays
 * unacknowledged for too long forces a cyclic reboot. Hold BOOT >=5 s to
 * erase the paired id (factory reset, LED hint).
 */
#include "app_config.h"
#include "onewire.h"
#include "ds18b20.h"
#include "lora.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"

static const char *TAG = "main";

static ds18b20_reading_t g_probes[HE_MAX_SENSORS];
static int g_probe_count = 0;

/* ---- Paired radio id (1..6, 0 = unpaired / awaiting assignment) ----
 * Persisted in NVS under "node_id". An unpaired node announces itself with
 * frames "<02 E3><temp>T0#" and listens for "PR<id>T9#" from the
 * controller; on receipt it stores the id and announces itself under it. */
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

/* ---- Binary protocol (LoRa.txt) ----
 * Frame: <0x02 0xE3><payload>T<sender>#, sender = node id (the controller
 * is id 9). Payload: measurement "%02.3f", probe failure "ERR", controller
 * ack "X<id>" and pairing "PR<id>". */

#define HE_BIN_MAGIC0       0x02
#define HE_BIN_MAGIC1       0xE3
#define HE_BIN_CTRL_ID      9

/* Build and send one binary frame. */
static void send_frame_bin(int rid, const char *payload)
{
    uint8_t frame[40];
    size_t n = 0;
    frame[n++] = HE_BIN_MAGIC0;
    frame[n++] = HE_BIN_MAGIC1;
    n += (size_t)snprintf((char *)frame + n, sizeof(frame) - n - 1,
                          "%sT%d#", payload, rid);
    ESP_LOGI(TAG, "tx: <02><E3>%sT%d#", payload, rid);
    lora_write(frame, n);
}

/* Parse "<magic><payload>T<sender>#" (trailing '#' optional —
 * lora_receive_line() already strips it). Returns false when the bytes do
 * not fit the frame template; a single space between payload and 'T'
 * (readable spec form) is tolerated on receive. */
static bool node_parse_bin(const char *line, size_t len,
                           char *payload, size_t psize, int *out_sender_id)
{
    if (len < 6) return false;
    if ((uint8_t)line[0] != HE_BIN_MAGIC0 ||
        (uint8_t)line[1] != HE_BIN_MAGIC1) return false;
    if ((uint8_t)line[len - 1] == '#') len--;
    if (len < 6) return false;
    const char *t = NULL;
    for (ssize_t i = (ssize_t)len - 1; i >= 2; i--)
        if (line[i] == 'T') { t = line + i; break; }
    if (!t) return false;
    size_t plen = (size_t)(t - (line + 2));
    if (plen < 1 || plen >= psize) return false;
    memcpy(payload, line + 2, plen);
    payload[plen] = '\0';
    size_t ilen = (size_t)((line + len) - (t + 1));
    if (ilen < 1 || ilen > 2) return false;
    char idbuf[3];
    memcpy(idbuf, t + 1, ilen);
    idbuf[ilen] = '\0';
    int id = atoi(idbuf);
    if (id < 0) return false;
    *out_sender_id = id;
    return true;
}

/* Drop the trailing space of the tolerated readable form ("X2 T9#"). */
static void payload_trim(char *p)
{
    size_t n = strlen(p);
    while (n > 0 && p[n - 1] == ' ') p[--n] = '\0';
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
 * paired id is erased; the node then reboots (id 0 / T0) on release. */
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

/* Erase the persisted radio id (factory default = unpaired T0). */
static void node_id_erase(void)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "node_id");
    nvs_commit(h);
    nvs_close(h);
    g_node_id = 0;
}

/* Unpaired mode: announce "<02 E3><temp>T0#" frames and wait for the
 * controller's PR assignment. Pairing is confirmed with an immediate
 * announcement under the new id — the controller completes pairing only
 * after seeing a fresh "T<id>" frame. */
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
        char pl[16];
        snprintf(pl, sizeof(pl), "%02.3f", t);
        ESP_LOGI(TAG, "unpaired, announcing T0");
        send_frame_bin(0, pl);

        /* Wait for the controller's assignment. */
        int rc = lora_receive_line(HE_LORA_RX_WINDOW_MS, line, sizeof(line));
        if (rc > 0) {
            char payload[24];
            int sender = -1;
            if (node_parse_bin(line, (size_t)rc, payload, sizeof(payload), &sender)) {
                payload_trim(payload);
                if (sender == HE_BIN_CTRL_ID &&
                    payload[0] == 'P' && payload[1] == 'R') {
                    int new_id = atoi(payload + 2);
                    if (new_id >= 1 && new_id <= HE_MAX_SENSORS &&
                        node_id_save((uint8_t)new_id)) {
                        int16_t c = (g_probe_count > 0) ? g_probes[0].centi : INT16_MIN;
                        if (c == INT16_MIN) {
                            send_frame_bin(new_id, "ERR");
                        } else {
                            char pl2[16];
                            snprintf(pl2, sizeof(pl2), "%02.3f", c / 100.0f);
                            send_frame_bin(new_id, pl2);
                        }
                        ESP_LOGI(TAG, "paired as T%d", new_id);
                        return;   /* back to normal operation */
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---- ACK handling (LoRa.txt) ----
 * After each frame the node opens a 2 s receive window and accepts only
 * the ack "<02 E3>X<id>T9#" for the id it just sent — ack/multi frames
 * addressed to another node ("X2" while we are T1) are ignored (rule 15).
 * 2 s without ack: raise the output power and repeat the same byte-identical
 * frame once. An ack lowers the power back to standard. */

typedef struct {
    int  rid;          /* radio id whose frame is awaiting the ack */
    bool acked;
} ack_ctx_t;

/* Consume binary frames for up to remaining_ms; set ctx->acked when the
 * ack for ctx->rid arrives. Everything else is ignored. */
static void await_ack(ack_ctx_t *ctx, int remaining_ms)
{
    int64_t end = esp_timer_get_time() + (int64_t)remaining_ms * 1000;
    char line[HE_LORA_LINE_MAX];
    while (esp_timer_get_time() < end && !ctx->acked) {
        int rc = lora_receive_line(
            (uint32_t)((end - esp_timer_get_time()) / 1000), line, sizeof(line));
        if (rc <= 0) break;
        char payload[24];
        int sender = -1;
        if (!node_parse_bin(line, (size_t)rc, payload, sizeof(payload), &sender))
            continue;
        payload_trim(payload);
        if (sender == HE_BIN_CTRL_ID && payload[0] == 'X') {
            int id = atoi(payload + 1);
            if (id == ctx->rid) {
                ctx->acked = true;
            } else {
                ESP_LOGI(TAG, "ack for other node (T%d), ignored", id);
            }
        }
    }
}

/* Send one binary measurement/ERR frame and run the ack dance.
 * Returns 1 when the frame was acknowledged. */
static int send_and_ack(int rid, const char *payload)
{
    ack_ctx_t ctx = { .rid = rid, .acked = false };
    send_frame_bin(rid, payload);
    await_ack(&ctx, HE_LORA_ACK_TIMEOUT_MS);
    if (!ctx.acked) {
        bool raised = lora_power_status() != HE_LORA_PWR_HIGH;
        if (raised) {
            ESP_LOGW(TAG, "no ack in %d ms — raising power", HE_LORA_ACK_TIMEOUT_MS);
            lora_set_power(HE_LORA_PWR_HIGH);
        }
        send_frame_bin(rid, payload);   /* byte-identical repeat */
        await_ack(&ctx, HE_LORA_ACK_TIMEOUT_MS);
    }
    if (ctx.acked) {
        ESP_LOGI(TAG, "ack confirmed T%d", rid);
        if (lora_power_status() != HE_LORA_PWR_LOW)
            lora_set_power(HE_LORA_PWR_LOW);
    }
    return ctx.acked ? 1 : 0;
}

/* Payload for one probe: "%02.3f" temperature, or "ERR" for a read
 * failure (DS18B20 absent / CRC error, centi == INT16_MIN). */
static void probe_payload(const ds18b20_reading_t *p, int index, char *out, size_t len)
{
    if (p->centi == INT16_MIN) {
        ESP_LOGW(TAG, "probe %d read failure — ERR frame", index);
        snprintf(out, len, "ERR");
        return;
    }
    snprintf(out, len, "%02.3f", p->centi / 100.0f);
}

/* ---- Main task ---- */
static void sensor_task(void *arg)
{
    esp_task_wdt_add(NULL);
    int64_t next_frame_us = 0;
    int64_t last_ack_us = esp_timer_get_time();   /* boot grace */

    for (;;) {
        esp_task_wdt_reset();

        /* 1-Wire: discover + read. */
        if (g_probe_count == 0)
            g_probe_count = ds18b20_enumerate(g_probes, HE_MAX_SENSORS);
        ds18b20_read_all(g_probes, g_probe_count);

        /* One frame (and its ack dance) per probe; radio id = paired base
         * + probe index — a node can carry several probes on one bus. */
        int acked = 0;
        if (g_probe_count == 0) {
            /* No probe on the bus: report the base radio id as failed. */
            if (send_and_ack(g_node_id, "ERR"))
                { acked++; last_ack_us = esp_timer_get_time(); }
        }
        for (int i = 0; i < g_probe_count; i++) {
            int rid = g_node_id + i;
            if (rid > HE_MAX_SENSORS) rid = HE_MAX_SENSORS;
            char pl[16];
            probe_payload(&g_probes[i], i, pl, sizeof(pl));
            if (send_and_ack(rid, pl))
                { acked++; last_ack_us = esp_timer_get_time(); }
        }
        (void)acked;

        /* Cyclic reboot when the controller stopped acknowledging. */
        if ((esp_timer_get_time() - last_ack_us) / 1000 > HE_LORA_NO_ACK_REBOOT_MS) {
            ESP_LOGW(TAG, "link dead — %u s without ack, rebooting",
                     (unsigned)(HE_LORA_NO_ACK_REBOOT_MS / 1000));
            vTaskDelay(pdMS_TO_TICKS(200));   /* let logs flush */
            esp_restart();
        }

        /* Period between frames: 20 s + 0-2 s jitter (LoRa.txt), measured
         * start-to-start — the ack windows and probe reads count toward the
         * interval instead of being added on top. */
        int64_t now = esp_timer_get_time();
        next_frame_us += (int64_t)(HE_LORA_FRAME_PERIOD_MS +
                                   (int)(esp_random() % HE_LORA_FRAME_JITTER_MS)) * 1000;
        if (next_frame_us < now) next_frame_us = now + 100000;
        vTaskDelay(pdMS_TO_TICKS((next_frame_us - now) / 1000));
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
     * LED, release reboots into T0. Independent of pairing/LoRa state. */
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