/* The board's main: the network up, then the bridge's whole life in
 * app_main's own task. The Switch leg (LDN) starts beside it once it
 * exists. */
#include <esp_log.h>
#include <nvs_flash.h>
#include <stdio.h>

#include "bridge.h"
#include "wifi.h"

void app_main(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    wifi_start_ap();
    /* The Direct Corner (the trades); the counters' kinds are the game's. */
    bridge_init(2, true, false);
    if (bridge_bind() != 0)
    {
        ESP_LOGE("bridge", "no socket: the bridge is down");
        return;
    }
    bridge_run_loop();
}
