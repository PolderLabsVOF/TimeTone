#include <stdlib.h>
#include <time.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "api.h"
#include "display.h"
#include "network.h"
#include "storage.h"

static const char *TAG = "timekeep";

void app_main(void)
{
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    ESP_ERROR_CHECK(tk_storage_init());
    // Preserve the CET default if an old installation has no timezone or a
    // configured IANA zone is outside the terminal's embedded rule set.
    tk_time_apply_timezone(tk_config_get()->timezone);
    ESP_ERROR_CHECK(tk_network_init());
    ESP_ERROR_CHECK(tk_display_init());
    ESP_ERROR_CHECK(tk_api_start());
    if (!tk_config_get()->configured) tk_display_show_setup();
    else tk_display_show_startup();
    ESP_LOGI(TAG, "TimeTone %s ready", TK_FIRMWARE_VERSION);
}
