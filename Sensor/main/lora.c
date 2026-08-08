#include "lora.h"
#include "app_config.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "lora";
static int s_power = HE_LORA_PWR_HIGH;

void lora_set_mode(int mode)
{
    gpio_set_level(HE_GPIO_LORA_AUX,  mode);
    gpio_set_level(HE_GPIO_LORA_TXRX, mode);
}

void lora_init(void)
{
    /* Mode-control pins as outputs, start in config mode for AT setup. */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << HE_GPIO_LORA_AUX) | (1ULL << HE_GPIO_LORA_TXRX),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

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

    /* Bring the radio up at full power. */
    lora_set_mode(HE_LORA_MODE_CONFIG);
    vTaskDelay(pdMS_TO_TICKS(200));
    lora_send_at("AT+POWER=3");
    vTaskDelay(pdMS_TO_TICKS(200));
    lora_send_at("AT+RESET");
    vTaskDelay(pdMS_TO_TICKS(HE_LORA_INIT_MS));
    lora_set_mode(HE_LORA_MODE_NORMAL);
    s_power = HE_LORA_PWR_HIGH;

    ESP_LOGI(TAG, "LoRa UART%d @ %d baud, full power", HE_LORA_UART, HE_LORA_BAUD);
}

void lora_set_power(int power)
{
    lora_set_mode(HE_LORA_MODE_CONFIG);
    vTaskDelay(pdMS_TO_TICKS(200));
    if (power == HE_LORA_PWR_LOW) {
        lora_send_at("AT+POWER=1");
        s_power = HE_LORA_PWR_LOW;
    } else {
        lora_send_at("AT+POWER=3");
        s_power = HE_LORA_PWR_HIGH;
    }
    vTaskDelay(pdMS_TO_TICKS(HE_LORA_PWR_SETTLE_MS));
    lora_send_at("AT+RESET");
    vTaskDelay(pdMS_TO_TICKS(HE_LORA_INIT_MS));
    lora_set_mode(HE_LORA_MODE_NORMAL);
    ESP_LOGI(TAG, "power -> %d", s_power);
}

void lora_send_at(const char *at_cmd)
{
    uart_write_bytes(HE_LORA_UART, at_cmd, strlen(at_cmd));
    /* Modules accept AT commands terminated by CRLF. */
    uart_write_bytes(HE_LORA_UART, "\r\n", 2);
    ESP_LOGI(TAG, "AT> %s", at_cmd);
}

void lora_write(const uint8_t *data, size_t len)
{
    uart_write_bytes(HE_LORA_UART, data, len);
}

bool lora_receive(uint32_t window_ms, lora_rx_cb_t on_byte, void *user)
{
    int64_t end = esp_timer_get_time() + (int64_t)window_ms * 1000;
    uint8_t b;
    while (esp_timer_get_time() < end) {
        int got = uart_read_bytes(HE_LORA_UART, &b, 1, pdMS_TO_TICKS(20));
        if (got > 0) {
            if (on_byte && on_byte(b, user)) return true;
        }
    }
    return false;
}

int lora_power_status(void) { return s_power; }