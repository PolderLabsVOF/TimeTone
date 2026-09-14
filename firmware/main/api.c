#include "api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network.h"
#include "storage.h"
#include "display.h"

static const char *TAG = "api";
// HTTP, TLS and the largest working sets in the firmware all run on this task.
// Its biggest single frame is under a kilobyte, so the size below is headroom
// against the unexpected: an mbedTLS handshake, cJSON recursion, and the
// interrupt frames that land on whichever task is running. Raising it from 8192
// spends 12,288 bytes of DRAM heap; the headroom log below is what tells us
// whether that was needed, before an overflow corrupts the heap instead.
#define TK_API_TASK_STACK 20480
static SemaphoreHandle_t s_wake;
static bool s_ota_in_progress;
static volatile bool s_force_config;
static volatile bool s_code_requested;
static volatile bool s_warm_requested;
static TickType_t s_next_pair_attempt;
static volatile bool s_clock_request_in_flight;
static uint8_t s_sync_failures;
// A colour code is the terminal's interactive path. Retain its HTTP client so
// TLS is negotiated once rather than again for every employee. Background
// calls stay one-shot: some reverse proxies close their idle HTTP/1.1 sockets,
// and reusing those sockets can incorrectly trap a terminal in retry mode.
static esp_http_client_handle_t s_clock_client;
static char s_clock_client_server[160];
static bool s_clock_connection_warmed;

typedef struct { char *data; size_t length; size_t capacity; } response_buffer_t;
static void ota_task(void *argument);

static bool tick_due(TickType_t now, TickType_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static TickType_t seconds_to_ticks(uint16_t seconds, uint16_t fallback)
{
    uint32_t effective_seconds = seconds ? seconds : fallback;
    return pdMS_TO_TICKS(effective_seconds * 1000U);
}

static TickType_t retry_ticks(uint16_t base_seconds)
{
    const uint8_t exponent = s_sync_failures > 4 ? 4 : s_sync_failures;
    uint32_t seconds = (uint32_t)(base_seconds ? base_seconds : 5) << exponent;
    if (seconds > 60) seconds = 60;
    return pdMS_TO_TICKS(seconds * 1000U);
}

static void discard_clock_client(void)
{
    if (s_clock_client) esp_http_client_cleanup(s_clock_client);
    s_clock_client = NULL;
    s_clock_client_server[0] = 0;
    s_clock_connection_warmed = false;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    response_buffer_t *buffer = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && buffer && event->data_len > 0) {
        if (buffer->length + event->data_len + 1 > buffer->capacity) return ESP_ERR_NO_MEM;
        memcpy(buffer->data + buffer->length, event->data, event->data_len);
        buffer->length += event->data_len;
        buffer->data[buffer->length] = 0;
    }
    return ESP_OK;
}

static int request(const char *path, esp_http_client_method_t method, const char *body, char *response, size_t response_size)
{
    const tk_config_t *config = tk_config_get();
    if (!config->server_url[0] || (strncmp(config->server_url, "https://", 8) != 0 && strncmp(config->server_url, "http://", 7) != 0)) return -1;
    char url[256];
    snprintf(url, sizeof(url), "%s%s", config->server_url, path);
    response_buffer_t buffer = { .data = response, .capacity = response_size };
    response[0] = 0;
    bool is_clock = strstr(path, "/clock") != NULL;
    esp_http_client_config_t client_config = {
        .url = url,
        .method = method,
        // Keep the 5-second health loop from monopolizing the keypad when a
        // server is unreachable; larger config payloads retain a longer
        // timeout for slower networks.
        // HTTPS through a reverse proxy includes DNS, TCP and TLS setup.
        // A tiny heartbeat timeout caused otherwise healthy terminals to flap
        // on busy Wi-Fi. These limits stay bounded while allowing a full
        // connection setup to complete reliably.
        // Clock requests are the interactive path. Four and a half seconds is
        // enough for DNS/TLS on the supported networks, while a dead route
        // fails quickly instead of leaving the next person waiting for 10s.
        .timeout_ms = strstr(path, "/heartbeat") ? 8000 : strstr(path, "/clock") ? 4500 : strstr(path, "/events") ? 10000 : 15000,
        .keep_alive_enable = true,
        .event_handler = http_event,
        .user_data = &buffer,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    // Settings may change the server URL from the terminal web UI. Dispose of
    // the retained interactive connection only in that case.
    if (s_clock_client && strcmp(s_clock_client_server, config->server_url) != 0) {
        discard_clock_client();
    }
    esp_http_client_handle_t client = is_clock ? s_clock_client : NULL;
    if (!client) {
        client = esp_http_client_init(&client_config);
        if (is_clock && client) {
            s_clock_client = client;
            strlcpy(s_clock_client_server, config->server_url, sizeof(s_clock_client_server));
        }
    }
    if (!client) return -1;
    // Reusing a client requires updating its per-request values explicitly.
    esp_http_client_set_url(client, url);
    esp_http_client_set_method(client, method);
    esp_http_client_set_timeout_ms(client, is_clock ? 4500 : client_config.timeout_ms);
    esp_http_client_set_user_data(client, &buffer);
    char auth[150];
    snprintf(auth, sizeof(auth), "Bearer %s", config->device_token);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "TimeTone-Terminal/" TK_FIRMWARE_VERSION);
    esp_http_client_set_post_field(client, body, body ? strlen(body) : 0);
    int64_t started_us = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(client);
    // A 401 response can make esp_http_client return ESP_ERR_NOT_SUPPORTED
    // before ESP_OK (it attempts an auth challenge). Preserve the HTTP status
    // so the pairing handshake can still run for a new token.
    int status = esp_http_client_get_status_code(client);
    if (status <= 0) status = err == ESP_OK ? 200 : -1;
    int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000;
    if (err != ESP_OK) ESP_LOGW(TAG, "%s failed after %lldms: %s", path, elapsed_ms, esp_err_to_name(err));
    else if (strstr(path, "/clock") && elapsed_ms > 1000) ESP_LOGW(TAG, "slow clock request: %lldms", elapsed_ms);
    if (!is_clock) esp_http_client_cleanup(client);
    return status;
}

static esp_err_t fetch_config(void)
{
    char *response = malloc(18000);
    if (!response) return ESP_ERR_NO_MEM;
    int status = request("/api/device/v1/config", HTTP_METHOD_GET, NULL, response, 18000);
    if (status != 200) { free(response); return ESP_FAIL; }
    cJSON *root = cJSON_Parse(response);
    free(response);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    cJSON *employees = cJSON_GetObjectItem(root, "employees");
    if (!cJSON_IsArray(employees)) { cJSON_Delete(root); return ESP_ERR_INVALID_RESPONSE; }
    tk_state_t *state = tk_state_lock();
    state->employee_count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, employees) {
        if (state->employee_count >= TK_MAX_EMPLOYEES) break;
        cJSON *id = cJSON_GetObjectItem(item, "id"), *name = cJSON_GetObjectItem(item, "name");
        cJSON *digest = cJSON_GetObjectItem(item, "codeDigest"), *clocked = cJSON_GetObjectItem(item, "clockedIn");
        if (!cJSON_IsString(id) || !cJSON_IsString(name)) continue;
        tk_employee_t *employee = &state->employees[state->employee_count++];
        memset(employee, 0, sizeof(*employee));
        strlcpy(employee->id, id->valuestring, sizeof(employee->id));
        strlcpy(employee->name, name->valuestring, sizeof(employee->name));
        if (cJSON_IsString(digest)) strlcpy(employee->code_digest, digest->valuestring, sizeof(employee->code_digest));
        employee->clocked_in = cJSON_IsTrue(clocked);
    }
    esp_err_t err = tk_state_save();
    tk_state_unlock();
    cJSON *settings = cJSON_GetObjectItem(root, "settings");
    if (cJSON_IsObject(settings)) {
        cJSON *interval = cJSON_GetObjectItem(settings, "syncIntervalSeconds");
        cJSON *full_interval = cJSON_GetObjectItem(settings, "fullSyncIntervalSeconds");
        cJSON *screen_off_timeout = cJSON_GetObjectItem(settings, "screenOffTimeoutSeconds");
        cJSON *low_power_timeout = cJSON_GetObjectItem(settings, "lowPowerTimeoutSeconds");
        cJSON *theme = cJSON_GetObjectItem(settings, "terminalTheme");
        cJSON *timezone = cJSON_GetObjectItem(settings, "timezone");
        cJSON *company_name = cJSON_GetObjectItem(settings, "companyName");
        tk_config_t updated = *tk_config_get();
        bool changed = false;
        if (!updated.local_intervals_override && cJSON_IsNumber(interval)) {
            uint16_t seconds = (uint16_t)(interval->valuedouble < 2 ? 2 : interval->valuedouble > 60 ? 60 : interval->valuedouble);
            if (updated.sync_interval_seconds != seconds) { updated.sync_interval_seconds = seconds; changed = true; }
        }
        if (!updated.local_intervals_override && cJSON_IsNumber(full_interval)) {
            uint16_t seconds = (uint16_t)(full_interval->valuedouble < 30 ? 30 : full_interval->valuedouble > 3600 ? 3600 : full_interval->valuedouble);
            if (updated.full_sync_interval_seconds != seconds) { updated.full_sync_interval_seconds = seconds; changed = true; }
        }
        if (!updated.local_power_override && cJSON_IsNumber(screen_off_timeout) && cJSON_IsNumber(low_power_timeout)) {
            uint16_t screen_seconds = (uint16_t)(screen_off_timeout->valuedouble < 0 ? 0 : screen_off_timeout->valuedouble > 3600 ? 3600 : screen_off_timeout->valuedouble);
            uint16_t low_power_seconds = (uint16_t)(low_power_timeout->valuedouble < 0 ? 0 : low_power_timeout->valuedouble > 3600 ? 3600 : low_power_timeout->valuedouble);
            if (low_power_seconds && screen_seconds && low_power_seconds < screen_seconds) low_power_seconds = screen_seconds;
            if (updated.screen_off_timeout_seconds != screen_seconds || updated.low_power_timeout_seconds != low_power_seconds || !updated.power_timeouts_configured) {
                updated.screen_off_timeout_seconds = screen_seconds;
                updated.low_power_timeout_seconds = low_power_seconds;
                updated.power_timeouts_configured = true;
                changed = true;
            }
        }
        if (!updated.terminal_theme_override && cJSON_IsString(theme) && (strcmp(theme->valuestring, "dark") == 0 || strcmp(theme->valuestring, "light") == 0) && strcmp(updated.terminal_theme, theme->valuestring) != 0) {
            strlcpy(updated.terminal_theme, theme->valuestring, sizeof(updated.terminal_theme));
            changed = true;
        }
        if (cJSON_IsString(timezone) && strcmp(updated.timezone, timezone->valuestring) != 0 && tk_time_apply_timezone(timezone->valuestring)) {
            strlcpy(updated.timezone, timezone->valuestring, sizeof(updated.timezone));
            changed = true;
        }
        if (changed) {
            esp_err_t save_err = tk_config_save(&updated);
            if (save_err == ESP_OK) tk_display_apply_settings();
            else ESP_LOGW(TAG, "could not persist device settings: %s", esp_err_to_name(save_err));
        }
        if (cJSON_IsString(company_name)) tk_display_set_company_name(company_name->valuestring);
    }
    cJSON *firmware_update = cJSON_GetObjectItem(root, "firmwareUpdate");
    if (!s_ota_in_progress && cJSON_IsObject(firmware_update)) {
        cJSON *version = cJSON_GetObjectItem(firmware_update, "version");
        cJSON *url = cJSON_GetObjectItem(firmware_update, "url");
        if (cJSON_IsString(version) && cJSON_IsString(url) && strcmp(version->valuestring, TK_FIRMWARE_VERSION) != 0 && strncmp(url->valuestring, "https://", 8) == 0 && strlen(url->valuestring) < 512) {
            char *ota_url = strdup(url->valuestring);
            if (ota_url) {
                s_ota_in_progress = true;
                tk_display_show_ota(version->valuestring);
                if (xTaskCreate(ota_task, "timekeep_ota", 8192, ota_url, 5, NULL) != pdPASS) {
                    ESP_LOGE(TAG, "could not start OTA task");
                    free(ota_url);
                    s_ota_in_progress = false;
                    tk_display_finish_ota(false);
                }
            }
        }
    }
    cJSON_Delete(root);
    tk_display_refresh();
    return err;
}

static void ota_task(void *argument)
{
    char *url = argument;
    ESP_LOGI(TAG, "starting OTA update from %s", url);
    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_https_ota_config_t ota_config = { .http_config = &http_config };
    esp_err_t err = esp_https_ota(&ota_config);
    free(url);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA update complete; restarting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(err));
    s_ota_in_progress = false;
    tk_display_finish_ota(false);
    vTaskDelete(NULL);
}

esp_err_t tk_api_submit_code(const char *code, char employee_name[48], bool *clocked_in)
{
    char body[96];
    snprintf(body, sizeof(body), "{\"code\":\"%s\"}", code);
    char response[512];
    s_clock_request_in_flight = true;
    int status = request("/api/device/v1/clock", HTTP_METHOD_POST, body, response, sizeof(response));
    s_clock_request_in_flight = false;
    if (status == 404) return ESP_ERR_NOT_FOUND;
    if (status == 401) return ESP_ERR_INVALID_STATE;
    if (status != 200) return ESP_FAIL;
    cJSON *root = cJSON_Parse(response);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    cJSON *name = cJSON_GetObjectItem(root, "employeeName"), *clocked = cJSON_GetObjectItem(root, "clockedIn");
    if (!cJSON_IsString(name) || !cJSON_IsBool(clocked)) { cJSON_Delete(root); return ESP_ERR_INVALID_RESPONSE; }
    strlcpy(employee_name, name->valuestring, 48);
    *clocked_in = cJSON_IsTrue(clocked);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t tk_api_queue_code(const char *code)
{
    return tk_queue_code_request(code);
}

static esp_err_t push_code_requests(void)
{
    tk_code_request_t queued;
    if (!tk_peek_code_request(&queued)) return ESP_ERR_NOT_FOUND;
    char body[160], response[512];
    snprintf(body, sizeof(body), "{\"code\":\"%s\",\"requestId\":\"%s\"}", queued.code, queued.id);
    int status = request("/api/device/v1/clock", HTTP_METHOD_POST, body, response, sizeof(response));
    if (status == 404) {
        tk_pop_code_request(queued.id);
        tk_display_submission_status("Code not recognised", 0xC43D3D);
        return ESP_OK;
    }
    if (status != 200) {
        tk_display_submission_status("Saved - retrying", 0xC47B24);
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(response);
    cJSON *name = root ? cJSON_GetObjectItem(root, "employeeName") : NULL;
    cJSON *clocked = root ? cJSON_GetObjectItem(root, "clockedIn") : NULL;
    if (!root || !cJSON_IsString(name) || !cJSON_IsBool(clocked)) {
        if (root) cJSON_Delete(root);
        tk_display_submission_status("Saved - retrying", 0xC47B24);
        return ESP_FAIL;
    }
    bool clocked_in = cJSON_IsTrue(clocked);
    char message[96];
    snprintf(message, sizeof(message), "%s, %s!", clocked_in ? "Welcome" : "Goodbye", name->valuestring);
    cJSON_Delete(root);
    tk_pop_code_request(queued.id);
    tk_display_submission_status(message, clocked_in ? 0x168455 : 0x526159);
    return ESP_OK;
}

// The upload reply carries one result object per event sent, and request()'s
// collector fails the whole request (ESP_ERR_NO_MEM from http_event) when the
// body does not fit, so this cannot be shrunk below a full batch of
// acknowledgements.
#define TK_EVENTS_RESPONSE_MAX 4096

// An event upload needs the events snapshot the body is built from, a 4 KiB
// response body, and the rollback copy tk_state_save() restores on failure.
// Declared as locals they were 17,392 bytes of frame in push_events(), on a
// task whose stack was 8,192 bytes: the frame ran off the bottom of the stack
// into the heap underneath it. push_events() is only reachable from the
// connected sync path (api_task), which is why the terminal reset only while
// connected, and with CONFIG_ESP_COREDUMP_ENABLE_TO_NONE it left no evidence.
// The working set is now owned by push_events() and passed in.
static esp_err_t upload_events(tk_event_t *events, char *response, tk_event_t *original_events)
{
    int count;
    tk_state_t *state = tk_state_lock();
    count = state->event_count;
    memcpy(events, state->events, count * sizeof(tk_event_t));
    tk_state_unlock();
    if (!count) return ESP_OK;
    cJSON *root = cJSON_CreateObject(), *array = cJSON_AddArrayToObject(root, "events");
    cJSON_AddNumberToObject(root, "pendingCount", count);
    for (int i = 0; i < count; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", events[i].id);
        cJSON_AddStringToObject(item, "employeeId", events[i].employee_id);
        cJSON_AddStringToObject(item, "type", events[i].clock_in ? "CLOCK_IN" : "CLOCK_OUT");
        cJSON_AddStringToObject(item, "occurredAt", events[i].occurred_at);
        cJSON_AddItemToArray(array, item);
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    int status = request("/api/device/v1/events", HTTP_METHOD_POST, body, response, TK_EVENTS_RESPONSE_MAX);
    free(body);
    if (status != 200) return ESP_FAIL;

    // The server acknowledges every uploaded event separately. A transport
    // success does not mean every item was accepted (for example, an employee
    // can have been deactivated while this terminal was offline), so never
    // discard unacknowledged or rejected events from the local durable queue.
    cJSON *root_response = cJSON_Parse(response);
    cJSON *results = root_response ? cJSON_GetObjectItem(root_response, "results") : NULL;
    if (!cJSON_IsArray(results)) {
        if (root_response) cJSON_Delete(root_response);
        ESP_LOGW(TAG, "event upload response had no acknowledgements; retaining queue");
        return ESP_ERR_INVALID_RESPONSE;
    }
    bool acknowledged[TK_MAX_EVENTS] = { false };
    cJSON *result;
    cJSON_ArrayForEach(result, results) {
        cJSON *id = cJSON_GetObjectItem(result, "id");
        cJSON *result_status = cJSON_GetObjectItem(result, "status");
        if (!cJSON_IsString(id) || !cJSON_IsString(result_status)) continue;
        for (int i = 0; i < count; ++i) {
            if (strcmp(events[i].id, id->valuestring) != 0) continue;
            if (strcmp(result_status->valuestring, "accepted") == 0 ||
                strcmp(result_status->valuestring, "duplicate") == 0 ||
                strcmp(result_status->valuestring, "ignored") == 0) {
                acknowledged[i] = true;
            } else {
                ESP_LOGW(TAG, "server rejected queued event %s; retaining it for review", events[i].id);
            }
            break;
        }
    }
    cJSON_Delete(root_response);

    state = tk_state_lock();
    uint16_t original_count = state->event_count;
    memcpy(original_events, state->events, original_count * sizeof(tk_event_t));
    int removed = 0;
    int retained = 0;
    for (int i = 0; i < state->event_count; ++i) {
        bool sent_event = false;
        bool accepted = false;
        for (int j = 0; j < count; ++j) {
            if (strcmp(state->events[i].id, events[j].id) == 0) {
                sent_event = true;
                accepted = acknowledged[j];
                break;
            }
        }
        if (sent_event && accepted) {
            removed++;
            continue;
        }
        if (retained != i) state->events[retained] = state->events[i];
        retained++;
    }
    if (removed) {
        state->event_count = retained;
        esp_err_t save_err = tk_state_save();
        if (save_err != ESP_OK) {
            memcpy(state->events, original_events, original_count * sizeof(tk_event_t));
            state->event_count = original_count;
            tk_state_unlock();
            return save_err;
        }
    }
    tk_state_unlock();
    return ESP_OK;
}

// Owns the working set so every exit path from upload_events() frees it.
//
// Heap rather than file-scope statics on purpose: statics would spend the same
// ~17 KiB permanently out of the ~57 KiB of DRAM that Wi-Fi, mbedTLS and every
// task stack share at run time, while the heap cost here lasts only for the
// duration of the upload. That 17 KiB peak is the same size as the 18 KiB
// buffer fetch_config() already allocates on this task, and the two never
// overlap. Allocation failure is survivable: the queue is durable in NVS and
// the caller retries on the next healthy heartbeat.
static esp_err_t push_events(void)
{
    const size_t events_size = TK_MAX_EVENTS * sizeof(tk_event_t);
    tk_event_t *events = malloc(events_size);
    char *response = malloc(TK_EVENTS_RESPONSE_MAX);
    tk_event_t *original_events = malloc(events_size);
    if (!events || !response || !original_events) {
        // Not fatal: the queue is durable in NVS and the next healthy
        // heartbeat retries the upload.
        ESP_LOGW(TAG, "no heap for event upload scratch (%u bytes); will retry",
                 (unsigned)(2 * events_size + TK_EVENTS_RESPONSE_MAX));
        free(events);
        free(response);
        free(original_events);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = upload_events(events, response, original_events);
    free(events);
    free(response);
    free(original_events);
    return err;
}

static int warm_clock_connection(void)
{
    if (s_clock_connection_warmed) return 200;
    char response[96];
    int status = request("/api/device/v1/clock", HTTP_METHOD_POST, "{\"warmup\":true}", response, sizeof(response));
    // Even a legacy server returning 400 has completed DNS/TCP/TLS and left
    // the retained client ready for the real colour-code request.
    if (status > 0) {
        s_clock_connection_warmed = true;
        ESP_LOGI(TAG, "clock connection warmed");
    }
    return status;
}

// The push_events() overflow corrupted the heap and rebooted with no evidence,
// so log the headroom the health pass actually has (and the heap, which is the
// other budget this task competes for) rather than discovering the next one by
// crash. uxTaskGetStackHighWaterMark() is the smallest free stack ever seen, in
// words. Rate-limited: the health pass can run as often as every few seconds.
#define TK_HEALTH_LOG_INTERVAL_US (60 * 1000000LL)
static void log_task_headroom(void)
{
    static int64_t s_last_log_us;
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_log_us < TK_HEALTH_LOG_INTERVAL_US) return;
    s_last_log_us = now_us;
    ESP_LOGI(TAG, "api task stack headroom %u of %u bytes; heap free %u, minimum %u",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
             (unsigned)TK_API_TASK_STACK,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
}

static int heartbeat(void)
{
    int pending;
    tk_state_t *state = tk_state_lock(); pending = state->event_count; tk_state_unlock();
    char ip[16]; tk_network_ip(ip, sizeof(ip));
    char body[220];
    snprintf(body, sizeof(body), "{\"firmwareVersion\":\"%s\",\"ipAddress\":\"%s\",\"pendingEvents\":%d}", TK_FIRMWARE_VERSION, ip, pending);
    char response[256];
    int status = request("/api/device/v1/heartbeat", HTTP_METHOD_POST, body, response, sizeof(response));
    if (status == 200) {
        cJSON *root = cJSON_Parse(response);
        if (root) {
            if (cJSON_IsTrue(cJSON_GetObjectItem(root, "configRefresh"))) s_force_config = true;
            cJSON_Delete(root);
        }
        // Warm the dedicated keep-alive client while the terminal is idle.
        // This performs DNS/TCP/TLS before a person reaches the keypad; the
        // response has no timekeeping side effect.
        warm_clock_connection();
        return status;
    }
    if (status == 401) {
        // Pairing an unapproved device more than once every 30 seconds adds
        // load without making approval faster. The regular health loop still
        // notices approval immediately on its next pass.
        if (xTaskGetTickCount() < s_next_pair_attempt) return status;
        s_next_pair_attempt = xTaskGetTickCount() + pdMS_TO_TICKS(30000);
        char pair_body[320];
        snprintf(pair_body, sizeof(pair_body), "{\"deviceName\":\"%s\",\"token\":\"%s\",\"firmwareVersion\":\"%s\",\"ipAddress\":\"%s\"}", tk_network_setup_ssid(), tk_config_get()->device_token, TK_FIRMWARE_VERSION, ip);
        int pair_status = request("/api/device/v1/pair", HTTP_METHOD_POST, pair_body, response, sizeof(response));
        if (pair_status == 202) ESP_LOGW(TAG, "device pairing requested; approve it in the server dashboard");
        else if (pair_status == 200) {
            // Approval was already in place — the first heartbeat raced
            // ahead of the server updating its token table. Retry once so
            // the caller sees 200 and the display flips to ONLINE without
            // waiting on a manual Sync Now tap. If the retry still 401s
            // (e.g. server replication lag) keep returning the original
            // 401 and let the next scheduled attempt recover.
            ESP_LOGI(TAG, "device pairing already approved");
            int retry_status = request("/api/device/v1/heartbeat", HTTP_METHOD_POST, body, response, sizeof(response));
            if (retry_status == 200) return retry_status;
        }
    }
    return status;
}

static void api_task(void *argument)
{
    bool first_sync = true;
    TickType_t next_health = 0;
    TickType_t next_config = 0;
    while (true) {
        const tk_config_t *config = tk_config_get();
        TickType_t now = xTaskGetTickCount();
        if (!next_health) next_health = now;
        if (!next_config) next_config = now;
        TickType_t deadline = tick_due(next_health, next_config) ? next_config : next_health;
        TickType_t wait = tick_due(now, deadline) ? 0 : deadline - now;
        xSemaphoreTake(s_wake, wait);

        now = xTaskGetTickCount();
        bool refresh_requested = s_force_config;
        bool code_requested = s_code_requested;
        bool warm_requested = s_warm_requested;
        s_force_config = false;
        s_code_requested = false;
        s_warm_requested = false;
        bool health_due = tick_due(now, next_health);
        bool config_due = tick_due(now, next_config);
        if (!tk_network_connected() || !tk_config_get()->configured || tk_display_is_sleeping() || s_clock_request_in_flight) {
            if (refresh_requested) tk_display_set_network_state(TK_DISPLAY_CONNECTING);
            if (code_requested) tk_display_submission_status("Saved - retry when online", 0xC47B24);
            if (health_due) next_health = now + retry_ticks(config->sync_interval_seconds);
            if (config_due) next_config = now + pdMS_TO_TICKS(30000);
            continue;
        }

        if (refresh_requested || health_due || config_due) {
            int health_status = heartbeat();
            if (health_status == 200) {
                s_sync_failures = 0;
                log_task_headroom();
                next_health = now + seconds_to_ticks(config->sync_interval_seconds, 5);
                // heartbeat() sets this when the dashboard explicitly requests
                // a sync. Consume it in this pass so a stable connection picks
                // up remote changes without waiting for a disconnect or wake.
                bool server_requested_config = s_force_config;
                s_force_config = false;
                if (first_sync || refresh_requested || config_due || server_requested_config) {
                    tk_display_set_network_state(TK_DISPLAY_SYNCING);
                    if (fetch_config() == ESP_OK) {
                        first_sync = false;
                        next_config = now + seconds_to_ticks(tk_config_get()->full_sync_interval_seconds, 300);
                    } else {
                        // Keep the cached terminal state and retry a broken
                        // config route calmly; health checks continue normally.
                        next_config = now + pdMS_TO_TICKS(30000);
                        ESP_LOGW(TAG, "configuration sync failed; retaining local cache and retrying in 30s");
                    }
                }
                if (push_events() != ESP_OK) ESP_LOGW(TAG, "queued event upload failed; will retry");
                tk_display_set_network_state(TK_DISPLAY_ONLINE);
            } else {
                if (s_sync_failures < 5) s_sync_failures++;
                next_health = now + retry_ticks(config->sync_interval_seconds);
                if (config_due) next_config = now + pdMS_TO_TICKS(30000);
                tk_display_set_network_state(health_status == 401 ? TK_DISPLAY_CONNECTING : TK_DISPLAY_SYNC_RETRYING);
            }
        }
        // Waking from low power primes only the retained interactive client.
        // It does not fetch settings or perform the removed periodic sync.
        if (warm_requested) warm_clock_connection();
        // A colour-code entry is the only routine server request after setup.
        // A persisted code is retried after every healthy heartbeat, which
        // recovers submissions made during an outage even when nobody taps
        // the terminal again. Request IDs make these retries idempotent.
        if (code_requested || (health_due && !s_sync_failures)) {
            esp_err_t code_result = push_code_requests();
            if (code_result == ESP_OK) tk_display_set_network_state(TK_DISPLAY_ONLINE);
            else if (code_result != ESP_ERR_NOT_FOUND) tk_display_set_network_state(TK_DISPLAY_SYNC_RETRYING);
        }
    }
}

esp_err_t tk_api_start(void)
{
    s_wake = xSemaphoreCreateBinary();
    if (!s_wake) return ESP_ERR_NO_MEM;
    if (xTaskCreate(api_task, "timekeep_api", TK_API_TASK_STACK, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    s_force_config = true;
    xSemaphoreGive(s_wake);
    return ESP_OK;
}

void tk_api_wake(void) { s_clock_connection_warmed = false; s_force_config = true; if (s_wake) xSemaphoreGive(s_wake); }
void tk_api_resume(void)
{
    // A proxy commonly drops an idle HTTP keep-alive while the terminal is in
    // low power. Keeping that socket makes the first colour code after wake
    // wait for a TCP timeout. Start a clean TLS connection during wake-up,
    // before anyone reaches the keypad, and independently validate Wi-Fi.
    // A touch wake (or sleep-cycle wake) is also the cheapest moment to
    // refresh server config: the radio and the user are both already there,
    // so kick off a full heartbeat + config fetch instead of only warming
    // the socket.
    tk_network_resume();
    if (!s_clock_request_in_flight) discard_clock_client();
    s_warm_requested = true;
    s_force_config = true;
    if (s_wake) xSemaphoreGive(s_wake);
}
void tk_api_poke(void) { s_code_requested = true; if (s_wake) xSemaphoreGive(s_wake); }
