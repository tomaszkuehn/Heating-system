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

static const char *TAG = "main";

static ds18b20_reading_t g_probes[HE_MAX_SENSORS];
static int g_probe_count = 0;

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
     * original sketch format (single-probe variant was "%02.3f T01&"). */
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
        snprintf(line, sizeof(line), "%02.3f T%02d&", t, g_probes[i].id);
        ESP_LOGI(TAG, "T%d = %.2f C", g_probes[i].id, t);
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

        rx_ctx_t ctx = { .got_ack = false };
        lora_receive(HE_LORA_RX_WINDOW_MS, on_byte, &ctx);
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

    /* Watchdog. */
    esp_task_wdt_deinit();
    esp_task_wdt_config_t wdt = {
        .timeout_ms     = HE_WDT_TIMEOUT_SEC * 1000,
        .idle_core_mask  = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic   = true,
    };
    ESP_ERROR_CHECK(esp_task_wdt_init(&wdt));
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    /* 1-Wire bus. */
    ow_init(HE_GPIO_ONEWIRE);

    /* LoRa radio. */
    lora_init();

    /* Run the sensor loop on its own task so app_main can return. */
    xTaskCreate(sensor_task, "sensor", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "ready");
}