#pragma once
#include "esp_err.h"
#include "timekeep.h"

esp_err_t tk_storage_init(void);
const tk_config_t *tk_config_get(void);
esp_err_t tk_config_save(const tk_config_t *config);
tk_state_t *tk_state_lock(void);
void tk_state_unlock(void);
esp_err_t tk_state_save(void);
const tk_employee_t *tk_find_employee_by_code(const char *code);
esp_err_t tk_toggle_employee(const char *employee_id, tk_event_t *created_event, bool *now_clocked_in);
esp_err_t tk_queue_code_request(const char *code);
bool tk_peek_code_request(tk_code_request_t *request);
esp_err_t tk_pop_code_request(const char *id);
void tk_format_utc(char output[25]);
bool tk_time_is_valid(void);
// The terminal includes the POSIX rules needed for its supported IANA zones.
// Unsupported values leave the currently active, known-safe timezone in place.
bool tk_time_apply_timezone(const char *timezone);
