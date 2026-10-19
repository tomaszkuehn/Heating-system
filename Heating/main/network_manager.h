/**
 * Network manager (spec section 12).
 *
 * ESP32-ETH01 V1.4: Ethernet (LAN8720 PHY, RMII) is the ONLY link. WiFi was
 * removed from the firmware — the radio stack is disabled in sdkconfig
 * (CONFIG_ESP_WIFI_ENABLED=n) and the runtime only drives the Ethernet
 * netif, SNTP and mDNS. The historic WiFi settings kept space in the NVS
 * config blob (tail-safety rule) but are inert: no UI field, no API handler
 * reads or writes them.
 *
 * SNTP is started once the link comes up so the daily profile can use the
 * real time of day.
 */
#pragma once

#include "storage_manager.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start networking: Ethernet (static IP), then SNTP/mDNS on link up. */
void network_init(system_config_t *cfg);

/* Ethernet link has an IP address. */
bool network_is_up(void);

/* Current device IP address (Ethernet, static or DHCP). */
const char *network_device_ip(void);

#ifdef __cplusplus
}
#endif