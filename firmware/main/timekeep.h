#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TK_MAX_EMPLOYEES 32
#define TK_MAX_EVENTS 48
#define TK_MAX_CODE_REQUESTS 12
#define TK_FIRMWARE_VERSION "0.3.0"

typedef struct {
    char ssid[33];
    char wifi_password[65];
    char server_url[160];
    char device_token[129];
    char timezone[64];
    // Keep this established field in place: config is persisted as an NVS
    // blob, so later fields must be appended for backward-compatible reads.
    bool configured;
    char terminal_theme[8];
    uint16_t sync_interval_seconds;
    int16_t touch_x_offset;
    int16_t touch_y_offset;
    uint16_t touch_x_scale;
    uint16_t touch_y_scale;
    bool terminal_theme_override;
    uint16_t sleep_timeout_seconds;
    bool sleep_timeout_configured;
    uint16_t screen_off_timeout_seconds;
    uint16_t low_power_timeout_seconds;
    bool power_timeouts_configured;
    uint16_t full_sync_interval_seconds;
    uint32_t ui_preferences_version;
    bool reduce_motion;
    bool local_intervals_override;
    bool local_power_override;
} tk_config_t;

typedef struct {
    char id[64];
    char name[48];
    char code_digest[65];
    bool clocked_in;
} tk_employee_t;

typedef struct {
    char id[48];
    char employee_id[64];
    char occurred_at[25];
    bool clock_in;
} tk_event_t;

typedef struct {
    char id[48];
    char code[5];
} tk_code_request_t;

typedef struct {
    uint32_t version;
    uint16_t employee_count;
    uint16_t event_count;
    tk_employee_t employees[TK_MAX_EMPLOYEES];
    tk_event_t events[TK_MAX_EVENTS];
} tk_state_t;
