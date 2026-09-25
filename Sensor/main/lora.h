/**
 * LoRa radio manager (ESP-IDF).
 *
 * Wraps a UART-attached LoRa module (e.g. E32/E220-style) with the
 * AT command set the original sketch used (AT+POWER, AT+RESET), plus
 * a non-blocking receive window driven by esp_timer.
 *
 * Replaces the Arduino SoftwareSerial layer of the original sketch
 * with a real ESP-IDF UART driver and structured logging.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise UART and bring the radio to full power / normal mode. */
void lora_init(void);

/* Set the mode-control pins (AUX / TX-RX) to normal or config. */
void lora_set_mode(int mode);

/* Change radio power level (HE_LORA_PWR_LOW / HE_LORA_PWR_HIGH). */
void lora_set_power(int power);

/* Send an AT command string (no trailing newline needed). */
void lora_send_at(const char *at_cmd);

/* Send raw bytes. */
void lora_write(const uint8_t *data, size_t len);

/* Non-blocking receive window: up to window_ms, calling on_byte for each
 * received byte. Returns true if on_byte returned true (handled). */
typedef bool (*lora_rx_cb_t)(uint8_t byte, void *user);
bool lora_receive(uint32_t window_ms, lora_rx_cb_t on_byte, void *user);

/* Receive one protocol line ("...\r\n" or "...&") into buf (NUL-terminated)
 * within window_ms. Returns line length (>=0) or -1 on timeout. Trailing
 * terminator ('\n' or '&') is stripped. */
int lora_receive_line(uint32_t window_ms, char *buf, size_t buflen);

/* Current power level (HE_LORA_PWR_LOW / HE_LORA_PWR_HIGH). */
int lora_power_status(void);

#ifdef __cplusplus
}
#endif