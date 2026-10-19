#include "network_manager.h"
#include "app_config.h"
#include "fault_manager.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "driver/gpio.h"
#include "mdns.h"

static const char *TAG = "net";

static EventGroupHandle_t s_evt;
#define BIT_ETH_CONNECTED  (1 << 1)
static esp_netif_t *s_eth_netif = NULL;
static esp_eth_handle_t s_eth_handle = NULL;

static bool s_sntp_started = false;
static char s_device_ip[16] = "";   /* current device IP for the dashboard */

static void sntp_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "SNTP time acquired");
}

static void start_sntp(void)
{
    if (s_sntp_started) return;
    s_sntp_started = true;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_set_time_sync_notification_cb(sntp_cb);
    esp_sntp_init();
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
}

static void start_mdns(void)
{
    static bool s_mdns_done = false;
    if (s_mdns_done) return;
    s_mdns_done = true;
    esp_err_t mr = mdns_init();
    if (mr == ESP_OK) {
        mdns_hostname_set("heating");
        mdns_instance_name_set("Heating Controller");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS started: http://heating.local");
    } else {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(mr));
    }
}

static void net_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    (void)arg;
    if (base == ETH_EVENT && id == ETHERNET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "Ethernet link up");
    } else if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "Ethernet link down");
        xEventGroupClearBits(s_evt, BIT_ETH_CONNECTED);
        fault_manager_set_network_up(false);
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        xEventGroupSetBits(s_evt, BIT_ETH_CONNECTED);
        fault_manager_set_network_up(true);
        snprintf(s_device_ip, sizeof(s_device_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        ESP_LOGI(TAG, "Ethernet got IP: %s", s_device_ip);
        start_sntp();
        start_mdns();
    }
}

/* Scan all 32 MDIO addresses for a responding PHY.
 * LAN8720 (SMSC/Microchip) PHY ID register bits 31:16 = 0x0007. */
static int detect_phy_addr(esp_eth_mac_t *mac)
{
    /* Pass 1: exact LAN8720 ID match. */
    for (int a = 0; a < 32; a++) {
        uint32_t id1 = 0, id2 = 0;
        if (mac->read_phy_reg(mac, a, 0x02, &id1) == ESP_OK &&
            mac->read_phy_reg(mac, a, 0x03, &id2) == ESP_OK &&
            id1 == 0x0007 && id2 != 0xFFFF && id2 != 0x0000) {
            ESP_LOGI(TAG, "LAN8720 detected at MDIO addr %d (ID %04lX:%04lX)", a, id1, id2);
            return a;
        }
    }
    /* Pass 2: any plausible ID (clone PHYs). */
    for (int a = 0; a < 32; a++) {
        uint32_t id1 = 0, id2 = 0;
        if (mac->read_phy_reg(mac, a, 0x02, &id1) == ESP_OK &&
            mac->read_phy_reg(mac, a, 0x03, &id2) == ESP_OK &&
            id1 != 0xFFFF && id1 != 0x0000 && id2 != 0xFFFF && id2 != 0x0000) {
            ESP_LOGW(TAG, "Non-LAN8720 PHY at MDIO addr %d (ID %04lX:%04lX)", a, id1, id2);
            return a;
        }
    }
    return -1;
}

static void start_ethernet(void)
{
    if (s_eth_handle) return;   /* already started */

    /* WT32-ETH01: GPIO16 powers the LAN8720 PHY and the 50 MHz oscillator.
     * Drive it HIGH and let the rail settle before touching the PHY. */
    gpio_config_t pw = {
        .pin_bit_mask = (1ULL << HE_ETH_PHY_POWER_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pw);
    gpio_set_level(HE_ETH_PHY_POWER_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    /* MDIO needs a pull-up on some boards where the PHY-side resistor is missing. */
    gpio_set_pull_mode(HE_ETH_MDIO_GPIO, GPIO_PULLUP_ONLY);

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t esp32_emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp32_emac_cfg.smi_gpio.mdc_num = HE_ETH_MDC_GPIO;
    esp32_emac_cfg.smi_gpio.mdio_num = HE_ETH_MDIO_GPIO;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_cfg, &mac_cfg);

    int phy_addr = detect_phy_addr(mac);
    if (phy_addr < 0) {
        ESP_LOGE(TAG, "No PHY answered on MDIO (MDC=%d MDIO=%d) — check cabling/PHY",
                 HE_ETH_MDC_GPIO, HE_ETH_MDIO_GPIO);
        return;
    }

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = phy_addr;
    phy_cfg.reset_gpio_num = HE_ETH_PHY_RST_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_cfg);

    esp_eth_config_t eth_cfg_drv = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t er = esp_eth_driver_install(&eth_cfg_drv, &s_eth_handle);
    if (er != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver install failed: %s",
                 esp_err_to_name(er));
        return;
    }

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(s_eth_handle);
    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, glue));

    /* Static IP: stop the DHCP client and set the address before the link
     * comes up, so lwip binds the netif to the configured address directly. */
    esp_netif_dhcpc_stop(s_eth_netif);
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = esp_ip4addr_aton(HE_ETH_STATIC_IP);
    ip.netmask.addr = esp_ip4addr_aton(HE_ETH_NETMASK);
    ip.gw.addr = esp_ip4addr_aton(HE_ETH_GATEWAY);
    esp_err_t ipr = esp_netif_set_ip_info(s_eth_netif, &ip);
    if (ipr != ESP_OK) {
        ESP_LOGE(TAG, "static IP set failed: %s (falling back to DHCP)", esp_err_to_name(ipr));
        esp_netif_dhcpc_start(s_eth_netif);
    } else {
        snprintf(s_device_ip, sizeof(s_device_ip), HE_ETH_STATIC_IP);
        ESP_LOGI(TAG, "ETH static IP: %s/%s gw %s", HE_ETH_STATIC_IP, HE_ETH_NETMASK, HE_ETH_GATEWAY);
    }

    /* With dhcpc stopped the netif has no DNS servers, so lwip cannot resolve
     * pool.ntp.org and SNTP never syncs (time_synced stays false, which gates
     * minute sampling -> empty 24h/daily charts). Set DNS explicitly. */
    esp_netif_dns_info_t dns;
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton(HE_ETH_DNS_MAIN);
    esp_netif_set_dns_info(s_eth_netif, ESP_NETIF_DNS_MAIN, &dns);
    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton(HE_ETH_DNS_BACKUP);
    esp_netif_set_dns_info(s_eth_netif, ESP_NETIF_DNS_BACKUP, &dns);
    ESP_LOGI(TAG, "ETH DNS: %s / %s", HE_ETH_DNS_MAIN, HE_ETH_DNS_BACKUP);

    ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));
    ESP_LOGI(TAG, "Ethernet started (LAN8720 addr=%d, MDC=%d, MDIO=%d)",
             phy_addr, HE_ETH_MDC_GPIO, HE_ETH_MDIO_GPIO);
}

void network_init(system_config_t *cfg)
{
    (void)cfg;   /* WiFi settings removed — Ethernet is the only interface. */
    s_evt = xEventGroupCreate();

    esp_netif_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Register event handlers BEFORE starting Ethernet — the static-IP
     * IP_EVENT_ETH_GOT_IP fires synchronously during esp_eth_start(),
     * so late registration would miss it. */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                                        &net_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                        &net_event_handler, NULL, NULL));

    /* Ethernet is the only link (WiFi stack disabled in sdkconfig). */
    start_ethernet();
}

bool network_is_up(void)
{
    if (!s_evt) return false;
    return (xEventGroupGetBits(s_evt) & BIT_ETH_CONNECTED) != 0;
}

const char *network_device_ip(void)
{
    return s_device_ip[0] ? s_device_ip : "---";
}

/* (The former network-reset button task and its LED blink were removed along
 * with the WiFi feature — the button's only action was restoring the WiFi AP
 * defaults, which no longer exist.) */