#include "storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "storage";
static tk_config_t s_config;
static tk_state_t s_state;
typedef struct {
    uint16_t count;
    tk_code_request_t requests[TK_MAX_CODE_REQUESTS];
} tk_code_queue_t;
static tk_code_queue_t s_code_queue;
static SemaphoreHandle_t s_mutex;

static esp_err_t save_blob(const char *key, const void *data, size_t size)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open("timekeep", NVS_READWRITE, &handle), TAG, "open nvs");
    esp_err_t err = nvs_set_blob(handle, key, data, size);
    // NVS stores blobs across multiple entries. On long-lived terminals,
    // repeated updates can leave the namespace without a contiguous set of
    // free entries even though the total partition still has room. Reclaim
    // this key and retry once before reporting a real storage failure.
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        ESP_LOGW(TAG, "NVS space low while saving %s; reclaiming old value", key);
        if (nvs_erase_key(handle, key) == ESP_OK) {
            err = nvs_commit(handle);
            if (err == ESP_OK) err = nvs_set_blob(handle, key, data, size);
        }
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t tk_storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "init nvs");
    s_mutex = xSemaphoreCreateMutex();
    nvs_handle_t handle;
    if (nvs_open("timekeep", NVS_READONLY, &handle) == ESP_OK) {
        // Configuration grows as terminal capabilities are added. Read the
        // stored blob at its original size so installed devices migrate
        // safely instead of losing Wi-Fi credentials on a firmware update.
        size_t size = 0;
        if (nvs_get_blob(handle, "config", NULL, &size) == ESP_OK && size) {
            void *saved = calloc(1, size);
            if (saved && nvs_get_blob(handle, "config", saved, &size) == ESP_OK) {
                memcpy(&s_config, saved, size < sizeof(s_config) ? size : sizeof(s_config));
            }
            free(saved);
        }
        // State has changed shape over releases. Read its stored length then
        // copy the shared prefix so a smaller/newer state never wipes cached
        // employees just because a blob grew or shrank.
        size = 0;
        if (nvs_get_blob(handle, "state", NULL, &size) == ESP_OK && size) {
            void *saved = calloc(1, size);
            if (saved && nvs_get_blob(handle, "state", saved, &size) == ESP_OK) {
                memcpy(&s_state, saved, size < sizeof(s_state) ? size : sizeof(s_state));
            }
            free(saved);
        }
        size = sizeof(s_code_queue);
        nvs_get_blob(handle, "code_queue", &s_code_queue, &size);
        nvs_close(handle);
    }
    if (s_state.version != 1) {
        memset(&s_state, 0, sizeof(s_state));
        s_state.version = 1;
    }
    if (s_code_queue.count > TK_MAX_CODE_REQUESTS) memset(&s_code_queue, 0, sizeof(s_code_queue));
    if (!s_config.touch_x_scale) s_config.touch_x_scale = 1000;
    if (!s_config.touch_y_scale) s_config.touch_y_scale = 1000;
    if (s_config.ui_preferences_version != 1) {
        s_config.ui_preferences_version = 1;
        s_config.reduce_motion = false;
        s_config.local_intervals_override = false;
        s_config.local_power_override = false;
    }
    if (!s_config.sync_interval_seconds) s_config.sync_interval_seconds = 5;
    if (!s_config.full_sync_interval_seconds) s_config.full_sync_interval_seconds = 300;
    // Existing installations predate this field, so use a sensible screen
    // sleep default until their first device-settings sync arrives.
    if (!s_config.sleep_timeout_configured) s_config.sleep_timeout_seconds = 120;
    if (!s_config.power_timeouts_configured) {
        s_config.screen_off_timeout_seconds = 30;
        s_config.low_power_timeout_seconds = 120;
    }
    if (!s_config.terminal_theme[0]) strlcpy(s_config.terminal_theme, "light", sizeof(s_config.terminal_theme));
    return ESP_OK;
}

const tk_config_t *tk_config_get(void) { return &s_config; }

esp_err_t tk_config_save(const tk_config_t *config)
{
    esp_err_t err = save_blob("config", config, sizeof(*config));
    if (err == ESP_OK) memcpy(&s_config, config, sizeof(s_config));
    return err;
}

tk_state_t *tk_state_lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    return &s_state;
}

void tk_state_unlock(void) { xSemaphoreGive(s_mutex); }
esp_err_t tk_state_save(void) { return save_blob("state", &s_state, sizeof(s_state)); }

static void digest_code(const char *code, char output[65])
{
    unsigned char digest[32];
    size_t digest_length = 0;
    psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)code, strlen(code), digest, sizeof(digest), &digest_length);
    for (int i = 0; i < 32; ++i) snprintf(output + i * 2, 3, "%02x", digest[i]);
}

const tk_employee_t *tk_find_employee_by_code(const char *code)
{
    static tk_employee_t result;
    char digest[65];
    digest_code(code, digest);
    tk_state_t *state = tk_state_lock();
    bool found = false;
    for (int i = 0; i < state->employee_count; ++i) {
        if (strcmp(state->employees[i].code_digest, digest) == 0) {
            result = state->employees[i];
            found = true;
            break;
        }
    }
    tk_state_unlock();
    return found ? &result : NULL;
}

bool tk_time_is_valid(void)
{
    time_t now;
    time(&now);
    return now > 1704067200; // 2024-01-01
}

void tk_format_utc(char output[25])
{
    time_t now;
    struct tm utc;
    time(&now);
    gmtime_r(&now, &utc);
    strftime(output, 25, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

esp_err_t tk_toggle_employee(const char *employee_id, tk_event_t *created_event, bool *now_clocked_in)
{
    if (!tk_time_is_valid()) return ESP_ERR_INVALID_STATE;
    tk_state_t *state = tk_state_lock();
    int employee_index = -1;
    for (int i = 0; i < state->employee_count; ++i) if (strcmp(state->employees[i].id, employee_id) == 0) employee_index = i;
    if (employee_index < 0 || state->event_count >= TK_MAX_EVENTS) {
        tk_state_unlock();
        return employee_index < 0 ? ESP_ERR_NOT_FOUND : ESP_ERR_NO_MEM;
    }
    tk_employee_t *employee = &state->employees[employee_index];
    employee->clocked_in = !employee->clocked_in;
    tk_event_t *event = &state->events[state->event_count++];
    memset(event, 0, sizeof(*event));
    uint32_t random = esp_random();
    int64_t epoch = time(NULL);
    snprintf(event->id, sizeof(event->id), "%08lx-%lld", (unsigned long)random, (long long)epoch);
    strlcpy(event->employee_id, employee_id, sizeof(event->employee_id));
    tk_format_utc(event->occurred_at);
    event->clock_in = employee->clocked_in;
    if (created_event) *created_event = *event;
    if (now_clocked_in) *now_clocked_in = employee->clocked_in;
    esp_err_t err = tk_state_save();
    tk_state_unlock();
    return err;
}

esp_err_t tk_queue_code_request(const char *code)
{
    if (!code || strlen(code) != 4) return ESP_ERR_INVALID_ARG;
    tk_state_lock();
    if (s_code_queue.count >= TK_MAX_CODE_REQUESTS) { tk_state_unlock(); return ESP_ERR_NO_MEM; }
    tk_code_request_t *request = &s_code_queue.requests[s_code_queue.count++];
    memset(request, 0, sizeof(*request));
    snprintf(request->id, sizeof(request->id), "%08lx-%lld", (unsigned long)esp_random(), (long long)esp_timer_get_time());
    strlcpy(request->code, code, sizeof(request->code));
    esp_err_t err = save_blob("code_queue", &s_code_queue, sizeof(s_code_queue));
    tk_state_unlock();
    return err;
}

bool tk_peek_code_request(tk_code_request_t *request)
{
    if (!request) return false;
    tk_state_lock();
    bool found = s_code_queue.count > 0;
    if (found) *request = s_code_queue.requests[0];
    tk_state_unlock();
    return found;
}

esp_err_t tk_pop_code_request(const char *id)
{
    tk_state_lock();
    if (!s_code_queue.count || !id || strcmp(s_code_queue.requests[0].id, id) != 0) { tk_state_unlock(); return ESP_ERR_NOT_FOUND; }
    memmove(s_code_queue.requests, s_code_queue.requests + 1, (s_code_queue.count - 1) * sizeof(tk_code_request_t));
    s_code_queue.count--;
    esp_err_t err = save_blob("code_queue", &s_code_queue, sizeof(s_code_queue));
    tk_state_unlock();
    return err;
}
