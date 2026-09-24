#include "network_manager.h"
#include "app_config.h"
#include "fault_manager.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
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
#define BIT_STA_CONNECTED  (1 << 0)
#define BIT_ETH_CONNECTED  (1 << 1)
static bool s_sta_mode = false;
static int s_retry = 0;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
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

static void wifi_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        fault_manager_set_network_up(false);
        if (s_retry < 10) {
            esp_wifi_connect();
            s_retry++;
            ESP_LOGW(TAG, "STA reconnect attempt %d", s_retry);
        } else {
            xEventGroupClearBits(s_evt, BIT_STA_CONNECTED);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "AP: station connected");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        s_retry = 0;
        xEventGroupSetBits(s_evt, BIT_STA_CONNECTED);
        fault_manager_set_network_up(true);
        snprintf(s_device_ip, sizeof(s_device_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        ESP_LOGI(TAG, "STA got IP: %s", s_device_ip);
        start_sntp();
        start_mdns();
    } else if (base == ETH_EVENT && id == ETHERNET_EVENT_CONNECTED) {
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

static void start_ap(system_config_t *cfg)
{
    /* Guard only the netif creation; the mode/config/start must run every call
     * so a runtime switch or network reset actually brings the AP back up. */
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
    wifi_config_t wc = {0};
    strncpy((char *)wc.ap.ssid, cfg->wifi_ssid, sizeof(wc.ap.ssid));
    strncpy((char *)wc.ap.password, cfg->wifi_pass, sizeof(wc.ap.password));
    wc.ap.ssid_len = strlen((char *)wc.ap.ssid);
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wc.ap.max_connection = 4;
    if (strlen((char *)wc.ap.password) < 8) wc.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    if (s_device_ip[0] == '\0')
        snprintf(s_device_ip, sizeof(s_device_ip), "192.168.4.1");
    ESP_LOGI(TAG, "AP up: SSID=%s  IP=%s", cfg->wifi_ssid, s_device_ip);
}

static void start_sta(system_config_t *cfg)
{
    if (!s_sta_netif) s_sta_netif = esp_netif_create_default_wifi_sta();
    s_retry = 0;
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, cfg->wifi_ssid, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, cfg->wifi_pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "STA connecting to SSID=%s", cfg->wifi_ssid);
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
        ESP_LOGE(TAG, "No PHY answered on MDIO (MDC=%d MDIO=%d) — falling back to WiFi only",
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
        ESP_LOGE(TAG, "Ethernet driver install failed: %s — falling back to WiFi only",
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

    ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));
    ESP_LOGI(TAG, "Ethernet started (LAN8720 addr=%d, MDC=%d, MDIO=%d)",
             phy_addr, HE_ETH_MDC_GPIO, HE_ETH_MDIO_GPIO);
}

void network_init(system_config_t *cfg)
{
    s_evt = xEventGroupCreate();
    s_sta_mode = cfg->wifi_sta_mode;

    esp_netif_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Register event handlers BEFORE starting Ethernet — the static-IP
     * IP_EVENT_ETH_GOT_IP fires synchronously during esp_eth_start(),
     * so late registration would miss it. */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));

    /* Ethernet is primary on ESP32-ETH01. */
    start_ethernet();

    /* WiFi is the fallback / configuration path. Initialise the radio so
     * it is ready when needed, but only start AP/STA if Ethernet is not
     * up after a short grace period. */
    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wic));

    /* Start WiFi immediately in STA mode if the user configured it; otherwise
     * bring up the AP so the device is reachable even before Ethernet links. */
    if (s_sta_mode && cfg->wifi_ssid[0]) {
        start_sta(cfg);
    } else {
        start_ap(cfg);
    }
}

void network_apply(const system_config_t *cfg)
{
    s_sta_mode = cfg->wifi_sta_mode;
    esp_wifi_stop();
    if (s_sta_mode && cfg->wifi_ssid[0]) {
        start_sta((system_config_t *)cfg);
    } else {
        start_ap((system_config_t *)cfg);
    }
}

bool network_is_up(void)
{
    if (!s_evt) return false;
    return (xEventGroupGetBits(s_evt) & (BIT_STA_CONNECTED | BIT_ETH_CONNECTED)) != 0;
}

const char *network_device_ip(void)
{
    return s_device_ip[0] ? s_device_ip : "---";
}

/* Hardware reset button: hold GPIO0 for 5s => network reset to AP default. */
static void reset_button_task(void *arg)
{
    system_config_t *cfg = (system_config_t *)arg;

    /* Configure the BOOT button as input with pull-up. */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << HE_GPIO_NET_RESET_BTN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    /* Configure the onboard LED as push-pull output, start off. */
    gpio_config_t led = {
        .pin_bit_mask = (1ULL << HE_GPIO_LED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led);
    gpio_set_level(HE_GPIO_LED, 0);

    int held_ms = 0;
    int blink_cnt = 0;   /* 100-ms tick counter for toggling */
    bool led_on = false;

    while (1) {
        if (gpio_get_level(HE_GPIO_NET_RESET_BTN) == 0) {
            held_ms += 100;
            blink_cnt++;

            /* Blink pattern:
             *   0 – 3000 ms : slow (toggle every 500 ms → every 5th tick)
             *   3000 – 5000 ms : fast (toggle every 200 ms → every 2nd tick)
             *   >= 5000 ms : LED solid ON, then reset fires below. */
            int period = (held_ms < 3000) ? 5 : 2;

            if (held_ms >= HE_NET_RESET_HOLD_MS) {
                /* Solid on while the reset runs. */
                gpio_set_level(HE_GPIO_LED, 1);
                ESP_LOGW(TAG, "net reset button held => AP default");
                he_config_lock();
                storage_reset_network(cfg);
                network_apply(cfg);
                he_config_unlock();
                gpio_set_level(HE_GPIO_LED, 0);
                led_on = false;
                held_ms = 0;
                blink_cnt = 0;
                vTaskDelay(pdMS_TO_TICKS(2000));
            } else if (blink_cnt >= period) {
                blink_cnt = 0;
                led_on = !led_on;
                gpio_set_level(HE_GPIO_LED, led_on ? 1 : 0);
            }
        } else {
            /* Button released: reset everything. */
            held_ms = 0;
            blink_cnt = 0;
            if (led_on) { gpio_set_level(HE_GPIO_LED, 0); led_on = false; }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void network_start_reset_button(system_config_t *cfg)
{
    xTaskCreate(reset_button_task, "netbtn", 3072, cfg, 5, NULL);
}