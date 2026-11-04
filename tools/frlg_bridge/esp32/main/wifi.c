/* The board's own network (see wifi.h). The settings are fixed here for
 * now: the Switch leg will replace the password with the GBA app's own
 * keys, and the SSID with the one its scan expects, so nothing to make
 * configurable yet. Channel 1: the 3DS's wireless is 2.4 GHz only. */
#include "wifi.h"

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <stdio.h>
#include <string.h>

#include "bridge.h"

#define AP_PASSWORD "emerald3ds" /* WPA2-PSK needs eight or more */

void wifi_start_ap(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t ap = {0};
    uint8_t mac[6] = {0};

    esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    memcpy(ap.ssid, "EM3DS", 5);
    snprintf((char *)ap.ssid + 5, sizeof(ap.ssid) - 5, "-%02X%02X%02X",
             mac[3], mac[4], mac[5]);
    ap.ssid_len = strlen((char *)ap.ssid);
    memcpy(ap.password, AP_PASSWORD, sizeof(AP_PASSWORD));
    ap.ap.channel = 1;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 4;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI("bridge", "the network is up: %s, channel %u", (char *)ap.ssid, 1);
}
