/**
 * Global hardware / wiring configuration.
 * Centralised so it can be retargeted without touching module logic.
 */
#pragma once

#include "data_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- GPIO (WT32-ETH01 V1.4 pinout) ----
 * RMII hardwired: GPIO0(clk),18(MDIO),19(TXD0),21(TX_EN),22(TXD1),
 * 23(MDC),25(RXD0),26(RXD1),27(CRS_DV).
 * GPIO16 = LAN8720 + 50 MHz oscillator power enable (must be HIGH).
 * Relocated: heating relay 16->32 (frees PHY power pin). */
#define HE_GPIO_HEATING         GPIO_NUM_32   /* relay line to the furnace   */
#define HE_GPIO_NET_RESET_BTN   GPIO_NUM_15   /* external button: hold 5s => net reset */
#define HE_GPIO_LED             GPIO_NUM_2    /* onboard LED (blinks during net-reset hold) */
#define HE_NET_RESET_HOLD_MS    5000          /* 5s hold => network reset    */

/* ---- Sensor interface (external board over UART) ----
 * GPIO17 TX is safe; GPIO18 is MDIO → RX moved to GPIO13. */
#define HE_SENSOR_UART          UART_NUM_1
#define HE_SENSOR_UART_TX       GPIO_NUM_17
#define HE_SENSOR_UART_RX       GPIO_NUM_13
#define HE_SENSOR_BAUD          115200
#define HE_SENSOR_BUF_SIZE      256

/* ---- LoRa receiver (radio module Ebyte E32 433T20D).
 *      Receives "TT.TTT T<id>&\r\n" text lines over UART2.
 *      GPIO12/14 are free (not on RMII bus). Mode-control pins moved
 *      off GPIO25/26 (RMII RXD0/RXD1) to GPIO4/5. ---- */
#define HE_LORA_UART            UART_NUM_2
#define HE_LORA_UART_TX         GPIO_NUM_12    /* to module RXD            */
#define HE_LORA_UART_RX         GPIO_NUM_14    /* from module TXD          */
#define HE_LORA_BAUD            9600
#define HE_LORA_BUF_SIZE        256
/* Mode-control pins (AUX / TX-RX) on the E32 module. Moved off GPIO25/26
 * (RMII RXD0/RXD1) to safe output pins GPIO4/GPIO5. */
#define HE_GPIO_LORA_AUX        GPIO_NUM_4    /* mode select (0=normal)   */
#define HE_GPIO_LORA_TXRX       GPIO_NUM_5    /* mode select (0=normal)    */
#define HE_LORA_MODE_NORMAL    0
#define HE_LORA_MODE_CONFIG    1
#define HE_LORA_RX_TASK_STACK   3072
#define HE_LORA_LINE_MAX        48             /* max length of one frame  */
/* Nominal period between radio frames from a sensor node (~5.5 s measured on
 * the air with the current node firmware). Used for loss-rate estimation. */
#define HE_LORA_FRAME_PERIOD_MS 6000

/* ---- Ethernet (WT32-ETH01 V1.4 — LAN8720 PHY) ----
 * RMII data pins are hardwired in the ESP32 silicon. 50 MHz clock from the
 * on-board oscillator on GPIO0. PHY address 0. CRITICAL: GPIO16 is the
 * LAN8720 + oscillator power enable — it must be driven HIGH before the
 * PHY is initialised. */
#define HE_ETH_PHY_ADDR        0
#define HE_ETH_PHY_RST_GPIO    (-1)           /* no dedicated reset line */
#define HE_ETH_MDC_GPIO        23
#define HE_ETH_MDIO_GPIO       18
#define HE_ETH_PHY_POWER_GPIO  GPIO_NUM_16    /* LAN8720 power enable (HIGH=on) */
/* Static Ethernet configuration (DHCP client disabled on the ETH netif). */
#define HE_ETH_STATIC_IP       "10.168.34.55"
#define HE_ETH_NETMASK         "255.255.255.0"
#define HE_ETH_GATEWAY         "10.168.34.1"

/* ---- Control loop timing ---- */
#define HE_CONTROL_TICK_MS      1000          /* control engine period       */
#define HE_SENSOR_POLL_MS       1000          /* how often we poll the iface */
#define HE_BOOST_DURATION_SEC   300           /* 5-minute boost              */
#define HE_SIM_TIME_SCALE       10            /* x10 fast-forward in sim mode */

/* ---- Hysteresis / protection defaults (spec 5.2) ----
 * HE_MIN_ON_SEC / HE_MIN_OFF_SEC / HE_ANTIOSC_LOCK_SEC are the compile-time
 * fallbacks used when a loaded config field is out of range (< the floor). The
 * user-configurable values live in system_config_t (min_on_sec etc., tail
 * fields, set via /api/limits) and default to HE_DEFAULT_* below. */
#define HE_MIN_ON_SEC           90           /* min continuous ON before off allowed */
#define HE_MIN_OFF_SEC          90           /* min continuous OFF before on allowed */
#define HE_MAX_ON_SEC           (3600 * 4)
#define HE_ANTIOSC_LOCK_SEC     120           /* anti-oscillation lock after state change */
#define HE_DEFAULT_MIN_ON_SEC       90       /* user-config: min ON  (range 30..3600) */
#define HE_DEFAULT_MIN_OFF_SEC      90       /* user-config: min OFF (range 30..3600) */
#define HE_DEFAULT_ANTIOSC_LOCK_SEC 120      /* user-config: anti-osc lock (range 30..600) */
#define HE_MIN_ON_OFF_FLOOR     30           /* hard floor for min_on/min_off (anti-chatter) */
#define HE_ANTIOSC_FLOOR        30           /* hard floor for anti-osc lock */

/* ---- Network defaults (spec 12) ---- */
#define HE_DEFAULT_AP_SSID      "ESP"
#define HE_DEFAULT_AP_PASS      "12345678"    /* WPA2 requires >= 8 chars */

/* ---- Web panel authentication defaults (user/password live in NVS) ---- */
#define HE_DEFAULT_PANEL_USER   "admin"
#define HE_DEFAULT_PANEL_PASS   "12345678"
#define HE_PANEL_PASS_MIN       6             /* min length for a new password */

/* ---- Device identity & protection-limit defaults ---- */
#define HE_DEFAULT_DEVICE_NAME      "Sterownik CO"
#define HE_DEFAULT_SMTP_PORT        25          /* plain-text SMTP submission port */
#define HE_DEFAULT_FAULT_GRACE_SEC  300        /* NO_HEAT_RISE grace (5 min)    */
#define HE_DEFAULT_MAX_ON_SEC       14400      /* max continuous heating (4 h)  */
#define HE_DEFAULT_MAX_ON_BREAK_SEC 600        /* forced break after max-on      */

/* ---- Emergency periodic mode defaults ---- */
#define HE_DEFAULT_EMERGENCY_ENABLED         false
#define HE_DEFAULT_EMERGENCY_ON_SEC          900   /* 15 min ON within the cycle  */
#define HE_DEFAULT_EMERGENCY_PERIOD_SEC      3600  /* 60 min cycle                 */
#define HE_DEFAULT_EMERGENCY_ON_SENSOR_FAULT false /* opt-in: emergency duty cycle when all sensors fail */

#ifdef __cplusplus
}
#endif