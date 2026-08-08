/**
 * Global hardware / wiring configuration for the DS18x20 sensor node.
 *
 * The node reads up to HE_MAX_SENSORS DS18B20 probes on a 1-Wire bus and
 * sends the readings to the heating controller over a LoRa radio as
 * text lines ("TT.TTT T<id>&"). A UART debug link mirrors the traffic.
 *
 * Centralised so it can be retargeted without touching module logic.
 */
#pragma once

#include <stdint.h>
#include "driver/gpio.h"
#include "driver/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 1-Wire bus ---- */
#define HE_GPIO_ONEWIRE        GPIO_NUM_32   /* DS18B20 data line        */

/* ---- LoRa radio (UART) ---- */
#define HE_LORA_UART           UART_NUM_1
#define HE_LORA_UART_TX         GPIO_NUM_12
#define HE_LORA_UART_RX         GPIO_NUM_14
#define HE_LORA_BAUD            9600
#define HE_LORA_BUF_SIZE        256

/* ---- LoRa mode-control pins (AUX / TX-RX) ---- */
#define HE_GPIO_LORA_AUX        GPIO_NUM_25  /* mode-select (was 25)     */
#define HE_GPIO_LORA_TXRX       GPIO_NUM_26  /* mode-select (was 26)     */
#define HE_LORA_MODE_NORMAL     0            /* normal Tx/Rx operation   */
#define HE_LORA_MODE_CONFIG     1            /* AT-command / config mode */

/* ---- LoRa power levels ---- */
#define HE_LORA_PWR_LOW        1             /* low-power mode           */
#define HE_LORA_PWR_HIGH       3             /* full-power mode          */

/* ---- Timing ---- */
#define HE_WDT_TIMEOUT_SEC     19            /* task watchdog timeout     */
#define HE_LORA_RX_WINDOW_MS   5000         /* ACK receive window         */
#define HE_LORA_INIT_MS        4000         /* wait after AT+RESET        */
#define HE_LORA_PWR_SETTLE_MS  500          /* settle after power change  */
#define HE_STATUS_OK            50          /* confirmed link status      */
#define HE_STATUS_PWR_DOWN      40          /* threshold to drop power    */
#define HE_STATUS_REBOOT        1           /* threshold to force reboot  */

/* ---- Sensor count / limits ---- */
#define HE_MAX_SENSORS         6             /* 1..6 internal probes       */
/* Logical temperature window (matches controller spec). */
#define HE_TEMP_MIN_LOGICAL   (-40.0f)
#define HE_TEMP_MAX_LOGICAL    (125.0f)
/* Raw error sentinel from a missing/disconnected DS18B20. */
#define HE_TEMP_ERROR_RAW      (-127.0f)

/* ---- Frame protocol (must match controller's sensor_manager) ---- */
#define HE_FRAME_START         0xAA
#define HE_FRAME_END           0x55
#define HE_CMD_POLL            0x01

#ifdef __cplusplus
}
#endif