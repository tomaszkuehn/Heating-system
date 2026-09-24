/**
 * Network manager (spec section 12).
 *
 * ESP32-ETH01 V1.4: Ethernet (IP101GR PHY, RMII) is the primary link.
 * WiFi SoftAP is a fallback when no Ethernet link is detected within a
 * timeout, so the device is always reachable for configuration.
 *
 *   - Ethernet: DHCP on the wired link (primary).
 *   - AP (fallback "ESP" / "12345") used when Ethernet is down, so the
 *     user can reach the captive setup page.
 *   - STA client of the user's network, configured via the web UI (optional).
 *
 * A hardware button (held 5s) resets only the network settings while
 * preserving the rest of the user configuration. SNTP is started once a
 * link comes up so the daily profile can use the real time of day.
 */
#pragma once

#include "storage_manager.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start networking: Ethernet first, WiFi AP/STA fallback. */
void network_init(system_config_t *cfg);

/* Switch WiFi mode at runtime (called after the user saves network settings).
 * Ethernet stays up regardless. */
void network_apply(const system_config_t *cfg);

/* Any interface (Ethernet or WiFi STA) has an IP address. */
bool network_is_up(void);

/* Current device IP address (Ethernet DHCP, AP 192.168.4.1, or STA DHCP). */
const char *network_device_ip(void);

/* Start the hardware reset-button watcher. */
void network_start_reset_button(system_config_t *cfg);

#ifdef __cplusplus
}
#endif