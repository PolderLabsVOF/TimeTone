#include "network.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "storage.h"
#include "display.h"
#include "api.h"

static const char *TAG = "network";
static EventGroupHandle_t s_events;
static const int CONNECTED_BIT = BIT0;
static char s_ap_ssid[33];
static char s_ip[16] = "0.0.0.0";
static bool s_ap_started;
static uint8_t s_connect_attempts;
static bool s_fallback_active;
static httpd_handle_t s_server;
static esp_timer_handle_t s_reconnect_timer;

// Reconnecting directly from WIFI_EVENT_STA_DISCONNECTED creates a tight
// event-loop loop when a router is unavailable or rejects the credentials.
// Keep exactly one delayed retry outstanding and back off to protect both the
// station and the access point while still recovering without intervention.
static uint32_t reconnect_delay_ms(void)
{
    const uint8_t exponent = s_connect_attempts > 5 ? 5 : s_connect_attempts;
    return 1000U << exponent;
}

static void schedule_reconnect(uint32_t delay_ms)
{
    if (!s_reconnect_timer) return;
    esp_err_t stop_err = esp_timer_stop(s_reconnect_timer);
    if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "could not reset reconnect timer: %s", esp_err_to_name(stop_err));
    }
    // esp_timer requires a positive timeout; one millisecond is effectively
    // immediate while still keeping connects off the event callback.
    uint64_t timeout_us = delay_ms ? (uint64_t)delay_ms * 1000ULL : 1000ULL;
    esp_err_t start_err = esp_timer_start_once(s_reconnect_timer, timeout_us);
    if (start_err != ESP_OK) ESP_LOGW(TAG, "could not schedule reconnect: %s", esp_err_to_name(start_err));
}

static void reconnect_timer_callback(void *argument)
{
    if (tk_network_connected()) return;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi reconnect could not start: %s", esp_err_to_name(err));
        schedule_reconnect(reconnect_delay_ms());
    }
}

static const char SETUP_HTML_HEAD[] =
"<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>TimeTone terminal setup</title><style>*{box-sizing:border-box}body{margin:0;min-height:100vh;background:radial-gradient(circle at 15%% 0,#2b493a 0,#101813 48%%);color:#edf4ec;font:16px system-ui,-apple-system,sans-serif}main{max-width:520px;margin:5vh auto;padding:18px}.shell{background:#f6f8f4;color:#17211b;border-radius:24px;padding:30px;box-shadow:0 24px 70px #0007}.brand{display:flex;align-items:center;gap:10px;color:#516157;font-size:12px;letter-spacing:.13em;font-weight:800}.mark{display:grid;place-items:center;width:28px;height:28px;border-radius:9px;background:#d8ff62;color:#17211b;font-size:16px;letter-spacing:0}h1{margin:18px 0 8px;font-size:29px;letter-spacing:-.04em}p{color:#657068;line-height:1.55;margin:0}.address{margin:18px 0 2px;padding:12px 14px;border-radius:12px;background:#eaf0e6;color:#334139;font-size:14px}fieldset{border:0;padding:0;margin:0}legend{font-size:11px;letter-spacing:.13em;text-transform:uppercase;color:#657068;margin-top:27px;font-weight:800}label{display:block;margin:17px 0 7px;font-size:13px;font-weight:700;color:#3e4b43}input,select{box-sizing:border-box;width:100%%;padding:13px;border:1px solid #d4ddd3;border-radius:11px;font:16px system-ui;background:#fff;color:#17211b;outline:0}input:focus,select:focus{border-color:#86a631;box-shadow:0 0 0 3px #d8ff6280}button{width:100%%;margin-top:28px;padding:14px;border:0;border-radius:12px;background:#d8ff62;color:#17211b;font-weight:800;font-size:16px;cursor:pointer}.foot{margin-top:18px;font-size:13px;color:#788078}@media(max-width:560px){main{margin:0;padding:0}.shell{min-height:100vh;border-radius:0;padding:24px}}</style></head>";

static void html_escape(char *output, size_t size, const char *input)
{
    size_t j = 0;
    for (size_t i = 0; input && input[i] && j + 1 < size; ++i) {
        const char *replacement = input[i] == '&' ? "&amp;" : input[i] == '"' ? "&quot;" : input[i] == '<' ? "&lt;" : input[i] == '>' ? "&gt;" : NULL;
        if (replacement) { size_t n = strlen(replacement); if (j + n >= size) break; memcpy(output + j, replacement, n); j += n; }
        else output[j++] = input[i];
    }
    output[j] = 0;
}

static esp_err_t root_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html");
    const tk_config_t *config = tk_config_get();
    char ssid[96], server[320], timezone[140], ip[16];
    html_escape(ssid, sizeof(ssid), config->ssid); html_escape(server, sizeof(server), config->server_url); html_escape(timezone, sizeof(timezone), config->timezone); tk_network_ip(ip, sizeof(ip));
    char *html = calloc(1, 9000); if (!html) return ESP_ERR_NO_MEM;
    snprintf(html, 9000, "%s<body><main><div class=shell><div class=brand><span class=mark>T</span> TIMETONE · TERMINAL SETUP</div><h1>Configure this terminal</h1><p>Connect it to your office network and TimeTone server.</p><div class=address>Terminal address <b>%s</b></div><form method=post action=/save><fieldset><legend>Connection</legend><label>Wi-Fi name</label><input name=ssid maxlength=32 value=\"%s\" required autocomplete=off><label>Wi-Fi password</label><input name=password type=password maxlength=64 placeholder=\"Leave blank to keep current\"><label>TimeTone server URL</label><input name=server maxlength=159 value=\"%s\" placeholder=\"https://time.example.com\" required></fieldset><fieldset><legend>Terminal behaviour</legend><label>Timezone</label><input name=timezone maxlength=63 value=\"%s\"><label>Health check interval (seconds)</label><input name=sync type=number min=2 max=60 value=\"%u\"><label>Full settings sync (seconds)</label><input name=fullsync type=number min=30 max=3600 value=\"%u\"><label>Screen off after (seconds, 0 disables)</label><input name=screenoff type=number min=0 max=3600 value=\"%u\"><label>Low power after (seconds, 0 disables)</label><input name=lowpower type=number min=0 max=3600 value=\"%u\"><label>Terminal theme</label><select name=theme><option value=\"dark\" %s>Dark</option><option value=\"light\" %s>Light</option></select></fieldset><button>Save changes</button></form><div class=foot>Wi-Fi changes restart the terminal. All other settings apply immediately.</div></div></main></body></html>", SETUP_HTML_HEAD, ip, ssid, server, timezone, config->sync_interval_seconds ?: 5, config->full_sync_interval_seconds ?: 300, config->screen_off_timeout_seconds, config->low_power_timeout_seconds, strcmp(config->terminal_theme, "dark") == 0 ? "selected" : "", strcmp(config->terminal_theme, "light") == 0 ? "selected" : "");
    esp_err_t err = httpd_resp_send(request, html, HTTPD_RESP_USE_STRLEN); free(html); return err;
}

static void url_decode(char *output, const char *input, size_t output_size)
{
    size_t j = 0;
    for (size_t i = 0; input[i] && j + 1 < output_size; ++i) {
        if (input[i] == '+' ) output[j++] = ' ';
        else if (input[i] == '%' && isxdigit((unsigned char)input[i + 1]) && isxdigit((unsigned char)input[i + 2])) {
            char hex[3] = { input[i + 1], input[i + 2], 0 };
            output[j++] = (char)strtol(hex, NULL, 16); i += 2;
        } else output[j++] = input[i];
    }
    output[j] = 0;
}

static void form_value(const char *body, const char *key, char *output, size_t output_size)
{
    output[0] = 0;
    char needle[40];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *start = body;
    while ((start = strstr(start, needle)) != NULL) {
        if (start == body || start[-1] == '&') break;
        start += strlen(needle);
    }
    if (!start) return;
    start += strlen(needle);
    const char *end = strchr(start, '&');
    size_t length = end ? (size_t)(end - start) : strlen(start);
    char encoded[192];
    length = length < sizeof(encoded) - 1 ? length : sizeof(encoded) - 1;
    memcpy(encoded, start, length); encoded[length] = 0;
    url_decode(output, encoded, output_size);
}

static esp_err_t save_handler(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len >= 1400) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid form");
    char *body = calloc(1, request->content_len + 1);
    if (!body) return ESP_ERR_NO_MEM;
    size_t received = 0;
    while (received < (size_t)request->content_len) {
        int chunk = httpd_req_recv(request, body + received, request->content_len - received);
        if (chunk <= 0) { free(body); return ESP_FAIL; }
        received += (size_t)chunk;
    }
    body[received] = 0;
    tk_config_t previous = *tk_config_get();
    tk_config_t config = previous; config.configured = true;
    form_value(body, "ssid", config.ssid, sizeof(config.ssid));
    char password[65]; form_value(body, "password", password, sizeof(password)); if (password[0]) strlcpy(config.wifi_password, password, sizeof(config.wifi_password));
    form_value(body, "server", config.server_url, sizeof(config.server_url));
    char value[96];
    form_value(body, "timezone", value, sizeof(value)); if (value[0]) strlcpy(config.timezone, value, sizeof(config.timezone));
    form_value(body, "theme", value, sizeof(value)); if (strcmp(value, "dark") == 0 || strcmp(value, "light") == 0) { strlcpy(config.terminal_theme, value, sizeof(config.terminal_theme)); config.terminal_theme_override = true; }
    form_value(body, "sync", value, sizeof(value)); int number = atoi(value); if (number >= 2 && number <= 60) config.sync_interval_seconds = number;
    form_value(body, "fullsync", value, sizeof(value)); number = atoi(value); if (number >= 30 && number <= 3600) config.full_sync_interval_seconds = number;
    form_value(body, "screenoff", value, sizeof(value)); number = atoi(value); if (number >= 0 && number <= 3600) config.screen_off_timeout_seconds = number;
    form_value(body, "lowpower", value, sizeof(value)); number = atoi(value); if (number >= 0 && number <= 3600) config.low_power_timeout_seconds = number;
    config.power_timeouts_configured = true;
    // Generate the device credential locally; it is never typed or exposed
    // during setup. The server receives it through the pairing handshake.
    if (tk_config_get()->device_token[0]) {
        strlcpy(config.device_token, tk_config_get()->device_token, sizeof(config.device_token));
    } else {
        uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(config.device_token, sizeof(config.device_token), "tk-%02X%02X%02X%02X-%08lX", mac[2], mac[3], mac[4], mac[5], (unsigned long)esp_random());
    }
    free(body);
    if (!config.ssid[0] || !config.server_url[0]) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Missing required value");
    while (strlen(config.server_url) && config.server_url[strlen(config.server_url) - 1] == '/') config.server_url[strlen(config.server_url) - 1] = 0;
    esp_err_t save_err = tk_config_save(&config);
    ESP_LOGI(TAG, "saving terminal settings: server=%s ssid=%s result=%s", config.server_url, config.ssid, esp_err_to_name(save_err));
    if (save_err != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save settings to flash");
    }
    bool restart_required = strcmp(previous.ssid, config.ssid) != 0 || strcmp(previous.wifi_password, config.wifi_password) != 0;
    httpd_resp_set_type(request, "text/html");
    if (restart_required) {
        httpd_resp_sendstr(request, "<html><head><meta http-equiv='refresh' content='3;url=/'></head><body style='font:18px system-ui;padding:40px'><h1>Saved</h1><p>Wi-Fi settings changed. Restarting terminal…</p></body></html>");
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    } else {
        tk_display_apply_settings();
        httpd_resp_sendstr(request, "<html><head><meta http-equiv='refresh' content='2;url=/'></head><body style='font:18px system-ui;padding:40px'><h1>Saved</h1><p>Settings applied. Returning to configuration…</p></body></html>");
    }
    return ESP_OK;
}

static void start_http_server(void)
{
    if (s_server) return;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    if (httpd_start(&s_server, &config) == ESP_OK) {
        httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
        httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_handler };
        httpd_register_uri_handler(s_server, &root);
        httpd_register_uri_handler(s_server, &save);
    }
}

static void start_sntp_once(void)
{
    static bool started;
    if (started) return;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&config) == ESP_OK) started = true;
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_connect_attempts = 0;
        s_fallback_active = false;
        tk_display_set_network_state(TK_DISPLAY_CONNECTING);
        schedule_reconnect(0);
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, CONNECTED_BIT);
        strlcpy(s_ip, "0.0.0.0", sizeof(s_ip));
        if (++s_connect_attempts >= 3 && !s_fallback_active) {
            s_fallback_active = true;
            tk_network_start_setup_ap();
            tk_display_show_setup();
        }
        // AP fallback is a recovery portal, not a replacement for the station.
        // Keep attempting STA reconnection while the portal is visible so a
        // temporary router or internet outage heals on its own.
        tk_display_set_network_state(TK_DISPLAY_CONNECTING);
        const wifi_event_sta_disconnected_t *event = data;
        uint32_t delay_ms = reconnect_delay_ms();
        ESP_LOGW(TAG, "station disconnected (reason %u, attempt %u); retrying in %lu ms", event ? event->reason : 0, s_connect_attempts, (unsigned long)delay_ms);
        schedule_reconnect(delay_ms);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
        tk_display_set_ip(s_ip);
        xEventGroupSetBits(s_events, CONNECTED_BIT);
        s_connect_attempts = 0; s_fallback_active = false;
        if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);
        tk_display_set_network_state(TK_DISPLAY_CONNECTING);
        // Sync is event-driven after the v0.2.26 rework: if the api task
        // already consumed its boot wake before Wi-Fi came up it would block
        // forever on the semaphore, leaving the terminal stuck on the boot
        // screen. Re-arm it here so a freshly-online station immediately
        // heartbeats and fetches config from the server.
        tk_api_wake();
        start_sntp_once();
        ESP_LOGI(TAG, "connected with IP %s", s_ip);
    }
}

void tk_network_start_setup_ap(void)
{
    if (s_ap_started) return;
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    esp_wifi_set_mode(mode == WIFI_MODE_STA ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    wifi_config_t ap = { .ap = { .channel = 1, .max_connection = 4, .authmode = WIFI_AUTH_WPA2_PSK } };
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    strlcpy((char *)ap.ap.password, "timekeep", sizeof(ap.ap.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    s_ap_started = true;
    start_http_server();
    ESP_LOGW(TAG, "setup access point %s started", s_ap_ssid);
}

esp_err_t tk_network_init(void)
{
    s_events = xEventGroupCreate();
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    const esp_timer_create_args_t reconnect_timer_args = {
        .callback = reconnect_timer_callback,
        .name = "wifi_reconnect",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&reconnect_timer_args, &s_reconnect_timer), TAG, "reconnect timer");
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "TimeTone-%02X%02X", mac[4], mac[5]);
    const tk_config_t *stored = tk_config_get();
    if (stored->configured) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        wifi_config_t station = {0};
        strlcpy((char *)station.sta.ssid, stored->ssid, sizeof(station.sta.ssid));
        strlcpy((char *)station.sta.password, stored->wifi_password, sizeof(station.sta.password));
        // Do not reject WPA3 transition networks before trying credentials.
        // FAST_SCAN may stop at a weak, incompatible AP with a shared SSID.
        station.sta.threshold.authmode = WIFI_AUTH_OPEN;
        station.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        station.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        station.sta.listen_interval = 3;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &station));
    } else ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    // Keep the configuration portal available through the station IP as well
    // as the fallback setup access point.
    start_http_server();
    if (!stored->configured) tk_network_start_setup_ap();
    return ESP_OK;
}

bool tk_network_connected(void) { return (xEventGroupGetBits(s_events) & CONNECTED_BIT) != 0; }
const char *tk_network_setup_ssid(void) { return s_ap_ssid; }
void tk_network_ip(char *output, size_t size) { strlcpy(output, s_ip, size); }

void tk_network_set_low_power(bool enabled)
{
    // Keep the station associated but let the radio sleep between beacons
    // while the terminal display is idle.
    esp_wifi_set_ps(enabled ? WIFI_PS_MAX_MODEM : WIFI_PS_NONE);
}

void tk_network_resume(void)
{
    const tk_config_t *config = tk_config_get();
    if (!config->configured) return;

    // MAX_MODEM is useful while the terminal is idle, but after a long sleep
    // an AP may have discarded the station without generating a prompt event.
    // Restore full radio power first, then verify association rather than
    // trusting an old DHCP/IP event bit.
    esp_err_t power_err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (power_err != ESP_OK) ESP_LOGW(TAG, "could not restore Wi-Fi power: %s", esp_err_to_name(power_err));

    wifi_ap_record_t access_point;
    bool association_lost = esp_wifi_sta_get_ap_info(&access_point) != ESP_OK;
    bool had_ip = tk_network_connected();
    if (!had_ip || association_lost) {
        xEventGroupClearBits(s_events, CONNECTED_BIT);
        ESP_LOGW(TAG, "wake network check: %s; reconnecting station", association_lost ? "association lost" : "no IP");
        // Disconnecting an orphaned station makes the normal event handler
        // perform its bounded recovery sequence. A disconnected station can
        // simply be connected directly.
        if (association_lost && had_ip) {
            esp_wifi_disconnect();
        } else {
            schedule_reconnect(0);
        }
    }
}
