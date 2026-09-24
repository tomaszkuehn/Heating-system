#include "lora_receiver.h"
#include "app_config.h"
#include "sensor_manager.h"
#include "data_model.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "lora";

/* Set while the web /api/lora/test handler owns the UART: the rx task then
 * idles instead of competing for incoming bytes (it would otherwise swallow
 * the module's config response). */
static volatile bool s_test_active = false;

void lora_receiver_test_mode(bool on)
{
    s_test_active = on;
}

/* Parse one line "TT.TTT T<id>&" into (id, temperature).
 * Returns true on success. id is 1..HE_MAX_SENSORS; temperature in degC.
 * Also accepts "ERR T&" as a sensor-failure marker (id=0, temp=NAN). */
static bool parse_line(const char *line, int *out_id, float *out_temp)
{
    /* Minimal validation: must contain '&' and 'T'. */
    const char *amp = strchr(line, '&');
    if (!amp) return false;
    const char *t = strchr(line, 'T');
    if (!t) return false;

    /* "ERR T&" → failure marker. */
    if (strncmp(line, "ERR", 3) == 0) {
        *out_id = 0;
        *out_temp = he_nan();
        return true;
    }

    /* Temperature is the float before the first space. */
    char *endp = NULL;
    float temp = strtof(line, &endp);
    if (endp == line || endp >= amp) return false;

    /* Skip spaces, expect 'T', then the numeric id. */
    while (*endp == ' ') endp++;
    if (*endp != 'T') return false;
    endp++;
    long id = strtol(endp, &endp, 10);
    if (id < 0 || id > HE_MAX_SENSORS) return false;
    /* The id may be followed by '&' or whitespace+&'. */
    while (*endp == ' ' || *endp == '\r' || *endp == '\n') endp++;
    if (*endp != '&') return false;

    *out_id = (int)id;
    *out_temp = temp;
    return true;
}

static void process_line(const char *line)
{
    int id;
    float temp;
    if (!parse_line(line, &id, &temp)) {
        ESP_LOGW(TAG, "malformed line: %s", line);
        return;
    }

    if (id == 0 || he_isnan(temp)) {
        ESP_LOGW(TAG, "sensor reports error");
        return;
    }

    if (temp < HE_TEMP_MIN_LOGICAL || temp > HE_TEMP_MAX_LOGICAL) {
        ESP_LOGW(TAG, "T%d out of range: %.2f", id, temp);
        return;
    }

    /* Forward under the shared config lock — sensor_manager_poll() and the
     * HTTP handlers touch the same sensor_t array from other tasks/cores. */
    he_config_lock();
    sensor_manager_lora_update(id, temp);
    he_config_unlock();

    ESP_LOGI(TAG, "T%d = %.2f C", id, temp);

    /* Acknowledge to the remote node so it can manage its power level and
     * watchdog. The node expects the single byte 'X' as confirmation. */
    static const uint8_t ack = 'X';
    uart_write_bytes(HE_LORA_UART, &ack, 1);
}

static void lora_rx_task(void *arg)
{
    char line[HE_LORA_LINE_MAX + 1];
    int  line_len = 0;

    for (;;) {
        if (s_test_active) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        uint8_t b;
        int got = uart_read_bytes(HE_LORA_UART, &b, 1, pdMS_TO_TICKS(100));
        if (got <= 0) continue;

        /* End-of-line delimiters: '\n' terminates a frame, '\r' ignored. */
        if (b == '\r') continue;
        if (b == '\n') {
            if (line_len > 0) {
                line[line_len] = '\0';
                process_line(line);
                line_len = 0;
            }
            continue;
        }
        /* The frame terminator in the protocol is '&'; flush at '&' too so
         * a missing CRLF does not starve the parser. */
        if (line_len < HE_LORA_LINE_MAX) {
            line[line_len++] = (char)b;
        }
        if (b == '&') {
            line[line_len] = '\0';
            process_line(line);
            line_len = 0;
        }
    }
}

void lora_receiver_init(void)
{
    /* Mode-control pins: both LOW for normal Tx/Rx operation. */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << HE_GPIO_LORA_AUX) | (1ULL << HE_GPIO_LORA_TXRX),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(HE_GPIO_LORA_AUX,  HE_LORA_MODE_NORMAL);
    gpio_set_level(HE_GPIO_LORA_TXRX, HE_LORA_MODE_NORMAL);

    const uart_config_t uc = {
        .baud_rate  = HE_LORA_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    ESP_ERROR_CHECK(uart_driver_install(HE_LORA_UART, HE_LORA_BUF_SIZE * 2,
                                         HE_LORA_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(HE_LORA_UART, &uc));
    ESP_ERROR_CHECK(uart_set_pin(HE_LORA_UART, HE_LORA_UART_TX, HE_LORA_UART_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(lora_rx_task, "lora_rx", HE_LORA_RX_TASK_STACK, NULL, 5, NULL);
    ESP_LOGI(TAG, "LoRa receiver on UART%d @ %d baud", HE_LORA_UART, HE_LORA_BAUD);
}