#include "display.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/lock.h>
#include <time.h>
#include <unistd.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_xpt2046.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "api.h"
#include "network.h"
#include "storage.h"

static const char *TAG = "display";
#define LCD_HOST SPI2_HOST
#define TOUCH_HOST SPI3_HOST
#define PIN_LCD_SCLK 14
#define PIN_LCD_MOSI 13
#define PIN_LCD_MISO 12
#define PIN_LCD_CS 15
#define PIN_LCD_DC 2
#define PIN_LCD_RST -1
#define PIN_LCD_BL 27
// Some S032 production runs route the backlight enable to GPIO21 (the
// connected unit did so with the previous firmware); GPIO27 is used by the
// other S032/ST7789 run. Drive both harmless control nets for compatibility.
#define PIN_LCD_BL_ALT 21
#define PIN_TOUCH_SCLK 25
#define PIN_TOUCH_MOSI 32
#define PIN_TOUCH_MISO 39
#define PIN_TOUCH_CS 33
#define PIN_TOUCH_IRQ 36
#define H_RES 240
#define V_RES 320

// ============================================================================
// Phase 1 palette tokens (design spec §1).
// Every widget colour comes from these arrays. No raw hex outside this file.
// ============================================================================
typedef enum {
    TK_TOKEN_BG = 0,
    TK_TOKEN_SURFACE,
    TK_TOKEN_INK,
    TK_TOKEN_MUTED,
    TK_TOKEN_LINE,
    TK_TOKEN_ACCENT,
    TK_TOKEN_GOOD,
    TK_TOKEN_BUSY,
    TK_TOKEN_SYNC,
    TK_TOKEN_ERROR,
    TK_TOKEN_COUNT,
} tk_color_token_t;

// Light theme tokens (spec §1).
static const uint32_t TK_COLOR_LIGHT[TK_TOKEN_COUNT] = {
    0xF3F2EB, // bg
    0xE5E8DD, // surface
    0x22362A, // ink
    0x58675B, // muted
    0xBEC6B7, // line
    0xCDDD8B, // accent
    0x238342, // good
    0x986400, // busy
    0x286BC4, // sync
    0xB43730, // error
};
// Dark theme tokens (spec §1).
static const uint32_t TK_COLOR_DARK[TK_TOKEN_COUNT] = {
    0x18221C, // bg
    0x26332A, // surface
    0xEEF2E6, // ink
    0xB4C1B1, // muted
    0x52624F, // line
    0xCDDD8B, // accent
    0x80DF98, // good
    0xEFBE58, // busy
    0x85B6FF, // sync
    0xFF9288, // error
};
// Four keypad colours are unchanged across themes. Order A B / C D preserved.
static const uint32_t TK_CODE_COLORS[4] = { 0xEF6F61, 0x3D8BFD, 0x9ACB3C, 0x9B72CF };
// "#22362A" is the literal foreground for accent-filled controls in BOTH themes.
#define TK_ACCENT_FG 0x22362A

// ============================================================================
// Geometry constants (design spec §3 — keypad vertical budget must sum to 320).
// Header 52 + sequence 36 + tiles 176 + footer 56 = 320.
// ============================================================================
#define TK_HEADER_H        52
#define TK_GUTTER          12
#define TK_TILE_W         104
#define TK_TILE_H          84
#define TK_TILE_GAP         8
#define TK_SEQ_H           36
#define TK_SEQ_DOT         10
#define TK_SEQ_DOT_GAP      7
#define TK_FOOTER_H        56
#define TK_CLEAR_W        216
#define TK_CLEAR_H         44
#define TK_GEAR_W          44
#define TK_GEAR_H          44
#define TK_DOT_W           14
#define TK_DOT_H           14
#define TK_TARGET          44
#define TK_ROW_W          216
#define TK_ROW_H           48
#define TK_GROUP_TOP_PAD   16
#define TK_GROUP_BOT_PAD    6

// Calibration target centres (spec §6).
// Container starts directly below the 52px header.
//
// ORDER IS LOAD-BEARING — it must stay top-left, top-right, bottom-left,
// bottom-right. Three places depend on it and will silently disagree otherwise:
//   * the on-screen prompt order in `steps[]` in touch_cb,
//   * the sample slot `s_calibration_x/y[s_calibration_step]`, and
//   * the maths in touch_cb, which averages indices 0+2 for the left column and
//     1+3 for the right column.
// If this array is reordered (e.g. to TL, TR, BR, BL) the user is prompted for
// "Bottom left" while being shown a bottom-right target, `right - left`
// collapses to zero, and the `right != left` guard fails every time — so
// calibration can never succeed and touch keeps its stale scale/offset.
static const int TK_CAL_TARGETS[4][2] = {
    { 34, 130 }, { 194, 130 }, { 34, 280 }, { 194, 280 }
};
// Calibration spans (used for the maths update below).
// Horizontal: 194-34 = 160 (was 190; the 12px gutters narrow the row).
// Vertical:   280-130 = 150 (unchanged; the header is still 52px).
#define TK_CAL_X_SPAN   160
#define TK_CAL_Y_SPAN   150
#define TK_CAL_X_ORIGIN  34
#define TK_CAL_Y_ORIGIN 130

// ============================================================================
// Motion timing constants (design spec §8). Consumed by the Phase 2 motion
// helpers below: press scale, entry growth, navigation slides, tint fades,
// and the three status-dot loops.
// ============================================================================
#define TK_MOTION_PRESS_MS    100
#define TK_MOTION_ENTRY_MS    120
#define TK_MOTION_NAV_FWD_MS  180
#define TK_MOTION_NAV_BACK_MS 140
#define TK_MOTION_TINT_MS     120
#define TK_MOTION_CONNECT_MS 1400
#define TK_MOTION_SYNC_MS     900
#define TK_MOTION_RETRY_MS   2400

// Phase 2 motion constants (spec §7/§8). The retry dip is 0.35 of full
// opacity; with LV_OPA_* named constants snapping to 30% (76) or 40% (102),
// a literal 89 keeps the spec's 35% exact.
#define TK_DOT_RETRY_DIP_OPA 89
// 96% of the 256-unit scale (tile press).
#define TK_SCALE_PRESSED     245
// 70% of 256 — the entry-growth start.
#define TK_SCALE_ENTRY_START 179
// Navigation offsets (spec §8): new page enters from +16px (forward)
// or -10px (return).
#define TK_NAV_FWD_OFFSET     16
#define TK_NAV_BACK_OFFSET   -10

// ============================================================================
// Font role mapping.
// LVGL ships discrete Montserrat sizes. Design types are 11/12/13/14/15px so
// we pick the nearest available. Bold is approximated by the accent
// background (LVGL's bundled Montserrat has no bold variant).
//   11/12/13px  -> Montserrat 12
//   14px        -> Montserrat 14
//   15px        -> Montserrat 16
// ============================================================================
typedef enum {
    TK_FONT_SMALL = 0, // 11 / 12 / 13px role
    TK_FONT_BODY,      // 14px role
    TK_FONT_LARGE,     // 15px role
} tk_font_role_t;

static const lv_font_t *tk_font(tk_font_role_t role)
{
    switch (role) {
        case TK_FONT_SMALL: return &lv_font_montserrat_12;
        case TK_FONT_BODY:  return &lv_font_montserrat_14;
        case TK_FONT_LARGE: return &lv_font_montserrat_16;
        default:            return &lv_font_montserrat_14;
    }
}

// ============================================================================
// Runtime state: theme + reduce-motion.
// ============================================================================
static bool s_reduce_motion;

static _lock_t s_lvgl_lock;
static lv_display_t *s_display;
static esp_lcd_touch_handle_t s_touch;
// Screen containers.
static lv_obj_t *s_main_screen, *s_status_screen, *s_setup_screen,
                *s_settings_screen, *s_calibration_screen, *s_boot_screen,
                *s_ota_screen, *s_picker_screen;
// Header on every screen (52px, per spec §3). Each screen owns its own
// header instance so theme tokens apply uniformly.
static lv_obj_t *s_header;
// Keypad page widgets.
static lv_obj_t *s_clock_label, *s_status_label, *s_pin_label, *s_count_label,
                *s_brand_label;
static lv_obj_t *s_gear_btn, *s_gear_icon;
// Status dot: a persistent 12x12 container in the header plus three overlay
// children, exactly one of which is visible per network state (Phase 2 §7).
// Overlays, not one restyled object, because the states need different
// widget classes (lv_obj for circle/square, lv_line for the slash, lv_arc
// for the open ring) and different animation lifetimes.
static lv_obj_t *s_status_dot;          // container (also the ONLINE solid dot)
static lv_obj_t *s_status_dot_offline;  // outline circle + slash child
static lv_obj_t *s_status_dot_connect;  // solid busy circle (pulses)
static lv_obj_t *s_status_dot_sync;     // lv_arc open ring (rotates)
static lv_obj_t *s_status_dot_retry;    // error rounded square (double pulse)
static lv_obj_t *s_status_dot_halo_bg, *s_status_dot_halo_line;
static lv_obj_t *s_sequence_label;
static lv_obj_t *s_sequence_dots[4];
static lv_obj_t *s_tiles[4];
static lv_obj_t *s_clear_btn;
// Team-status widgets.
static lv_obj_t *s_ip_label, *s_server_label;
static lv_obj_t *s_setup_details;
static lv_obj_t *s_employee_status_list;
// Boot / OTA labels.
static lv_obj_t *s_boot_label, *s_ota_label;
// Settings page widgets.
static lv_obj_t *s_settings_scroll;
static lv_obj_t *s_settings_header;
static lv_obj_t *s_theme_segment_bg;
static lv_obj_t *s_theme_segments[2];
static lv_obj_t *s_reduce_motion_switch;
static lv_obj_t *s_sync_value_label;
static lv_obj_t *s_sync_row;
static lv_timer_t *s_entry_timer;
static lv_obj_t *s_health_check_value_label;
static lv_obj_t *s_settings_sync_value_label;
static lv_obj_t *s_screen_off_value_label;
static lv_obj_t *s_low_power_value_label;
// Picker page widgets (spec §5 — one page reused by every settings picker).
static lv_obj_t *s_picker_header;
static lv_obj_t *s_picker_title_label;
static lv_obj_t *s_picker_list;
// Calibration page widgets.
static lv_obj_t *s_calibration_header;
static lv_obj_t *s_calibration_targets[4];
static lv_obj_t *s_calibration_target_labels[4];
// Track every screen for theme propagation. Each entry is the page container.
static lv_obj_t *s_all_screens[8];
static size_t s_all_screen_count;
// Do not place this on the LVGL task stack.  A complete employee cache is
// nearly 6 KiB, while the UI task must also have room for LVGL itself.
static tk_employee_t s_employee_status_cache[TK_MAX_EMPLOYEES];
static char s_pin[9];
static bool s_online;
static tk_display_network_state_t s_network_state = TK_DISPLAY_OFFLINE;
static uint8_t s_sync_frame;
static volatile bool s_starting;
// Picker page state (Phase 2 §5). s_picker_apply is the commit callback the
// opening settings row installs; NULL while no picker is open.
static void (*s_picker_apply)(uint16_t value);
static const uint16_t *s_picker_values;
// One-shot flag for the Sync-now "Done" feedback.
static bool s_sync_requested;
static volatile bool s_ota_visible;
static bool s_calibrating, s_calibration_wait_release;
static uint8_t s_calibration_step;
static uint16_t s_calibration_x[4], s_calibration_y[4];
static lv_obj_t *s_calibration_progress;
static volatile bool s_screen_sleeping;
static volatile bool s_screen_off;
static bool s_discard_wake_touch;
static int64_t s_last_activity_us;
static bool s_touch_down;
static int s_touch_start_x, s_touch_start_y, s_touch_last_x, s_touch_last_y;

static bool dark_theme(void) { return strcmp(tk_config_get()->terminal_theme, "dark") == 0; }
// Current palette (light or dark) token lookup.
static uint32_t tk_token_color(tk_color_token_t token)
{
    return (dark_theme() ? TK_COLOR_DARK : TK_COLOR_LIGHT)[token];
}
static lv_color_t tk_lv_color(tk_color_token_t token)
{
    return lv_color_hex(tk_token_color(token));
}
// Forward declarations for shared helpers (lock-free; called inside lvgl lock).
static void apply_theme_styles(void);
static void show_main_screen(void);
static void show_status_screen(void);
static void refresh_employee_status_list(void);
static void startup_timeout_timer(lv_timer_t *timer);
static void status_set_token(const char *text, tk_color_token_t token);
static void rebuild_settings_picker_labels(void);
static void set_scale_cb(void *obj, int32_t v);
static void set_translate_x_cb(void *obj, int32_t v);
static void calibration_back_event(lv_event_t *event);

_Static_assert(TK_HEADER_H + TK_SEQ_H + 2 * TK_TILE_H + TK_TILE_GAP + TK_FOOTER_H == V_RES, "Keypad must fit the native canvas");
_Static_assert(2 * TK_GUTTER + 2 * TK_TILE_W + TK_TILE_GAP == H_RES, "Keypad width must fit");

static void set_backlight(bool on)
{
    gpio_set_level(PIN_LCD_BL, on ? 1 : 0);
    gpio_set_level(PIN_LCD_BL_ALT, on ? 1 : 0);
}

static void set_screen_off(bool off)
{
    if (s_screen_off == off) return;
    s_screen_off = off;
    set_backlight(!off);
}

static void set_screen_sleeping(bool sleeping)
{
    if (s_screen_sleeping == sleeping) return;
    s_screen_sleeping = sleeping;
    if (sleeping) set_screen_off(true);
    tk_network_set_low_power(sleeping);
    if (!sleeping) tk_api_resume();
}

static void register_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
    if (s_screen_off) set_screen_off(false);
    if (s_screen_sleeping) set_screen_sleeping(false);
}

static bool flush_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *data, void *ctx)
{
    lv_display_flush_ready((lv_display_t *)ctx);
    return false;
}

static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(display);
    lv_draw_sw_rgb565_swap(pixels, (area->x2 + 1 - area->x1) * (area->y2 + 1 - area->y1));
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, pixels);
}

static void touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t touch = lv_indev_get_user_data(indev);
    uint16_t x[1], y[1]; uint8_t count = 0;
    esp_lcd_touch_read_data(touch);
    if (esp_lcd_touch_get_coordinates(touch, x, y, NULL, &count, 1) && count) {
        bool woke_screen = s_screen_off;
        register_activity();
        // The first tap only wakes a sleeping screen; it must not also clock
        // somebody in or activate a settings button.
        if (woke_screen || s_discard_wake_touch) { s_discard_wake_touch = false; data->state = LV_INDEV_STATE_RELEASED; return; }
        if (s_calibrating) {
            int mapped_x = (x[0] * tk_config_get()->touch_x_scale) / 1000 + tk_config_get()->touch_x_offset;
            int mapped_y = (y[0] * tk_config_get()->touch_y_scale) / 1000 + tk_config_get()->touch_y_offset;
            if (mapped_y < TK_HEADER_H && mapped_x >= H_RES - TK_GUTTER - TK_TARGET) {
                calibration_back_event(NULL);
                data->state = LV_INDEV_STATE_RELEASED;
                return;
            }
            if (!s_calibration_wait_release) {
                lv_obj_add_flag(s_calibration_targets[s_calibration_step], LV_OBJ_FLAG_HIDDEN);
                s_calibration_x[s_calibration_step] = x[0];
                s_calibration_y[s_calibration_step] = y[0];
                s_calibration_wait_release = true;
                s_calibration_step++;
                if (s_calibration_step >= 4) {
                    int left = (s_calibration_x[0] + s_calibration_x[2]) / 2;
                    int right = (s_calibration_x[1] + s_calibration_x[3]) / 2;
                    int top = (s_calibration_y[0] + s_calibration_y[1]) / 2;
                    int bottom = (s_calibration_y[2] + s_calibration_y[3]) / 2;
                    tk_config_t updated = *tk_config_get();
                    if (right - left >= 40 && bottom - top >= 40) {
                        // New target centres are (34,130) (194,130) (34,280) (194,280)
                        // (spec §6). The horizontal span shrinks from 190 to 160
                        // because the mockup's 12px gutters narrow the target row.
                        // The vertical span is still 150 (header is still 52px).
                        updated.touch_x_scale = (uint16_t)((TK_CAL_X_SPAN * 1000) / (right - left));
                        updated.touch_y_scale = (uint16_t)((TK_CAL_Y_SPAN * 1000) / (bottom - top));
                        updated.touch_x_offset = TK_CAL_X_ORIGIN - (int16_t)((left * updated.touch_x_scale) / 1000);
                        updated.touch_y_offset = TK_CAL_Y_ORIGIN - (int16_t)((top * updated.touch_y_scale) / 1000);
                        esp_err_t saved = tk_config_save(&updated);
                        lv_label_set_text(s_calibration_progress, saved == ESP_OK ?
                            "Calibration saved\nTap Back to return" : "Save failed\nTap Back to retry");
                    } else lv_label_set_text(s_calibration_progress, "Calibration failed\nTap BACK to retry");
                    s_calibrating = false;
                } else {
                    lv_obj_clear_flag(s_calibration_targets[s_calibration_step], LV_OBJ_FLAG_HIDDEN);
                    static const char *steps[] = { "Top left", "Top right", "Bottom left", "Bottom right" };
                    char hint[72]; snprintf(hint, sizeof(hint), "Touch %s target", steps[s_calibration_step]);
                    lv_label_set_text(s_calibration_progress, hint);
                }
            }
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        int adjusted_x = (x[0] * tk_config_get()->touch_x_scale) / 1000 + tk_config_get()->touch_x_offset;
        int adjusted_y = (y[0] * tk_config_get()->touch_y_scale) / 1000 + tk_config_get()->touch_y_offset;
        data->point.x = adjusted_x < 0 ? 0 : adjusted_x >= H_RES ? H_RES - 1 : adjusted_x;
        data->point.y = adjusted_y < 0 ? 0 : adjusted_y >= V_RES ? V_RES - 1 : adjusted_y;
        if (!s_touch_down) {
            s_touch_down = true;
            s_touch_start_x = data->point.x; s_touch_start_y = data->point.y;
        }
        s_touch_last_x = data->point.x; s_touch_last_y = data->point.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        // Detect the terminal-wide horizontal gesture before LVGL resolves a
        // click. It is deliberately generous so normal keypad taps remain
        // completely unaffected.
        if (s_touch_down && !s_calibrating) {
            int dx = s_touch_last_x - s_touch_start_x;
            int dy = s_touch_last_y - s_touch_start_y;
            if ((dx > 62 || dx < -62) && (dy < 34 && dy > -34)) {
                if (dx > 0 && s_main_screen && !lv_obj_has_flag(s_main_screen, LV_OBJ_FLAG_HIDDEN)) show_status_screen();
                else if (dx < 0 && s_status_screen && !lv_obj_has_flag(s_status_screen, LV_OBJ_FLAG_HIDDEN)) show_main_screen();
            }
        }
        s_touch_down = false;
        s_calibration_wait_release = false; data->state = LV_INDEV_STATE_RELEASED;
    }
}

static bool touch_is_pressed(void)
{
    if (!s_touch) return false;
    uint16_t x[1], y[1];
    uint8_t count = 0;
    esp_lcd_touch_read_data(s_touch);
    return esp_lcd_touch_get_coordinates(s_touch, x, y, NULL, &count, 1) && count > 0;
}

static void tick_cb(void *argument) { lv_tick_inc(2); }

static void lvgl_task(void *argument)
{
    while (true) {
        // Do not rely only on the XPT2046 IRQ level here. On some CYD board
        // revisions it is unreliable after modem sleep; a lightweight touch
        // controller poll reliably catches the first physical tap.
        if (s_screen_off && touch_is_pressed()) {
            s_discard_wake_touch = true;
            register_activity();
        }
        _lock_acquire(&s_lvgl_lock);
        uint32_t wait = lv_timer_handler();
        _lock_release(&s_lvgl_lock);
        int64_t idle_us = esp_timer_get_time() - s_last_activity_us;
        uint16_t screen_off_timeout = tk_config_get()->screen_off_timeout_seconds;
        uint16_t low_power_timeout = tk_config_get()->low_power_timeout_seconds;
        if (screen_off_timeout && !s_screen_off && idle_us >= (int64_t)screen_off_timeout * 1000000LL) {
            set_screen_off(true);
        }
        if (low_power_timeout && !s_screen_sleeping && idle_us >= (int64_t)low_power_timeout * 1000000LL) {
            set_screen_sleeping(true);
        }
        wait = wait < 2 ? 2 : (wait > 100 ? 100 : wait);
        if (s_screen_sleeping && wait < 250) wait = 250;
        usleep(wait * 1000);
    }
}

// Legacy set_status still accepts a raw hex so existing callers stay valid.
// The token-based sibling is the preferred path for new code; it keeps the
// "no raw colour outside the two palettes" rule.
static void set_status(const char *text, uint32_t color)
{
    lv_label_set_text(s_status_label, text);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(color), 0);
}

static void status_set_token(const char *text, tk_color_token_t token)
{
    if (!s_status_label || !text) return;
    lv_label_set_text(s_status_label, text);
    lv_obj_set_style_text_color(s_status_label, tk_lv_color(token), 0);
}

static void update_pin_label(void)
{
    // s_pin_label is intentionally hidden and kept for state handling. It still
    // gets a valid token colour so apply_theme_styles can re-paint it safely.
    char hidden[9] = {0};
    for (size_t i = 0; i < strlen(s_pin); ++i) hidden[i] = '*';
    lv_label_set_text(s_pin_label, hidden[0] ? hidden : "Choose your colors");
    lv_obj_set_style_text_color(s_pin_label, tk_lv_color(TK_TOKEN_INK), 0);
    // Sequence dots (spec §3): filled = ink fill + ink border; empty = 1px
    // muted border, transparent fill. ONLY the newly filled mark grows
    // 70->100% over 120ms (spec §8 "Entry"), pivot at its centre. The dots
    // persist, so cancel any prior entry animation on that dot first;
    // previously-filled dots settle at 100% with no animation.
    static size_t s_prev_filled;
    size_t filled = strlen(s_pin);
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *dot = s_sequence_dots[i];
        if (!dot) continue;
        bool on = (size_t)i < filled;
        bool newly = on && (size_t)i >= s_prev_filled; // dot i filled by this entry
        lv_obj_set_style_bg_color(dot, tk_lv_color(TK_TOKEN_INK), 0);
        lv_obj_set_style_bg_opa(dot, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(dot, tk_lv_color(on ? TK_TOKEN_INK : TK_TOKEN_MUTED), 0);
        lv_obj_set_style_transform_pivot_x(dot, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(dot, LV_PCT(50), 0);
        lv_anim_delete(dot, set_scale_cb);
        if (on && newly && !s_reduce_motion) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, dot);
            lv_anim_set_exec_cb(&a, set_scale_cb);
            lv_anim_set_values(&a, TK_SCALE_ENTRY_START, LV_SCALE_NONE);
            lv_anim_set_duration(&a, TK_MOTION_ENTRY_MS);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_start(&a);
        } else {
            lv_obj_set_style_transform_scale(dot, LV_SCALE_NONE, 0);
        }
    }
    s_prev_filled = filled;
}

static void entry_reset_timer(lv_timer_t *timer)
{
    s_entry_timer = NULL;
    s_pin[0] = 0;
    for (int i = 0; i < 4; ++i) lv_obj_remove_state(s_tiles[i], LV_STATE_DISABLED);
    update_pin_label();
    lv_timer_delete(timer);
}

static void handle_keypress(const char *text)
{
    if (!text || s_entry_timer) return;
    if (strlen(s_pin) < 4) {
        strlcat(s_pin, text, sizeof(s_pin));
        if (strlen(s_pin) == 4) {
            esp_err_t queued = tk_api_queue_code(s_pin);
            for (int i = 0; i < 4; ++i) lv_obj_add_state(s_tiles[i], LV_STATE_DISABLED);
            s_entry_timer = lv_timer_create(entry_reset_timer, 1400, NULL);
            if (queued == ESP_OK) {
                status_set_token("Saved - sending", TK_TOKEN_SYNC);
                tk_api_poke();
            } else {
                status_set_token(queued == ESP_ERR_NO_MEM ? "Queue full - try later" : "Could not save code", TK_TOKEN_ERROR);
            }
        } else {
            char progress[40];
            snprintf(progress, sizeof(progress), "%u of 4", (unsigned)strlen(s_pin));
            status_set_token(progress, TK_TOKEN_MUTED);
        }
    }
    update_pin_label();
}

static void keypad_button_event(lv_event_t *event)
{
    handle_keypress((const char *)lv_event_get_user_data(event));
}

static void clear_keypad_event(lv_event_t *event)
{
    (void)event;
    if (s_entry_timer) { lv_timer_delete(s_entry_timer); s_entry_timer = NULL; }
    s_pin[0] = 0;
    for (int i = 0; i < 4; ++i) lv_obj_remove_state(s_tiles[i], LV_STATE_DISABLED);
    update_pin_label();
    status_set_token("Your four colours", TK_TOKEN_MUTED);
}

static void clock_timer(lv_timer_t *timer)
{
    time_t now; struct tm local;
    time(&now); localtime_r(&now, &local);
    char text[24];
    if (tk_time_is_valid()) strftime(text, sizeof(text), "%H:%M", &local);
    else strlcpy(text, "--:--", sizeof(text));
    if (s_clock_label) lv_label_set_text(s_clock_label, text);
}

// ============================================================================
// Phase 2 motion helpers (spec §8). All run under the LVGL task context
// (caller holds s_lvgl_lock); none take the lock.
// ============================================================================

// Opacity wrapper — lv_obj_set_style_opa takes a selector, so it cannot be
// used directly as an lv_anim_exec_xcb_t (API report §2).
static void set_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, LV_PART_MAIN);
}

// Arc rotation wrapper — lv_arc_set_rotation(obj, rot) with rot in degrees.
static void set_arc_rotation_cb(void *obj, int32_t v)
{
    lv_arc_set_rotation((lv_obj_t *)obj, v);
}

// Style-scale wrapper for the entry-growth of a sequence mark.
static void set_scale_cb(void *obj, int32_t v)
{
    lv_obj_set_style_transform_scale((lv_obj_t *)obj, v, LV_PART_MAIN);
}

// Press feedback (spec §8 "Press"): scale to 96% while pressed, back to
// 100% on release, no input delay. Implemented as a style-state transition
// so LVGL drives it; no event callback, no explicit animation to cancel.
//
// WHY THE DESCRIPTOR LIVES ON THE DEFAULT SELECTOR (verified in the vendored
// 9.3 source): update_obj_state() (lv_obj.c:934) skips style entries whose
// selector state is absent from the *new* state when collecting transition
// descriptors. On release the new state lacks PRESSED, so a descriptor
// registered on LV_STATE_PRESSED is skipped and the release *snaps*. With
// the descriptor on selector 0 it is collected both ways. The props array
// and descriptor are static (they are stored by pointer — API report §5).
//
// An object holds exactly ONE transition descriptor per selector, so tiles
// (spec §3: pressed feedback is the scale motion, "not a colour change")
// use the scale-only descriptor, while tinted controls (gear, clear, theme
// segments — spec §3 pressed -> accent) use a combined scale+colour
// descriptor at the 120ms control-state timing.
static const lv_style_prop_t tk_press_scale_props[] = {
    LV_STYLE_TRANSFORM_SCALE_X, LV_STYLE_TRANSFORM_SCALE_Y, 0
};
static lv_style_transition_dsc_t tk_press_scale_dsc;
static const lv_style_prop_t tk_tint_press_props[] = {
    LV_STYLE_TRANSFORM_SCALE_X, LV_STYLE_TRANSFORM_SCALE_Y,
    LV_STYLE_BG_COLOR, LV_STYLE_TEXT_COLOR, 0
};
static lv_style_transition_dsc_t tk_tint_press_dsc;

static void press_feedback_attach(lv_obj_t *obj)
{
    if (s_reduce_motion) return;
    lv_obj_set_style_transform_pivot_x(obj, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(obj, LV_PCT(50), 0);
    lv_obj_set_style_transform_scale(obj, TK_SCALE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_transition(obj, &tk_press_scale_dsc, 0);
}

// Tinted controls: 120ms colour fade (spec §8 "Control state") plus the
// same centre-pivoted press scale.
static void tint_press_attach(lv_obj_t *obj)
{
    if (s_reduce_motion) return;
    lv_obj_set_style_transform_pivot_x(obj, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(obj, LV_PCT(50), 0);
    lv_obj_set_style_transform_scale(obj, TK_SCALE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_transition(obj, &tk_tint_press_dsc, 0);
}

// Strip the pressed-state scale so a future press changes no transforms
// (spec §8 reduced motion "removes transforms"). The colour transition
// descriptor stays attached — colour fades are not transforms, pulses or
// rotation — and with the pressed scale values gone the scale props in it
// compare equal, so lv_obj_style_create_transition() skips them entirely.
static void press_feedback_detach(lv_obj_t *obj)
{
    lv_obj_remove_local_style_prop(obj, LV_STYLE_TRANSFORM_SCALE_X, LV_STATE_PRESSED);
    lv_obj_remove_local_style_prop(obj, LV_STYLE_TRANSFORM_SCALE_Y, LV_STATE_PRESSED);
}

// Every interactive widget with press-scale feedback, indexed at build;
// called from settings_reduce_motion_event whenever the flag flips.
static void press_feedback_set_all(bool enable)
{
    // Tiles: scale-only descriptor. Others: combined tint+scale.
    for (int i = 0; i < 4; ++i) {
        if (!s_tiles[i]) continue;
        if (enable) press_feedback_attach(s_tiles[i]);
        else press_feedback_detach(s_tiles[i]);
    }
    lv_obj_t *tinted[4];
    int n = 0;
    if (s_clear_btn) tinted[n++] = s_clear_btn;
    if (s_gear_btn) tinted[n++] = s_gear_btn;
    if (s_theme_segments[0]) tinted[n++] = s_theme_segments[0];
    if (s_theme_segments[1]) tinted[n++] = s_theme_segments[1];
    for (int i = 0; i < n; ++i) {
        if (enable) tint_press_attach(tinted[i]);
        else press_feedback_detach(tinted[i]);
    }
}

static void motion_descriptors_init(void)
{
    lv_style_transition_dsc_init(&tk_press_scale_dsc, tk_press_scale_props,
                                 lv_anim_path_ease_out, TK_MOTION_PRESS_MS, 0, NULL);
    lv_style_transition_dsc_init(&tk_tint_press_dsc, tk_tint_press_props,
                                 lv_anim_path_ease_in_out, TK_MOTION_TINT_MS, 0, NULL);
}

// ----------------------------------------------------------------------------
// Status dot state machine (spec §7). The dot persists across network state
// changes, so every state transition MUST first cancel the previous state's
// animation (lv_anim_delete) before the next state's starts. Shapes alone
// identify each state when motion is reduced.
// ----------------------------------------------------------------------------
static void status_dot_show(lv_obj_t *visible)
{
    lv_obj_t *all[] = { s_status_dot, s_status_dot_offline, s_status_dot_connect,
                        s_status_dot_sync, s_status_dot_retry };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i)
        if (all[i]) lv_obj_add_flag(all[i], LV_OBJ_FLAG_HIDDEN);
    if (visible) lv_obj_clear_flag(visible, LV_OBJ_FLAG_HIDDEN);
}

// Cancel every animation bound to any status-dot widget. Safe to call from
// event callbacks (runs under the LVGL lock, never takes it).
static void status_dot_anim_cancel(void)
{
    lv_obj_t *all[] = { s_status_dot, s_status_dot_offline, s_status_dot_connect,
                        s_status_dot_sync, s_status_dot_retry };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i)
        if (all[i]) lv_anim_delete(all[i], NULL);
    // Animations above may leave a mid-pulse opacity; restore full opacity.
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i)
        if (all[i]) lv_obj_set_style_opa(all[i], LV_OPA_COVER, LV_PART_MAIN);
    if (s_status_dot_sync) lv_arc_set_rotation(s_status_dot_sync, 0);
}

// Custom anim path for the retrying double pulse (spec §7): opacity is
// 1.0 at 0/12/24% of the 2400ms cycle, 0.35 at 6% and 18%, and 1.0 across
// the steady tail. Returns the ABSOLUTE opacity; the anim's start/end
// values are placeholders. Uses only the documented path contract
// (act_time/duration — lv_anim.h:506-556) and integer trig (lv_trigo_cos
// takes degrees, returns -32768..32768 = -1..1).
static int32_t retry_pulse_path(const lv_anim_t *a)
{
    int32_t t = a->act_time;
    if (t < 0) t = 0;
    if (t >= TK_MOTION_RETRY_MS) t = TK_MOTION_RETRY_MS - 1;
    // Phase in tenth-percent of the cycle: 0..1200.
    int32_t phase = (t * 1200) / TK_MOTION_RETRY_MS;
    // Two dips centred at 6% (72) and 18% (216), each spanning +/-3%
    // (144 tenth-percent wide = 288ms), matching the spec's 0/12/24%
    // full-opacity points exactly.
    int32_t d;
    if (phase <= 144) d = phase - 72;        // first dip
    else if (phase <= 288) d = phase - 216;  // second dip
    else return LV_OPA_100;                  // steady tail to cycle end
    if (d < 0) d = -d;
    // Raised cosine: 0 at |d|=72 (full), 1 at d=0 (deepest). Angle 0..180deg.
    int32_t ramp = (32768 + lv_trigo_cos((int16_t)((180 * d) / 72))) >> 1; // 0..32768
    return LV_OPA_100 - (int32_t)(((LV_OPA_100 - TK_DOT_RETRY_DIP_OPA) * ramp) >> 15);
}

static void status_dot_set_state(tk_display_network_state_t state)
{
    if (!s_status_dot) return;
    // The dot persists across state changes — cancel before restyling.
    status_dot_anim_cancel();
    switch (state) {
        case TK_DISPLAY_ONLINE:
            // Solid good circle. Steady.
            status_dot_show(s_status_dot);
            lv_obj_set_style_radius(s_status_dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(s_status_dot, 0, 0);
            lv_obj_set_style_bg_color(s_status_dot, tk_lv_color(TK_TOKEN_GOOD), 0);
            lv_obj_set_style_bg_opa(s_status_dot, LV_OPA_COVER, 0);
            break;
        case TK_DISPLAY_OFFLINE: {
            // Transparent circle, 1px muted border, diagonal muted slash.
            status_dot_show(s_status_dot_offline);
            lv_obj_t *outline = s_status_dot_offline;
            lv_obj_set_style_border_color(outline, tk_lv_color(TK_TOKEN_MUTED), 0);
            lv_obj_set_style_border_width(outline, 1, 0);
            lv_obj_set_style_bg_opa(outline, LV_OPA_TRANSP, 0);
            // Slash child: a 2px muted line rotated -45deg, drawn corner to
            // corner of the 12px box (10px inner span, spec §7).
            lv_obj_t *slash = lv_obj_get_child(outline, 0);
            lv_obj_set_style_line_color(slash, tk_lv_color(TK_TOKEN_MUTED), 0);
            lv_obj_set_style_line_width(slash, 2, 0);
            lv_obj_set_style_line_rounded(slash, true, 0);
            break;
        }
        case TK_DISPLAY_CONNECTING: {
            // Solid busy circle; opacity pulses to 40% at midpoint, 1400ms
            // ease-in-out loop.
            status_dot_show(s_status_dot_connect);
            lv_obj_set_style_bg_color(s_status_dot_connect, tk_lv_color(TK_TOKEN_BUSY), 0);
            if (!s_reduce_motion) {
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, s_status_dot_connect);
                lv_anim_set_exec_cb(&a, set_opa_cb);
                lv_anim_set_values(&a, LV_OPA_100, LV_OPA_40);
                lv_anim_set_duration(&a, TK_MOTION_CONNECT_MS / 2);
                lv_anim_set_reverse_duration(&a, TK_MOTION_CONNECT_MS / 2);
                lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
                lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
                lv_anim_start(&a);
            }
            break;
        }
        case TK_DISPLAY_SYNCING: {
            // Sync-coloured open ring rotating 0->360deg, 900ms linear loop.
            // Drawn with lv_arc: bg arc = visible ring, indicator/knob off.
            status_dot_show(s_status_dot_sync);
            lv_obj_t *arc = s_status_dot_sync;
            lv_obj_set_style_arc_color(arc, tk_lv_color(TK_TOKEN_SYNC), LV_PART_MAIN);
            lv_obj_set_style_arc_width(arc, 2, LV_PART_MAIN);
            lv_obj_set_style_arc_rounded(arc, false, LV_PART_MAIN);
            lv_arc_set_bg_angles(arc, 30, 300);
            if (!s_reduce_motion) {
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, arc);
                lv_anim_set_exec_cb(&a, set_arc_rotation_cb);
                lv_anim_set_values(&a, 0, 359);
                lv_anim_set_duration(&a, TK_MOTION_SYNC_MS);
                lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
                lv_anim_set_path_cb(&a, lv_anim_path_linear);
                lv_anim_start(&a);
            }
            break;
        }
        case TK_DISPLAY_SYNC_RETRYING:
        default: {
            // Error rounded square (radius 3), double opacity dip per
            // 2400ms cycle. With reduced motion the shape alone signals
            // retry (distinct from the offline circle).
            status_dot_show(s_status_dot_retry);
            lv_obj_set_style_bg_color(s_status_dot_retry, tk_lv_color(TK_TOKEN_ERROR), 0);
            lv_obj_set_style_radius(s_status_dot_retry, 3, 0);
            if (!s_reduce_motion) {
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, s_status_dot_retry);
                lv_anim_set_exec_cb(&a, set_opa_cb);
                // Spec §7: opacity dips to 35% at 6% and 18% of the 2400ms
                // cycle, returns to full at 0/12/24%, then steady until
                // repeat. A custom path renders the exact two-dip waveform
                // from act_time, so a single infinite animation owns the
                // full cycle. lv_anim_set_values() is ignored — the path
                // returns the absolute opacity.
                lv_anim_set_values(&a, LV_OPA_100, LV_OPA_100);
                lv_anim_set_duration(&a, TK_MOTION_RETRY_MS);
                lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
                lv_anim_set_path_cb(&a, retry_pulse_path);
                lv_anim_start(&a);
            }
            break;
        }
    }
    // Re-assert theme-owned halo colours in case the palette flipped while
    // the previous state was showing.
    if (s_status_dot_halo_line)
        lv_obj_set_style_bg_color(s_status_dot_halo_line, tk_lv_color(TK_TOKEN_LINE), 0);
    if (s_status_dot_halo_bg)
        lv_obj_set_style_bg_color(s_status_dot_halo_bg, tk_lv_color(TK_TOKEN_BG), 0);
}

static void sync_animation_timer(lv_timer_t *timer)
{
    (void)timer;
    const char *frames[] = { "", ".", "..", "..." };
    if (s_ota_visible) {
        char text[48]; snprintf(text, sizeof(text), "Installing update%s", frames[s_sync_frame++ % 4]);
        lv_label_set_text(s_ota_label, text);
    } else if (s_starting) {
        char text[48]; snprintf(text, sizeof(text), "Starting terminal%s", frames[s_sync_frame++ % 4]);
        lv_label_set_text(s_boot_label, text);
    }
    // Phase 2: the status dot's connecting/syncing/retrying motion is driven
    // by lv_anim on the dot widgets (status_dot_set_state), not by this timer.
}

// Navigation motion (spec §8): a newly shown page slides in from +16px
// (forward) or -10px (return), 180/140ms, ease-out. The incoming screen is
// a persistent container, so cancel any prior slide on it first. Translate
// is a layout-affecting style, so each frame triggers relayout of that
// screen only — cheap for these small fixed pages.
static void set_translate_x_cb(void *obj, int32_t v)
{
    lv_obj_set_style_translate_x((lv_obj_t *)obj, v, 0);
}

static void screen_slide_in(lv_obj_t *screen, int from_offset, uint32_t duration)
{
    if (!screen) return;
    lv_anim_delete(screen, set_translate_x_cb);
    if (s_reduce_motion) {
        lv_obj_set_style_translate_x(screen, 0, 0);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, screen);
    lv_anim_set_exec_cb(&a, set_translate_x_cb);
    lv_anim_set_values(&a, from_offset, 0);
    lv_anim_set_duration(&a, duration);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void show_main_screen(void)
{
    lv_obj_clear_flag(s_main_screen, LV_OBJ_FLAG_HIDDEN);
    if (s_status_screen) lv_obj_add_flag(s_status_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_settings_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_calibration_screen, LV_OBJ_FLAG_HIDDEN);
    if (s_picker_screen) { lv_obj_add_flag(s_picker_screen, LV_OBJ_FLAG_HIDDEN); s_picker_apply = NULL; }
    screen_slide_in(s_main_screen, TK_NAV_BACK_OFFSET, TK_MOTION_NAV_BACK_MS);
}

// Network association and TLS can legitimately take several seconds on an
// ESP32. Never make the keypad unavailable while that happens: after the
// short branded startup moment, let people use the terminal and queue a code
// even if the server is still coming online.
static void startup_timeout_timer(lv_timer_t *timer)
{
    if (s_starting && !s_ota_visible) {
        s_starting = false;
        lv_obj_add_flag(s_boot_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_main_screen, LV_OBJ_FLAG_HIDDEN);
        status_set_token("Connecting in background", TK_TOKEN_BUSY);
    }
    lv_timer_delete(timer);
}

static void show_status_screen(void)
{
    if (!s_status_screen) return;
    refresh_employee_status_list();
    lv_obj_add_flag(s_main_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_settings_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_status_screen, LV_OBJ_FLAG_HIDDEN);
    screen_slide_in(s_status_screen, TK_NAV_FWD_OFFSET, TK_MOTION_NAV_FWD_MS);
}

static void settings_back_event(lv_event_t *event) { show_main_screen(); }
// Sync-now value-area feedback (spec §4): idle ">", busy "Syncing...", then
// "Done" or "Retry". Purely display-driven — network state changes push the
// text; there is NO polling timer. Repeated taps do not stack anything:
// tk_api_wake() only sets flags on the api task (api.c).
static void settings_sync_value_refresh(tk_display_network_state_t state)
{
    if (!s_sync_value_label) return;
    bool busy = state == TK_DISPLAY_SYNCING || (s_sync_requested && state == TK_DISPLAY_CONNECTING);
    if (busy) lv_obj_add_state(s_sync_row, LV_STATE_DISABLED);
    else lv_obj_remove_state(s_sync_row, LV_STATE_DISABLED);
    if (state == TK_DISPLAY_SYNCING) lv_label_set_text(s_sync_value_label, "Syncing...");
    else if (state == TK_DISPLAY_SYNC_RETRYING || state == TK_DISPLAY_OFFLINE) {
        lv_label_set_text(s_sync_value_label, state == TK_DISPLAY_OFFLINE ? "Offline" : "Retry");
        s_sync_requested = false;
    }
    else if (state == TK_DISPLAY_ONLINE && s_sync_requested) {
        lv_label_set_text(s_sync_value_label, "Done");
        s_sync_requested = false; // arm one-shot; next idle shows ">"
    }
    else lv_label_set_text(s_sync_value_label, ">");
}
static void settings_sync_event(lv_event_t *event)
{
    (void)event;
    if (s_sync_requested) return;
    if (s_network_state == TK_DISPLAY_OFFLINE) {
        lv_label_set_text(s_sync_value_label, "Offline");
        tk_api_wake();
        return;
    }
    tk_api_wake();
    s_sync_requested = true;
    lv_obj_add_state(s_sync_row, LV_STATE_DISABLED);
    if (s_sync_value_label) lv_label_set_text(s_sync_value_label, "Syncing...");
    status_set_token("Sync requested", TK_TOKEN_SYNC);
}

static void settings_calibrate_event(lv_event_t *event)
{
    (void)event;
    s_calibrating = true; s_calibration_wait_release = true; s_calibration_step = 0;
    for (int i = 0; i < 4; ++i) {
        if (i == 0) lv_obj_clear_flag(s_calibration_targets[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_calibration_targets[i], LV_OBJ_FLAG_HIDDEN);
    }
    // This label belongs to the calibration screen. Updating the former
    // settings hint here made the instruction disappear behind the overlay.
    lv_label_set_text(s_calibration_progress, "Touch top left target");
    lv_obj_add_flag(s_settings_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_calibration_screen, LV_OBJ_FLAG_HIDDEN);
    screen_slide_in(s_calibration_screen, TK_NAV_FWD_OFFSET, TK_MOTION_NAV_FWD_MS);
}

static void calibration_back_event(lv_event_t *event)
{
    s_calibrating = false;
    lv_obj_add_flag(s_calibration_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_settings_screen, LV_OBJ_FLAG_HIDDEN);
    screen_slide_in(s_settings_screen, TK_NAV_BACK_OFFSET, TK_MOTION_NAV_BACK_MS);
}

// Phase 2 settings handlers. The callbacks below run inside lv_timer_handler
// (LVGL lock already held), so they call lock-free helpers only and never
// take s_lvgl_lock themselves (brief §B).
static void settings_reduce_motion_event(lv_event_t *event);

static void settings_theme_segment_event(lv_event_t *event)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(event);
    tk_config_t updated = *tk_config_get();
    strlcpy(updated.terminal_theme, idx == 1 ? "dark" : "light", sizeof(updated.terminal_theme));
    updated.terminal_theme_override = true;
    tk_config_save(&updated);
    apply_theme_styles();
    // Rebuild the segment highlight (the new selection now matches dark_theme()).
    for (int i = 0; i < 2; ++i) {
        bool selected = i == (dark_theme() ? 1 : 0);
        lv_obj_t *seg = s_theme_segments[i];
        lv_obj_set_style_bg_color(seg, selected ? tk_lv_color(TK_TOKEN_ACCENT) :
                                  tk_lv_color(TK_TOKEN_SURFACE), 0);
        lv_obj_set_style_bg_color(seg, tk_lv_color(TK_TOKEN_ACCENT), LV_STATE_PRESSED);
        lv_obj_set_style_text_color(seg, selected ? lv_color_hex(TK_ACCENT_FG) :
                                    tk_lv_color(TK_TOKEN_INK), 0);
        lv_obj_set_style_text_color(seg, lv_color_hex(TK_ACCENT_FG), LV_STATE_PRESSED);
    }
}

static void settings_reduce_motion_event(lv_event_t *event)
{
    // Two sources converge here (spec §4 "tap anywhere on row"):
    //  - The switch itself: the switch class toggled CHECKED and emitted
    //    LV_EVENT_VALUE_CHANGED (lv_obj.c:745-756).
    //  - The row (CLICKED): the row is not CHECKABLE so nothing toggled the
    //    switch yet. Toggle it here; lv_obj_add_state does NOT emit
    //    VALUE_CHANGED (lv_obj.c:304-312), so this cannot double-fire.
    // This runs inside lv_timer_handler; never take s_lvgl_lock here.
    if (!s_reduce_motion_switch) return;
    if (lv_event_get_code(event) == LV_EVENT_CLICKED &&
        lv_event_get_target(event) != s_reduce_motion_switch) {
        if (lv_obj_has_state(s_reduce_motion_switch, LV_STATE_CHECKED))
            lv_obj_remove_state(s_reduce_motion_switch, LV_STATE_CHECKED);
        else
            lv_obj_add_state(s_reduce_motion_switch, LV_STATE_CHECKED);
    }
    bool reduce = lv_obj_has_state(s_reduce_motion_switch, LV_STATE_CHECKED);
    if (reduce == s_reduce_motion) return;
    s_reduce_motion = reduce;
    tk_config_t updated = *tk_config_get();
    updated.ui_preferences_version = 1;
    updated.reduce_motion = reduce;
    if (tk_config_save(&updated) != ESP_OK) ESP_LOGW(TAG, "Could not persist motion preference");
    if (reduce) {
        for (size_t i = 0; i < s_all_screen_count; ++i) {
            lv_anim_delete(s_all_screens[i], set_translate_x_cb);
            lv_obj_set_style_translate_x(s_all_screens[i], 0, 0);
        }
        for (int i = 0; i < 4; ++i) {
            lv_anim_delete(s_sequence_dots[i], set_scale_cb);
            lv_obj_set_style_transform_scale(s_sequence_dots[i], LV_SCALE_NONE, 0);
        }
    }
    lv_obj_set_style_anim_duration(s_reduce_motion_switch, reduce ? 0 : TK_MOTION_TINT_MS, 0);
    // Turning reduce ON stops the running dot animation immediately
    // (spec §8 "stop immediately when the state changes") and removes the
    // press-scale transforms from the interactive widgets; turning it OFF
    // re-arms both. status_dot_set_state cancels first, so animations
    // cannot stack.
    status_dot_set_state(s_network_state);
    press_feedback_set_all(!reduce);
}

static void open_settings_event(lv_event_t *event)
{
    (void)event;
    clear_keypad_event(NULL);
    lv_obj_add_flag(s_main_screen, LV_OBJ_FLAG_HIDDEN);
    if (s_status_screen) lv_obj_add_flag(s_status_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_settings_screen, LV_OBJ_FLAG_HIDDEN);
    screen_slide_in(s_settings_screen, TK_NAV_FWD_OFFSET, TK_MOTION_NAV_FWD_MS);
    s_calibrating = false;
}

// Helper: a coloured card. Phase 1 keeps the existing radius and 1px border
// because every surface in the design board uses radius >= 6 and a 1px line.
static lv_obj_t *ui_card(lv_obj_t *parent, int x, int y, int width, int height,
                         tk_color_token_t bg, tk_color_token_t border)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card); lv_obj_set_size(card, width, height); lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, tk_lv_color(bg), 0); lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 14, 0); lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, tk_lv_color(border), 0);
    return card;
}

// Helper: a coloured text label at (x,y). The font is the small role by
// default; callers override per-role for headings.
static lv_obj_t *ui_text(lv_obj_t *parent, const char *text, tk_color_token_t color,
                         int x, int y, tk_font_role_t font_role)
{
    lv_obj_t *label = lv_label_create(parent); lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, tk_lv_color(color), 0);
    lv_obj_set_style_text_font(label, tk_font(font_role), 0);
    lv_obj_set_pos(label, x, y);
    return label;
}
static lv_obj_t *ui_text_default(lv_obj_t *parent, const char *text, tk_color_token_t color, int x, int y)
{
    return ui_text(parent, text, color, x, y, TK_FONT_SMALL);
}

static void refresh_employee_status_list(void)
{
    if (!s_employee_status_list) return;
    uint16_t count;
    tk_state_t *state = tk_state_lock();
    count = state->employee_count > TK_MAX_EMPLOYEES ? TK_MAX_EMPLOYEES : state->employee_count;
    memcpy(s_employee_status_cache, state->employees, count * sizeof(tk_employee_t));
    tk_state_unlock();
    lv_obj_clean(s_employee_status_list);
    if (!count) {
        ui_text_default(s_employee_status_list, "No employee data yet.\nSync the terminal, then try again.", TK_TOKEN_MUTED, 12, 16);
        return;
    }
    for (uint16_t i = 0; i < count; ++i) {
        int y = i * 38;
        lv_obj_t *row = ui_card(s_employee_status_list, 0, y, 194, 33, TK_TOKEN_SURFACE, TK_TOKEN_LINE);
        lv_obj_t *dot = lv_obj_create(row); lv_obj_remove_style_all(dot); lv_obj_set_size(dot, 11, 11); lv_obj_set_pos(dot, 11, 11);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, tk_lv_color(s_employee_status_cache[i].clocked_in ? TK_TOKEN_GOOD : TK_TOKEN_MUTED), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_t *name = ui_text_default(row, s_employee_status_cache[i].name, TK_TOKEN_INK, 30, 8);
        lv_obj_set_width(name, 101); lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_t *state_label = ui_text_default(row, s_employee_status_cache[i].clocked_in ? "IN" : "OUT",
                                                s_employee_status_cache[i].clocked_in ? TK_TOKEN_GOOD : TK_TOKEN_MUTED, 0, 8);
        lv_obj_set_width(state_label, 47); lv_obj_set_style_text_align(state_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(state_label, 138, 8);
    }
}

static void status_back_event(lv_event_t *event) { (void)event; show_main_screen(); }

// ----------------------------------------------------------------------------
// Team-status screen (right-swipe destination).
// Re-tokenised onto the new palette and 52px header. Structure and wording
// are unchanged (the design board does not cover this screen — spec §9).
// ----------------------------------------------------------------------------
static void build_status_ui(void)
{
    s_status_screen = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_status_screen);
    lv_obj_set_size(s_status_screen, H_RES, V_RES);
    lv_obj_set_style_bg_color(s_status_screen, tk_lv_color(TK_TOKEN_BG), 0);
    lv_obj_set_style_bg_opa(s_status_screen, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_status_screen, LV_OBJ_FLAG_HIDDEN);
    if (s_all_screen_count < sizeof(s_all_screens)/sizeof(s_all_screens[0]))
        s_all_screens[s_all_screen_count++] = s_status_screen;

    // 52px header, matching the keypad pattern.
    lv_obj_t *header = lv_obj_create(s_status_screen);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, H_RES, TK_HEADER_H);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, tk_lv_color(TK_TOKEN_BG), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(header, tk_lv_color(TK_TOKEN_LINE), 0);
    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Team status");
    lv_obj_set_style_text_color(title, tk_lv_color(TK_TOKEN_INK), 0);
    lv_obj_set_style_text_font(title, tk_font(TK_FONT_BODY), 0);
    lv_obj_set_pos(title, TK_GUTTER, 17);

    ui_text_default(s_status_screen, "Who is in the office", TK_TOKEN_MUTED, TK_GUTTER, 60);

    s_employee_status_list = lv_obj_create(s_status_screen);
    lv_obj_remove_style_all(s_employee_status_list);
    lv_obj_set_size(s_employee_status_list, H_RES - 2 * TK_GUTTER, 168);
    lv_obj_set_pos(s_employee_status_list, TK_GUTTER, 84);
    lv_obj_set_scroll_dir(s_employee_status_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_employee_status_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_all(s_employee_status_list, 0, 0);
    lv_obj_set_style_pad_row(s_employee_status_list, 5, 0);

    lv_obj_t *hint = ui_text_default(s_status_screen, "Swipe left to return", TK_TOKEN_MUTED, 0, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -18);

    lv_obj_t *back = lv_button_create(s_status_screen);
    lv_obj_set_size(back, TK_ROW_W, 44);
    lv_obj_set_pos(back, TK_GUTTER, V_RES - 6 - 44);
    lv_obj_set_style_bg_color(back, tk_lv_color(TK_TOKEN_SURFACE), 0);
    lv_obj_set_style_bg_color(back, tk_lv_color(TK_TOKEN_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_border_color(back, tk_lv_color(TK_TOKEN_LINE), 0);
    lv_obj_set_style_text_color(back, tk_lv_color(TK_TOKEN_INK), 0);
    lv_obj_set_style_text_color(back, lv_color_hex(TK_ACCENT_FG), LV_STATE_PRESSED);
    lv_obj_set_style_text_font(back, tk_font(TK_FONT_BODY), 0);
    lv_obj_add_event_cb(back, status_back_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_text = lv_label_create(back);
    lv_label_set_text(back_text, "Back to keypad");
    lv_obj_center(back_text);

    refresh_employee_status_list();
}

// ----------------------------------------------------------------------------
// Keypad page (the only "main" screen on the terminal).
//
// Vertical budget (spec §3), all y values measured from the screen top:
//   header    h 52   padding 0 12, gap 12
//   sequence  h 36   padding 0 12, items space-between
//   tiles     h 176  2x2 grid, gap 8, padding 0 12  =>  each 104 x 84
//   footer    h 56   padding 6 12
//   ------------------------------
//   total     320
// ----------------------------------------------------------------------------
static void build_clock_ui(void)
{
    s_main_screen = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_main_screen);
    lv_obj_set_size(s_main_screen, H_RES, V_RES);
    lv_obj_set_style_bg_color(s_main_screen, tk_lv_color(TK_TOKEN_BG), 0);
    lv_obj_set_style_bg_opa(s_main_screen, LV_OPA_COVER, 0);
    if (s_all_screen_count < sizeof(s_all_screens)/sizeof(s_all_screens[0]))
        s_all_screens[s_all_screen_count++] = s_main_screen;

    // ---- Header (52px) ----
    s_header = lv_obj_create(s_main_screen);
    lv_obj_remove_style_all(s_header);
    lv_obj_set_size(s_header, H_RES, TK_HEADER_H);
    lv_obj_set_pos(s_header, 0, 0);
    lv_obj_set_style_bg_color(s_header, tk_lv_color(TK_TOKEN_BG), 0);
    lv_obj_set_style_bg_opa(s_header, LV_OPA_COVER, 0);
    // Bottom 1px line separator between header and content (spec §2).
    lv_obj_set_style_border_width(s_header, 1, 0);
    lv_obj_set_style_border_side(s_header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(s_header, tk_lv_color(TK_TOKEN_LINE), 0);

    // Brand label, 14px bold ink, left-aligned at x=12.
    s_brand_label = lv_label_create(s_header);
    lv_label_set_text(s_brand_label, "TimeTone");
    lv_label_set_long_mode(s_brand_label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_size(s_brand_label, 76, 18);
    lv_obj_set_style_text_color(s_brand_label, tk_lv_color(TK_TOKEN_INK), 0);
    lv_obj_set_style_text_font(s_brand_label, tk_font(TK_FONT_BODY), 0);
    lv_obj_set_pos(s_brand_label, 12, 17);
    // LVGL's bundled Montserrat has no bold variant; the accent background
    // fills the role for selected controls. Brand stays a normal-weight label.

    // Status dot (12x12) with halo (bg ring + line ring) — spec §3/§7.
    // The halo is two stacked circles: 20px line ring, 18px bg ring, so the
    // visible halo is 1px of `line` offset 3px from the dot.
    s_status_dot_halo_line = lv_obj_create(s_header);
    lv_obj_remove_style_all(s_status_dot_halo_line);
    lv_obj_set_size(s_status_dot_halo_line, 20, 20);
    lv_obj_set_pos(s_status_dot_halo_line, 138, 16);
    lv_obj_set_style_radius(s_status_dot_halo_line, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_status_dot_halo_line, tk_lv_color(TK_TOKEN_LINE), 0);
    lv_obj_set_style_bg_opa(s_status_dot_halo_line, LV_OPA_COVER, 0);

    s_status_dot_halo_bg = lv_obj_create(s_header);
    lv_obj_remove_style_all(s_status_dot_halo_bg);
    lv_obj_set_size(s_status_dot_halo_bg, 18, 18);
    lv_obj_set_pos(s_status_dot_halo_bg, 139, 17);
    lv_obj_set_style_radius(s_status_dot_halo_bg, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_status_dot_halo_bg, tk_lv_color(TK_TOKEN_BG), 0);
    lv_obj_set_style_bg_opa(s_status_dot_halo_bg, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_status_dot_halo_bg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_status_dot_halo_line, LV_OBJ_FLAG_HIDDEN);

    // The five shapes. s_status_dot doubles as the ONLINE solid circle; the
    // other states are overlay siblings hidden until their state is active.
    // All are non-clickable so a header tap cannot grab the dot.
    s_status_dot = lv_obj_create(s_header);
    lv_obj_remove_style_all(s_status_dot);
    lv_obj_set_size(s_status_dot, TK_DOT_W, TK_DOT_H);
    lv_obj_set_pos(s_status_dot, 142, 20);
    lv_obj_set_style_radius(s_status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_status_dot, tk_lv_color(TK_TOKEN_GOOD), 0);
    lv_obj_set_style_bg_opa(s_status_dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_status_dot, LV_OBJ_FLAG_CLICKABLE);

    // OFFLINE: transparent circle, 1px muted border, diagonal muted slash
    // (a 10x2 bar rotated -45deg per spec §7).
    s_status_dot_offline = lv_obj_create(s_header);
    lv_obj_remove_style_all(s_status_dot_offline);
    lv_obj_set_size(s_status_dot_offline, TK_DOT_W, TK_DOT_H);
    lv_obj_set_pos(s_status_dot_offline, 142, 20);
    lv_obj_set_style_radius(s_status_dot_offline, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_status_dot_offline, 1, 0);
    lv_obj_set_style_border_color(s_status_dot_offline, tk_lv_color(TK_TOKEN_MUTED), 0);
    lv_obj_set_style_bg_opa(s_status_dot_offline, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_status_dot_offline, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_status_dot_offline, LV_OBJ_FLAG_CLICKABLE);
    {
        lv_obj_t *slash = lv_line_create(s_status_dot_offline);
        // Corner-to-corner diagonal of the 12px box, inset 1px so the line
        // caps stay inside the 1px border.
        static const lv_point_precise_t slash_points[] = { {2, 10}, {10, 2} };
        lv_line_set_points(slash, slash_points, 2);
        lv_obj_set_style_line_color(slash, tk_lv_color(TK_TOKEN_MUTED), 0);
        lv_obj_set_style_line_width(slash, 2, 0);
        lv_obj_set_style_line_rounded(slash, true, 0);
        lv_obj_clear_flag(slash, LV_OBJ_FLAG_CLICKABLE);
    }

    // CONNECTING: solid busy circle; opacity pulse owned by status_dot_set_state.
    s_status_dot_connect = lv_obj_create(s_header);
    lv_obj_remove_style_all(s_status_dot_connect);
    lv_obj_set_size(s_status_dot_connect, TK_DOT_W, TK_DOT_H);
    lv_obj_set_pos(s_status_dot_connect, 142, 20);
    lv_obj_set_style_radius(s_status_dot_connect, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_status_dot_connect, tk_lv_color(TK_TOKEN_BUSY), 0);
    lv_obj_set_style_bg_opa(s_status_dot_connect, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_status_dot_connect, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_status_dot_connect, LV_OBJ_FLAG_CLICKABLE);

    // SYNCING: sync-coloured open ring via lv_arc. The visible ring is the
    // background arc (LV_PART_MAIN); indicator and knob are transparent so
    // only the open ring draws. bg angles leave a 90deg gap at 300..360.
    s_status_dot_sync = lv_arc_create(s_header);
    lv_obj_remove_style_all(s_status_dot_sync);
    lv_obj_set_size(s_status_dot_sync, TK_DOT_W, TK_DOT_H);
    lv_obj_set_pos(s_status_dot_sync, 142, 20);
    lv_arc_set_mode(s_status_dot_sync, LV_ARC_MODE_NORMAL);
    lv_arc_set_bg_angles(s_status_dot_sync, 30, 300);
    lv_obj_set_style_arc_color(s_status_dot_sync, tk_lv_color(TK_TOKEN_SYNC), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_status_dot_sync, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_status_dot_sync, false, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_status_dot_sync, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_status_dot_sync, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_status_dot_sync, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(s_status_dot_sync, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_status_dot_sync, LV_OBJ_FLAG_HIDDEN);

    // SYNC_RETRYING: error rounded square (radius 3).
    s_status_dot_retry = lv_obj_create(s_header);
    lv_obj_remove_style_all(s_status_dot_retry);
    lv_obj_set_size(s_status_dot_retry, TK_DOT_W, TK_DOT_H);
    lv_obj_set_pos(s_status_dot_retry, 142, 20);
    lv_obj_set_style_radius(s_status_dot_retry, 3, 0);
    lv_obj_set_style_bg_color(s_status_dot_retry, tk_lv_color(TK_TOKEN_ERROR), 0);
    lv_obj_set_style_bg_opa(s_status_dot_retry, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_status_dot_retry, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_status_dot_retry, LV_OBJ_FLAG_CLICKABLE);

    // Apply the current network state to the freshly built dot.
    status_dot_set_state(s_network_state);

    // Gear button, 44x44 surface, radius 8, 24x24 glyph, right-aligned.
    s_gear_btn = lv_button_create(s_header);
    lv_obj_set_size(s_gear_btn, TK_GEAR_W, TK_GEAR_H);
    lv_obj_set_pos(s_gear_btn, H_RES - TK_GUTTER - TK_GEAR_W, (TK_HEADER_H - TK_GEAR_H) / 2);
    lv_obj_set_style_bg_color(s_gear_btn, tk_lv_color(TK_TOKEN_SURFACE), 0);
    lv_obj_set_style_bg_color(s_gear_btn, tk_lv_color(TK_TOKEN_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_gear_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_gear_btn, 8, 0);
    lv_obj_set_style_border_width(s_gear_btn, 0, 0);
    tint_press_attach(s_gear_btn);
    lv_obj_add_event_cb(s_gear_btn, open_settings_event, LV_EVENT_CLICKED, NULL);
    // Gear glyph (LV_SYMBOL_SETTINGS).
    s_gear_icon = lv_label_create(s_gear_btn);
    lv_label_set_text(s_gear_icon, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(s_gear_icon, tk_font(TK_FONT_LARGE), 0);
    lv_obj_set_style_text_color(s_gear_icon, tk_lv_color(TK_TOKEN_INK), 0);
    lv_obj_set_style_text_color(s_gear_icon, lv_color_hex(TK_ACCENT_FG), LV_STATE_PRESSED);
    lv_obj_center(s_gear_icon);
    lv_obj_clear_flag(s_gear_icon, LV_OBJ_FLAG_CLICKABLE);

    // ---- Sequence band (h=36, y=52..88) ----
    // Feedback label on the left (12px muted, max-width 142, line-height 14).
    s_sequence_label = lv_label_create(s_main_screen);
    lv_obj_set_size(s_sequence_label, 142, 14);
    lv_obj_set_pos(s_sequence_label, TK_GUTTER, TK_HEADER_H + (TK_SEQ_H - 14) / 2);
    lv_label_set_long_mode(s_sequence_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_color(s_sequence_label, tk_lv_color(TK_TOKEN_MUTED), 0);
    lv_obj_set_style_text_font(s_sequence_label, tk_font(TK_FONT_SMALL), 0);
    lv_label_set_text(s_sequence_label, "Your four colours");
    // Status label lives just below the sequence band (spec §4 — visible
    // feedback under the keypad band).
    s_status_label = s_sequence_label;
    lv_obj_set_size(s_status_label, 142, 28);
    lv_obj_set_pos(s_status_label, TK_GUTTER, TK_HEADER_H + 4);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(s_status_label, tk_lv_color(TK_TOKEN_MUTED), 0);
    lv_obj_set_style_text_font(s_status_label, tk_font(TK_FONT_SMALL), 0);

    // Four sequence dots, each 10x10, 7px gap, right-aligned at x=H_RES-12-61.
    {
        const int dots_width = 4 * TK_SEQ_DOT + 3 * TK_SEQ_DOT_GAP;
        int dx = H_RES - TK_GUTTER - dots_width;
        int dy = TK_HEADER_H + (TK_SEQ_H - TK_SEQ_DOT) / 2;
        for (int i = 0; i < 4; ++i) {
            lv_obj_t *dot = lv_obj_create(s_main_screen);
            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, TK_SEQ_DOT, TK_SEQ_DOT);
            lv_obj_set_pos(dot, dx + i * (TK_SEQ_DOT + TK_SEQ_DOT_GAP), dy);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot, tk_lv_color(TK_TOKEN_BG), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(dot, 1, 0);
            lv_obj_set_style_border_color(dot, tk_lv_color(TK_TOKEN_MUTED), 0);
            s_sequence_dots[i] = dot;
        }
    }

    // ---- Tiles (h=176, y=88..264) ----
    // 2x2 grid, gap 8, padding 0 12. Each tile 104 x 84. Row-major order
    // A B / C D matches the web picker (brief constraint D).
    static const char *keys[] = { "A", "B", "C", "D" };
    {
        int tx = TK_GUTTER;
        int ty = TK_HEADER_H + TK_SEQ_H;
        for (int i = 0; i < 4; ++i) {
            lv_obj_t *button = lv_button_create(s_main_screen);
            lv_obj_set_size(button, TK_TILE_W, TK_TILE_H);
            lv_obj_set_pos(button, tx + (i % 2) * (TK_TILE_W + TK_TILE_GAP),
                                    ty + (i / 2) * (TK_TILE_H + TK_TILE_GAP));
            lv_obj_set_style_bg_color(button, lv_color_hex(TK_CODE_COLORS[i]), 0);
            lv_obj_set_style_radius(button, 9, 0);
            lv_obj_set_style_border_width(button, 0, 0);
            // Pressed feedback is the 100ms centre-pivoted scale-to-96%
            // motion (spec §8) — not a colour change.
            press_feedback_attach(button);
            lv_obj_add_event_cb(button, keypad_button_event, LV_EVENT_CLICKED, (void *)keys[i]);
            s_tiles[i] = button;
        }
    }

    // ---- Footer (h=56, y=264..320, padding 6 12) ----
    // Clear button: 216x44 radius 8, 1px line border, surface bg, 15px ink bold.
    s_clear_btn = lv_button_create(s_main_screen);
    lv_obj_set_size(s_clear_btn, TK_CLEAR_W, TK_CLEAR_H);
    lv_obj_set_pos(s_clear_btn, TK_GUTTER, V_RES - TK_FOOTER_H + 6);
    lv_obj_set_style_bg_color(s_clear_btn, tk_lv_color(TK_TOKEN_SURFACE), 0);
    lv_obj_set_style_bg_color(s_clear_btn, tk_lv_color(TK_TOKEN_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_clear_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_clear_btn, tk_lv_color(TK_TOKEN_INK), 0);
    lv_obj_set_style_text_color(s_clear_btn, lv_color_hex(TK_ACCENT_FG), LV_STATE_PRESSED);
    lv_obj_set_style_text_font(s_clear_btn, tk_font(TK_FONT_LARGE), 0);
    lv_obj_set_style_radius(s_clear_btn, 8, 0);
    lv_obj_set_style_border_width(s_clear_btn, 1, 0);
    lv_obj_set_style_border_color(s_clear_btn, tk_lv_color(TK_TOKEN_LINE), 0);
    tint_press_attach(s_clear_btn);
    lv_obj_add_event_cb(s_clear_btn, clear_keypad_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clear_label = lv_label_create(s_clear_btn);
    lv_label_set_text(clear_label, "Clear");
    lv_obj_center(clear_label);

    // Pin label is vestigial but kept so apply_theme_styles has a target.
    s_pin_label = lv_label_create(s_main_screen);
    lv_obj_add_flag(s_pin_label, LV_OBJ_FLAG_HIDDEN);
    s_count_label = lv_label_create(s_main_screen);
    lv_obj_add_flag(s_count_label, LV_OBJ_FLAG_HIDDEN);

    status_set_token("Your four colours", TK_TOKEN_MUTED); update_pin_label();
    lv_timer_create(clock_timer, 1000, NULL); clock_timer(NULL);
    // Phase 2 will replace this timer with LVGL animations on the dot.
    lv_timer_create(sync_animation_timer, 420, NULL);
}

// ----------------------------------------------------------------------------
// Captive-portal (setup) screen — re-tokenised onto the new palette and
// 52px header. Structure and wording unchanged (spec §9 — no design board).
// ----------------------------------------------------------------------------
static void build_setup_ui(void)
{
    s_setup_screen = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_setup_screen);
    lv_obj_set_size(s_setup_screen, H_RES, V_RES);
    lv_obj_set_style_bg_color(s_setup_screen, tk_lv_color(TK_TOKEN_BG), 0);
    lv_obj_set_style_bg_opa(s_setup_screen, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_setup_screen, LV_OBJ_FLAG_HIDDEN);
    if (s_all_screen_count < sizeof(s_all_screens)/sizeof(s_all_screens[0]))
        s_all_screens[s_all_screen_count++] = s_setup_screen;

    // 52px header.
    lv_obj_t *header = lv_obj_create(s_setup_screen);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, H_RES, TK_HEADER_H);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, tk_lv_color(TK_TOKEN_BG), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(header, tk_lv_color(TK_TOKEN_LINE), 0);
    lv_obj_t *brand = lv_label_create(header);
    lv_label_set_text(brand, "TimeTone");
    lv_obj_set_style_text_color(brand, tk_lv_color(TK_TOKEN_INK), 0);
    lv_obj_set_style_text_font(brand, tk_font(TK_FONT_BODY), 0);
    lv_obj_set_pos(brand, TK_GUTTER, 17);
    lv_obj_t *setup_badge = lv_label_create(header);
    lv_label_set_text(setup_badge, "Setup mode");
    lv_obj_set_style_text_color(setup_badge, tk_lv_color(TK_TOKEN_MUTED), 0);
    lv_obj_set_style_text_font(setup_badge, tk_font(TK_FONT_SMALL), 0);
    lv_obj_align(setup_badge, LV_ALIGN_RIGHT_MID, -TK_GUTTER, 0);

    ui_text_default(s_setup_screen, "Let's get this terminal online", TK_TOKEN_INK, TK_GUTTER, 84);
    ui_text_default(s_setup_screen, "No Wi-Fi connection was found.\nUse the temporary network below.",
                    TK_TOKEN_MUTED, TK_GUTTER, 112);

    // Wi-Fi card.
    lv_obj_t *card = ui_card(s_setup_screen, TK_GUTTER, 164, TK_ROW_W, 86, TK_TOKEN_SURFACE, TK_TOKEN_LINE);
    ui_text_default(card, "CONNECT TO WI-FI", TK_TOKEN_MUTED, TK_GUTTER, 12);
    s_setup_details = ui_text_default(card, "", TK_TOKEN_INK, TK_GUTTER, 38);

    // Accent CTA at bottom.
    lv_obj_t *open_card = ui_card(s_setup_screen, TK_GUTTER, 264, TK_ROW_W, 38, TK_TOKEN_ACCENT, TK_TOKEN_ACCENT);
    lv_obj_t *cta = lv_label_create(open_card);
    lv_label_set_text(cta, "Then open  192.168.4.1");
    lv_obj_set_style_text_color(cta, lv_color_hex(TK_ACCENT_FG), 0);
    lv_obj_set_style_text_font(cta, tk_font(TK_FONT_SMALL), 0);
    lv_obj_set_pos(cta, TK_GUTTER, 10);
}

// ----------------------------------------------------------------------------
// Boot screen — re-tokenised; structure and wording unchanged (spec §9).
// ----------------------------------------------------------------------------
static void build_boot_ui(void)
{
    s_boot_screen = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_boot_screen);
    lv_obj_set_size(s_boot_screen, H_RES, V_RES);
    lv_obj_set_style_bg_color(s_boot_screen, tk_lv_color(TK_TOKEN_BG), 0);
    lv_obj_set_style_bg_opa(s_boot_screen, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_boot_screen, LV_OBJ_FLAG_HIDDEN);
    if (s_all_screen_count < sizeof(s_all_screens)/sizeof(s_all_screens[0]))
        s_all_screens[s_all_screen_count++] = s_boot_screen;

    lv_obj_t *mark = ui_card(s_boot_screen, 91, 58, 58, 58, TK_TOKEN_ACCENT, TK_TOKEN_ACCENT);
    lv_obj_t *mark_label = lv_label_create(mark);
    lv_label_set_text(mark_label, "T");
    lv_obj_set_style_text_color(mark_label, lv_color_hex(TK_ACCENT_FG), 0);
    lv_obj_set_style_text_font(mark_label, tk_font(TK_FONT_LARGE), 0);
    lv_obj_center(mark_label);

    lv_obj_t *brand = ui_text_default(s_boot_screen, "TIMETONE", TK_TOKEN_ACCENT, 0, 0);
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 136);

    lv_obj_t *title = ui_text_default(s_boot_screen, "Your office time terminal", TK_TOKEN_INK, 0, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -4);

    s_boot_label = ui_text_default(s_boot_screen, "Starting terminal", TK_TOKEN_MUTED, 0, 0);
    lv_obj_align(s_boot_label, LV_ALIGN_CENTER, 0, 28);

    lv_obj_t *footer = ui_card(s_boot_screen, 30, 260, 180, 32, TK_TOKEN_SURFACE, TK_TOKEN_LINE);
    ui_text_default(footer, "Connecting securely", TK_TOKEN_MUTED, 26, 8);
}

// ----------------------------------------------------------------------------
// OTA screen — re-tokenised; structure and wording unchanged (spec §9).
// ----------------------------------------------------------------------------
static void build_ota_ui(void)
{
    s_ota_screen = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_ota_screen);
    lv_obj_set_size(s_ota_screen, H_RES, V_RES);
    lv_obj_set_style_bg_color(s_ota_screen, tk_lv_color(TK_TOKEN_BG), 0);
    lv_obj_set_style_bg_opa(s_ota_screen, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_ota_screen, LV_OBJ_FLAG_HIDDEN);
    if (s_all_screen_count < sizeof(s_all_screens)/sizeof(s_all_screens[0]))
        s_all_screens[s_all_screen_count++] = s_ota_screen;

    lv_obj_t *brand = lv_label_create(s_ota_screen);
    lv_label_set_text(brand, "TIMETONE");
    lv_obj_set_style_text_color(brand, tk_lv_color(TK_TOKEN_ACCENT), 0);
    lv_obj_set_style_text_font(brand, tk_font(TK_FONT_BODY), 0);
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 76);

    lv_obj_t *title = lv_label_create(s_ota_screen);
    lv_label_set_text(title, "Terminal update");
    lv_obj_set_style_text_color(title, tk_lv_color(TK_TOKEN_INK), 0);
    lv_obj_set_style_text_font(title, tk_font(TK_FONT_BODY), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -24);

    s_ota_label = lv_label_create(s_ota_screen);
    lv_label_set_text(s_ota_label, "Preparing update");
    lv_obj_set_style_text_color(s_ota_label, tk_lv_color(TK_TOKEN_MUTED), 0);
    lv_obj_set_style_text_font(s_ota_label, tk_font(TK_FONT_SMALL), 0);
    lv_obj_align(s_ota_label, LV_ALIGN_CENTER, 0, 12);

    lv_obj_t *hint = lv_label_create(s_ota_screen);
    lv_label_set_text(hint, "Do not disconnect power");
    lv_obj_set_style_text_color(hint, tk_lv_color(TK_TOKEN_MUTED), 0);
    lv_obj_set_style_text_font(hint, tk_font(TK_FONT_SMALL), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -64);
}

// ----------------------------------------------------------------------------
// Settings page (spec §4). Structure:
//   52px header: "Settings" + "Done"
//   Scrollable body, padding 0 12 12.
//     APPEARANCE       group label
//       theme segments  (2 columns, 44px each)
//       reduce motion   switch row (216x48)
//     DISPLAY          group label
//       Screen off      picker row
//       Low power       picker row
//       Calibrate touch action row
//     CONNECTION       group label
//       Sync now        action row
//       Health check    display-only row
//       Settings sync   display-only row
//     trailing note    (managed in web portal)
//
// Phase 1 builds the structure with display-only placeholders for the
// pickers; Phase 2 will add the custom picker page (spec §5) for actual
// selection.
// ----------------------------------------------------------------------------
static lv_obj_t *settings_row(lv_obj_t *parent, const char *label_text, bool is_picker,
                              bool display_only, lv_event_cb_t on_click)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, TK_ROW_W, TK_ROW_H);
    lv_obj_set_style_bg_color(row, tk_lv_color(TK_TOKEN_BG), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, tk_lv_color(TK_TOKEN_LINE), 0);
    lv_obj_set_style_pad_left(row, 0, 0);
    lv_obj_set_style_pad_right(row, 0, 0);

    // Label on the left, 13px -> Montserrat 14.
    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_color(label, tk_lv_color(TK_TOKEN_INK), 0);
    lv_obj_set_style_text_font(label, tk_font(TK_FONT_BODY), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

    if (is_picker || display_only) {
        // Right-hand value area: label + chevron.
        lv_obj_t *value = lv_label_create(row);
        lv_label_set_text(value, "");
        lv_obj_set_style_text_color(value, tk_lv_color(TK_TOKEN_MUTED), 0);
        lv_obj_set_style_text_font(value, tk_font(TK_FONT_SMALL), 0);
        lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(value, LV_ALIGN_RIGHT_MID, -22, 0);
        lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_DOTS);

        lv_obj_t *chevron = lv_label_create(row);
        lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(chevron, display_only ? tk_lv_color(TK_TOKEN_MUTED) : tk_lv_color(TK_TOKEN_INK), 0);
        lv_obj_set_style_text_font(chevron, tk_font(TK_FONT_SMALL), 0);
        lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -2, 0);

        if (on_click) lv_obj_add_event_cb(row, on_click, LV_EVENT_CLICKED, NULL);
        return row;
    }
    // Action row (Sync now / Calibrate touch).
    lv_obj_t *chevron = lv_label_create(row);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(chevron, tk_lv_color(TK_TOKEN_INK), 0);
    lv_obj_set_style_text_font(chevron, tk_font(TK_FONT_SMALL), 0);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -2, 0);
    if (on_click) lv_obj_add_event_cb(row, on_click, LV_EVENT_CLICKED, NULL);
    return row;
}

static void settings_screen_off_pick(lv_event_t *event);
static void settings_low_power_pick(lv_event_t *event);
static void settings_health_pick(lv_event_t *event);
static void settings_interval_pick(lv_event_t *event);

static void settings_follow_server(lv_event_t *event)
{
    (void)event;
    tk_config_t updated = *tk_config_get();
    updated.local_intervals_override = false;
    updated.local_power_override = false;
    updated.terminal_theme_override = false;
    if (tk_config_save(&updated) == ESP_OK) {
        lv_label_set_text(s_sync_value_label, "Requested");
        tk_api_wake();
    } else lv_label_set_text(s_sync_value_label, "Save failed");
}

static void build_settings_ui(void)
{
    s_settings_screen = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_settings_screen);
    lv_obj_set_size(s_settings_screen, H_RES, V_RES);
    lv_obj_set_style_bg_color(s_settings_screen, tk_lv_color(TK_TOKEN_BG), 0);
    lv_obj_set_style_bg_opa(s_settings_screen, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_settings_screen, LV_OBJ_FLAG_HIDDEN);
    if (s_all_screen_count < sizeof(s_all_screens)/sizeof(s_all_screens[0]))
        s_all_screens[s_all_screen_count++] = s_settings_screen;

    // 52px header.
    s_settings_header = lv_obj_create(s_settings_screen);
    lv_obj_remove_style_all(s_settings_header);
    lv_obj_set_size(s_settings_header, H_RES, TK_HEADER_H);
    lv_obj_set_pos(s_settings_header, 0, 0);
    lv_obj_set_style_bg_color(s_settings_header, tk_lv_color(TK_TOKEN_BG), 0);
    lv_obj_set_style_bg_opa(s_settings_header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_settings_header, 1, 0);
    lv_obj_set_style_border_side(s_settings_header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(s_settings_header, tk_lv_color(TK_TOKEN_LINE), 0);

    lv_obj_t *title = lv_label_create(s_settings_header);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, tk_lv_color(TK_TOKEN_INK), 0);
    lv_obj_set_style_text_font(title, tk_font(TK_FONT_BODY), 0);
    lv_obj_set_pos(title, TK_GUTTER, 17);

    // Done button (44px tall, 13px -> Montserrat 14).
    lv_obj_t *done = lv_button_create(s_settings_header);
    lv_obj_set_size(done, 44, 44);
    lv_obj_align(done, LV_ALIGN_RIGHT_MID, -TK_GUTTER, 0);
    lv_obj_set_style_bg_color(done, tk_lv_color(TK_TOKEN_BG), 0);
    lv_obj_set_style_bg_opa(done, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(done, 0, 0);
    lv_obj_set_style_text_color(done, tk_lv_color(TK_TOKEN_INK), 0);
    lv_obj_set_style_text_font(done, tk_font(TK_FONT_BODY), 0);
    lv_obj_add_event_cb(done, settings_back_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *done_label = lv_label_create(done);
    lv_label_set_text(done_label, "Done");
    lv_obj_center(done_label);

    // Scrollable body. Use flex layout so the rows stack vertically with the
    // natural row height (44-48px each); the spec just requires vertical-only
    // scroll, so a column flex is correct. Direction and scrollbar mode are
    // independent (API report §7) — set BOTH. Scrollbar shows only while
    // scrolling and is styled thin/muted via LV_PART_SCROLLBAR.
    s_settings_scroll = lv_obj_create(s_settings_screen);
    lv_obj_remove_style_all(s_settings_scroll);
    lv_obj_set_size(s_settings_scroll, H_RES, V_RES - TK_HEADER_H);
    lv_obj_set_pos(s_settings_scroll, 0, TK_HEADER_H);
    lv_obj_set_scroll_dir(s_settings_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_settings_scroll, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_pad_top(s_settings_scroll, 0, 0);
    lv_obj_set_style_pad_bottom(s_settings_scroll, TK_GUTTER, 0);
    lv_obj_set_style_pad_left(s_settings_scroll, TK_GUTTER, 0);
    lv_obj_set_style_pad_right(s_settings_scroll, TK_GUTTER, 0);
    lv_obj_set_style_width(s_settings_scroll, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_settings_scroll, tk_lv_color(TK_TOKEN_MUTED), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_settings_scroll, LV_OPA_40, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_settings_scroll, 1, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(s_settings_scroll, 2, LV_PART_SCROLLBAR);
    lv_obj_set_layout(s_settings_scroll, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_settings_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_settings_scroll, 0, 0);

    // APPEARANCE group.
    {
        lv_obj_t *label = lv_label_create(s_settings_scroll);
        lv_label_set_text(label, "APPEARANCE");
        lv_obj_set_style_text_color(label, tk_lv_color(TK_TOKEN_MUTED), 0);
        lv_obj_set_style_text_font(label, tk_font(TK_FONT_SMALL), 0);
        lv_obj_set_style_margin_top(label, TK_GROUP_TOP_PAD, 0);
        lv_obj_set_style_margin_bottom(label, TK_GROUP_BOT_PAD, 0);
    }
    {
        // theme segments — 2 columns in a surface bg, radius 8, 3px inset.
        s_theme_segment_bg = lv_obj_create(s_settings_scroll);
        lv_obj_remove_style_all(s_theme_segment_bg);
        lv_obj_set_size(s_theme_segment_bg, TK_ROW_W, 50);
        lv_obj_set_style_bg_color(s_theme_segment_bg, tk_lv_color(TK_TOKEN_SURFACE), 0);
        lv_obj_set_style_bg_opa(s_theme_segment_bg, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_theme_segment_bg, 8, 0);
        lv_obj_set_style_pad_left(s_theme_segment_bg, 3, 0);
        lv_obj_set_style_pad_right(s_theme_segment_bg, 3, 0);
        lv_obj_set_style_pad_top(s_theme_segment_bg, 3, 0);
        lv_obj_set_style_pad_bottom(s_theme_segment_bg, 3, 0);
        lv_obj_set_style_pad_column(s_theme_segment_bg, 4, 0);
        lv_obj_set_layout(s_theme_segment_bg, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(s_theme_segment_bg, LV_FLEX_FLOW_ROW);
        const char *names[2] = { "Light", "Dark" };
        for (int i = 0; i < 2; ++i) {
            lv_obj_t *seg = lv_button_create(s_theme_segment_bg);
            lv_obj_set_size(seg, (TK_ROW_W - 10) / 2, 44);
            bool selected = i == (dark_theme() ? 1 : 0);
            lv_obj_set_style_bg_color(seg, selected ? tk_lv_color(TK_TOKEN_ACCENT) :
                                      tk_lv_color(TK_TOKEN_SURFACE), 0);
            lv_obj_set_style_bg_color(seg, tk_lv_color(TK_TOKEN_ACCENT), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(seg, 6, 0);
            lv_obj_set_style_border_width(seg, 0, 0);
            lv_obj_set_style_text_color(seg, selected ? lv_color_hex(TK_ACCENT_FG) :
                                        tk_lv_color(TK_TOKEN_INK), 0);
            lv_obj_set_style_text_color(seg, lv_color_hex(TK_ACCENT_FG), LV_STATE_PRESSED);
            lv_obj_set_style_text_font(seg, tk_font(TK_FONT_BODY), 0);
            // Control-state colour change animates over 120ms (spec §8).
            tint_press_attach(seg);
            lv_obj_add_event_cb(seg, settings_theme_segment_event, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *seg_label = lv_label_create(seg);
            lv_label_set_text(seg_label, names[i]);
            lv_obj_center(seg_label);
            s_theme_segments[i] = seg;
        }
    }

    // Reduce-motion row (216x48). Whole row tappable; switch on the right.
    {
        lv_obj_t *row = lv_obj_create(s_settings_scroll);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, TK_ROW_W, TK_ROW_H);
        lv_obj_set_style_bg_color(row, tk_lv_color(TK_TOKEN_BG), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, tk_lv_color(TK_TOKEN_LINE), 0);

        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, "Reduce motion");
        lv_obj_set_style_text_color(label, tk_lv_color(TK_TOKEN_INK), 0);
        lv_obj_set_style_text_font(label, tk_font(TK_FONT_BODY), 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

        // LVGL switch widget, track 36x22 (spec §4). The knob is a square of
        // the track HEIGHT (lv_switch.c:216 — knob_size = obj height), so a
        // 36x22 track gives a 22px knob travelling exactly the spec's 14px
        // (36-22). A 16px knob is not expressible on a 22px-high track.
        // Track muted when off, good when on; knob travels with the switch's
        // own anim_duration (120ms, spec §8 "Control state").
        s_reduce_motion_switch = lv_switch_create(row);
        lv_obj_set_size(s_reduce_motion_switch, 36, 22);
        lv_obj_align(s_reduce_motion_switch, LV_ALIGN_RIGHT_MID, -2, 0);
        lv_obj_set_style_radius(s_reduce_motion_switch, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_reduce_motion_switch, tk_lv_color(TK_TOKEN_MUTED), 0);
        lv_obj_set_style_bg_opa(s_reduce_motion_switch, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_reduce_motion_switch, tk_lv_color(TK_TOKEN_GOOD), LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(s_reduce_motion_switch, tk_lv_color(TK_TOKEN_SURFACE), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_reduce_motion_switch, tk_lv_color(TK_TOKEN_GOOD), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(s_reduce_motion_switch, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(s_reduce_motion_switch, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(s_reduce_motion_switch, tk_lv_color(TK_TOKEN_SURFACE), LV_PART_KNOB);
        lv_obj_set_style_anim_duration(s_reduce_motion_switch, TK_MOTION_TINT_MS, LV_PART_MAIN);
        if (s_reduce_motion) lv_obj_add_state(s_reduce_motion_switch, LV_STATE_CHECKED);
        // Tapping the switch itself fires VALUE_CHANGED from the switch class
        // (lv_obj.c:751). Tapping the row elsewhere fires the row's CLICKED;
        // the handler mirrors the switch so both paths converge. lv_obj_add_state
        // does NOT emit VALUE_CHANGED (verified in lv_obj.c:304-312), so the
        // initial sync above cannot double-fire.
        lv_obj_add_event_cb(s_reduce_motion_switch, settings_reduce_motion_event,
                            LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(row, settings_reduce_motion_event, LV_EVENT_CLICKED, NULL);
    }

    // DISPLAY group.
    {
        lv_obj_t *label = lv_label_create(s_settings_scroll);
        lv_label_set_text(label, "DISPLAY");
        lv_obj_set_style_text_color(label, tk_lv_color(TK_TOKEN_MUTED), 0);
        lv_obj_set_style_text_font(label, tk_font(TK_FONT_SMALL), 0);
        lv_obj_set_style_margin_top(label, TK_GROUP_TOP_PAD, 0);
        lv_obj_set_style_margin_bottom(label, TK_GROUP_BOT_PAD, 0);
    }
    {
        lv_obj_t *row = settings_row(s_settings_scroll, "Screen off", true, false, settings_screen_off_pick);
        s_screen_off_value_label = lv_obj_get_child(row, 1);
    }
    {
        lv_obj_t *row = settings_row(s_settings_scroll, "Low power", true, false, settings_low_power_pick);
        s_low_power_value_label = lv_obj_get_child(row, 1);
    }
    {
        lv_obj_t *row = settings_row(s_settings_scroll, "Calibrate touch", false, false, settings_calibrate_event);
        (void)row;
    }

    // CONNECTION group.
    {
        lv_obj_t *label = lv_label_create(s_settings_scroll);
        lv_label_set_text(label, "CONNECTION");
        lv_obj_set_style_text_color(label, tk_lv_color(TK_TOKEN_MUTED), 0);
        lv_obj_set_style_text_font(label, tk_font(TK_FONT_SMALL), 0);
        lv_obj_set_style_margin_top(label, TK_GROUP_TOP_PAD, 0);
        lv_obj_set_style_margin_bottom(label, TK_GROUP_BOT_PAD, 0);
    }
    {
        lv_obj_t *row = settings_row(s_settings_scroll, "Sync now", false, false, settings_sync_event);
        s_sync_row = row;
        s_sync_value_label = lv_obj_get_child(row, 1);
    }
    {
        // Health check — display-only row.
        lv_obj_t *row = settings_row(s_settings_scroll, "Health check", true, false, settings_health_pick);
        s_health_check_value_label = lv_obj_get_child(row, 1);
    }
    {
        lv_obj_t *row = settings_row(s_settings_scroll, "Settings sync", true, false, settings_interval_pick);
        s_settings_sync_value_label = lv_obj_get_child(row, 1);
    }

    settings_row(s_settings_scroll, "Use web settings", false, false, settings_follow_server);

    // Trailing note.
    lv_obj_t *note = lv_label_create(s_settings_scroll);
    lv_label_set_text(note, "Changes are saved on this terminal.");
    lv_obj_set_style_text_color(note, tk_lv_color(TK_TOKEN_MUTED), 0);
    lv_obj_set_style_text_font(note, tk_font(TK_FONT_SMALL), 0);
    lv_obj_set_width(note, TK_ROW_W);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);

    rebuild_settings_picker_labels();
}

// Helper: format screen-off / low-power / health / sync picker values.
static const char *format_timeout(uint16_t seconds)
{
    static char buf[16];
    if (seconds == 0) return "Never";
    if (seconds < 60) snprintf(buf, sizeof(buf), "%u sec", (unsigned)seconds);
    else snprintf(buf, sizeof(buf), "%u min", (unsigned)(seconds / 60));
    return buf;
}

static void rebuild_settings_picker_labels(void)
{
    tk_config_t cfg = *tk_config_get();
    if (s_screen_off_value_label)
        lv_label_set_text(s_screen_off_value_label, format_timeout(cfg.screen_off_timeout_seconds));
    if (s_low_power_value_label)
        lv_label_set_text(s_low_power_value_label, format_timeout(cfg.low_power_timeout_seconds));
    if (s_health_check_value_label)
        lv_label_set_text(s_health_check_value_label, format_timeout(cfg.sync_interval_seconds));
    if (s_settings_sync_value_label)
        lv_label_set_text(s_settings_sync_value_label, format_timeout(cfg.full_sync_interval_seconds));
}

// ----------------------------------------------------------------------------
// Calibration page (spec §6). 44x44 targets, 2px ink stroke, accent fill,
// "+" glyph. Target centres (34,130), (194,130), (194,280), (34,280).
// ----------------------------------------------------------------------------
static void build_calibration_ui(void)
{
    s_calibration_screen = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_calibration_screen);
    lv_obj_set_size(s_calibration_screen, H_RES, V_RES);
    lv_obj_set_style_bg_color(s_calibration_screen, tk_lv_color(TK_TOKEN_BG), 0);
    lv_obj_set_style_bg_opa(s_calibration_screen, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_calibration_screen, LV_OBJ_FLAG_HIDDEN);
    if (s_all_screen_count < sizeof(s_all_screens)/sizeof(s_all_screens[0]))
        s_all_screens[s_all_screen_count++] = s_calibration_screen;

    // 52px header.
    s_calibration_header = lv_obj_create(s_calibration_screen);
    lv_obj_remove_style_all(s_calibration_header);
    lv_obj_set_size(s_calibration_header, H_RES, TK_HEADER_H);
    lv_obj_set_pos(s_calibration_header, 0, 0);
    lv_obj_set_style_bg_color(s_calibration_header, tk_lv_color(TK_TOKEN_BG), 0);
    lv_obj_set_style_bg_opa(s_calibration_header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_calibration_header, 1, 0);
    lv_obj_set_style_border_side(s_calibration_header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(s_calibration_header, tk_lv_color(TK_TOKEN_LINE), 0);

    lv_obj_t *title = lv_label_create(s_calibration_header);
    lv_label_set_text(title, "Calibrate touch");
    lv_obj_set_style_text_color(title, tk_lv_color(TK_TOKEN_INK), 0);
    lv_obj_set_style_text_font(title, tk_font(TK_FONT_BODY), 0);
    lv_obj_set_pos(title, TK_GUTTER, 17);

    // Back button (44px tall).
    lv_obj_t *back = lv_button_create(s_calibration_header);
    lv_obj_set_size(back, 44, 44);
    lv_obj_align(back, LV_ALIGN_RIGHT_MID, -TK_GUTTER, 0);
    lv_obj_set_style_bg_color(back, tk_lv_color(TK_TOKEN_BG), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_text_color(back, tk_lv_color(TK_TOKEN_INK), 0);
    lv_obj_set_style_text_font(back, tk_font(TK_FONT_BODY), 0);
    lv_obj_add_event_cb(back, calibration_back_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);

    // Hint text (centred, 12px muted).
    s_calibration_progress = lv_label_create(s_calibration_screen);
    lv_label_set_text(s_calibration_progress, "Touch the top-left target");
    lv_obj_set_style_text_color(s_calibration_progress, tk_lv_color(TK_TOKEN_MUTED), 0);
    lv_obj_set_style_text_font(s_calibration_progress, tk_font(TK_FONT_SMALL), 0);
    lv_obj_set_style_text_align(s_calibration_progress, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_calibration_progress, H_RES - 2 * TK_GUTTER);
    lv_obj_set_pos(s_calibration_progress, TK_GUTTER, 24 + TK_HEADER_H);

    // Four targets.
    static const char *labels[] = { "+", "+", "+", "+" };
    for (int i = 0; i < 4; ++i) {
        // 44x44 circle, 2px ink stroke, accent fill.
        lv_obj_t *target = lv_obj_create(s_calibration_screen);
        lv_obj_remove_style_all(target);
        lv_obj_set_size(target, TK_TARGET, TK_TARGET);
        lv_obj_set_pos(target, TK_CAL_TARGETS[i][0] - TK_TARGET / 2, TK_CAL_TARGETS[i][1] - TK_TARGET / 2);
        lv_obj_set_style_radius(target, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(target, tk_lv_color(TK_TOKEN_ACCENT), 0);
        lv_obj_set_style_bg_opa(target, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(target, 2, 0);
        lv_obj_set_style_border_color(target, tk_lv_color(TK_TOKEN_INK), 0);
        // Make the target click-transparent so the calibration handler in
        // touch_cb keeps receiving raw touches (LVGL button events would
        // otherwise swallow them).
        lv_obj_clear_flag(target, LV_OBJ_FLAG_CLICKABLE);
        s_calibration_targets[i] = target;

        lv_obj_t *glyph = lv_label_create(target);
        lv_label_set_text(glyph, labels[i]);
        lv_obj_set_style_text_color(glyph, lv_color_hex(TK_ACCENT_FG), 0);
        lv_obj_set_style_text_font(glyph, tk_font(TK_FONT_LARGE), 0);
        lv_obj_center(glyph);
        s_calibration_target_labels[i] = glyph;
    }
}

// ----------------------------------------------------------------------------
// Custom picker page (spec §5). One page, reused by every settings picker
// row: the opening row supplies title, choice labels, choice values, the
// current value, and a commit callback. Selecting commits once and returns
// to settings; Back cancels without committing.
//
// Lock discipline (brief §B): all of these run inside lv_timer_handler via
// event callbacks — they NEVER take s_lvgl_lock and only call lock-free
// helpers.
// ----------------------------------------------------------------------------
static void picker_close(void)
{
    lv_obj_add_flag(s_picker_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_settings_screen, LV_OBJ_FLAG_HIDDEN);
    screen_slide_in(s_settings_screen, TK_NAV_BACK_OFFSET, TK_MOTION_NAV_BACK_MS);
    s_picker_apply = NULL;
}

static void picker_back_event(lv_event_t *event)
{
    (void)event;
    picker_close(); // cancel: no commit
}

static void picker_choice_event(lv_event_t *event)
{
    if (!s_picker_apply) return;
    uint16_t value = s_picker_values[(uint8_t)(intptr_t)lv_event_get_user_data(event)];
    void (*apply)(uint16_t) = s_picker_apply;
    picker_close(); // commit exactly once, then return to settings
    apply(value);
}

// Fill the picker page for one setting. `apply` runs after the page closes,
// still under the LVGL lock (caller context), so it must stay lock-free.
static void picker_open(const char *title, const char *const labels[],
                        const uint16_t values[], uint8_t count,
                        uint16_t current, void (*apply)(uint16_t))
{
    lv_label_set_text(s_picker_title_label, title);
    lv_obj_clean(s_picker_list);
    lv_obj_scroll_to_y(s_picker_list, 0, LV_ANIM_OFF);
    for (uint8_t i = 0; i < count; ++i) {
        bool selected = values[i] == current;
        // Row: 216x48 (100% width minus gutters), radius 6, transparent bg,
        // 14px ink label left, 18px check right when selected (spec §5).
        lv_obj_t *row = lv_obj_create(s_picker_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, TK_ROW_W, TK_ROW_H);
        // The list's own pad_top=8 handles the spec's "padding-top 8"; rows
        // are stacked at content-y = i * row height.
        lv_obj_set_pos(row, TK_GUTTER, i * TK_ROW_H);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(row, tk_lv_color(TK_TOKEN_SURFACE), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        if (selected) {
            lv_obj_set_style_bg_color(row, tk_lv_color(TK_TOKEN_ACCENT), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        }
        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, labels[i]);
        lv_obj_set_style_text_color(label,
            selected ? lv_color_hex(TK_ACCENT_FG) : tk_lv_color(TK_TOKEN_INK), 0);
        lv_obj_set_style_text_font(label, tk_font(TK_FONT_BODY), 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(label, TK_ROW_W - 12 - 26);
        if (selected) {
            // 18x18 check mark (LV_SYMBOL_OK in the large role ~ 16px).
            lv_obj_t *check = lv_label_create(row);
            lv_label_set_text(check, LV_SYMBOL_OK);
            lv_obj_set_style_text_color(check, lv_color_hex(TK_ACCENT_FG), 0);
            lv_obj_set_style_text_font(check, tk_font(TK_FONT_LARGE), 0);
            lv_obj_align(check, LV_ALIGN_RIGHT_MID, -6, 0);
            lv_obj_clear_flag(check, LV_OBJ_FLAG_CLICKABLE);
        }
        lv_obj_add_event_cb(row, picker_choice_event, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }
    s_picker_values = values;
    s_picker_apply = apply;
    lv_obj_add_flag(s_settings_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_picker_screen, LV_OBJ_FLAG_HIDDEN);
    screen_slide_in(s_picker_screen, TK_NAV_FWD_OFFSET, TK_MOTION_NAV_FWD_MS);
}

// ----------------------------------------------------------------------------
// Picker commit callbacks. Called from picker_choice_event (inside the LVGL
// lock) after the picker page has closed; they persist the value and refresh
// the settings row labels. All lock-free.
// ----------------------------------------------------------------------------
static void picker_apply_screen_off(uint16_t value)
{
    tk_config_t updated = *tk_config_get();
    updated.screen_off_timeout_seconds = value;
    updated.power_timeouts_configured = true;
    updated.local_power_override = true;
    updated.ui_preferences_version = 1;
    // Spec §4 validation: low-power must be 0, or strictly later than
    // screen-off.
    if (updated.low_power_timeout_seconds &&
        (!value || updated.low_power_timeout_seconds < value))
        updated.low_power_timeout_seconds = 0;
    tk_config_save(&updated);
    rebuild_settings_picker_labels();
}

static void picker_apply_low_power(uint16_t value)
{
    tk_config_t updated = *tk_config_get();
    // Spec §4 validation: low-power must be Never when screen-off is Never
    // (0), or strictly later than screen-off. Otherwise refuse by forcing 0.
    if (updated.screen_off_timeout_seconds && value &&
        value < updated.screen_off_timeout_seconds) {
        value = 0;
    }
    if (!updated.screen_off_timeout_seconds) value = 0;
    updated.low_power_timeout_seconds = value;
    updated.power_timeouts_configured = true;
    updated.local_power_override = true;
    updated.ui_preferences_version = 1;
    tk_config_save(&updated);
    rebuild_settings_picker_labels();
}

// Settings rows now route through the picker page instead of committing a
// hard-coded value.
static void settings_screen_off_pick(lv_event_t *event)
{
    (void)event;
    static const char *const labels[] = { "30 sec", "1 min", "2 min", "5 min", "Never" };
    static const uint16_t values[] = { 30, 60, 120, 300, 0 };
    picker_open("Screen off", labels, values, 5,
                tk_config_get()->screen_off_timeout_seconds, picker_apply_screen_off);
}

static void settings_low_power_pick(lv_event_t *event)
{
    (void)event;
    static const char *const labels[] = { "2 min", "5 min", "10 min", "Never" };
    static const uint16_t values[] = { 120, 300, 600, 0 };
    picker_open("Low power", labels, values, 4,
                tk_config_get()->low_power_timeout_seconds, picker_apply_low_power);
}

static void picker_apply_health(uint16_t value)
{
    tk_config_t updated = *tk_config_get();
    updated.sync_interval_seconds = value;
    updated.local_intervals_override = true;
    updated.ui_preferences_version = 1;
    if (tk_config_save(&updated) != ESP_OK) ESP_LOGW(TAG, "Could not save health interval");
    rebuild_settings_picker_labels();
    tk_api_wake();
}

static void picker_apply_interval(uint16_t value)
{
    tk_config_t updated = *tk_config_get();
    updated.full_sync_interval_seconds = value;
    updated.local_intervals_override = true;
    updated.ui_preferences_version = 1;
    if (tk_config_save(&updated) != ESP_OK) ESP_LOGW(TAG, "Could not save sync interval");
    rebuild_settings_picker_labels();
    tk_api_wake();
}

static void settings_health_pick(lv_event_t *event)
{
    (void)event;
    static const char *const labels[] = { "5 sec", "15 sec", "30 sec", "60 sec" };
    static const uint16_t values[] = { 5, 15, 30, 60 };
    picker_open("Health check", labels, values, 4, tk_config_get()->sync_interval_seconds, picker_apply_health);
}

static void settings_interval_pick(lv_event_t *event)
{
    (void)event;
    static const char *const labels[] = { "5 min", "10 min", "30 min" };
    static const uint16_t values[] = { 300, 600, 1800 };
    picker_open("Settings sync", labels, values, 3, tk_config_get()->full_sync_interval_seconds, picker_apply_interval);
}

static void build_picker_ui(void)
{
    s_picker_screen = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_picker_screen);
    lv_obj_set_size(s_picker_screen, H_RES, V_RES);
    lv_obj_set_style_bg_color(s_picker_screen, tk_lv_color(TK_TOKEN_BG), 0);
    lv_obj_set_style_bg_opa(s_picker_screen, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_picker_screen, LV_OBJ_FLAG_HIDDEN);
    if (s_all_screen_count < sizeof(s_all_screens)/sizeof(s_all_screens[0]))
        s_all_screens[s_all_screen_count++] = s_picker_screen;

    s_picker_header = lv_obj_create(s_picker_screen);
    lv_obj_remove_style_all(s_picker_header);
    lv_obj_set_size(s_picker_header, H_RES, TK_HEADER_H);
    lv_obj_set_pos(s_picker_header, 0, 0);
    lv_obj_set_style_bg_color(s_picker_header, tk_lv_color(TK_TOKEN_BG), 0);
    lv_obj_set_style_bg_opa(s_picker_header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_picker_header, 1, 0);
    lv_obj_set_style_border_side(s_picker_header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(s_picker_header, tk_lv_color(TK_TOKEN_LINE), 0);

    s_picker_title_label = lv_label_create(s_picker_header);
    lv_label_set_text(s_picker_title_label, "");
    lv_obj_set_style_text_color(s_picker_title_label, tk_lv_color(TK_TOKEN_INK), 0);
    lv_obj_set_style_text_font(s_picker_title_label, tk_font(TK_FONT_BODY), 0);
    lv_obj_set_pos(s_picker_title_label, TK_GUTTER, 17);

    // Back button (44px target) — cancels without committing.
    lv_obj_t *back = lv_button_create(s_picker_header);
    lv_obj_set_size(back, 44, 44);
    lv_obj_align(back, LV_ALIGN_RIGHT_MID, -TK_GUTTER, 0);
    lv_obj_set_style_bg_color(back, tk_lv_color(TK_TOKEN_BG), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_text_color(back, tk_lv_color(TK_TOKEN_INK), 0);
    lv_obj_set_style_text_font(back, tk_font(TK_FONT_BODY), 0);
    lv_obj_add_event_cb(back, picker_back_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);

    // Choice list. Vertical-only scroll, active-mode scrollbar (Phase 2
    // scrolling rule: set BOTH scroll_dir and scrollbar_mode).
    s_picker_list = lv_obj_create(s_picker_screen);
    lv_obj_remove_style_all(s_picker_list);
    lv_obj_set_size(s_picker_list, H_RES, V_RES - TK_HEADER_H);
    lv_obj_set_pos(s_picker_list, 0, TK_HEADER_H);
    lv_obj_set_scroll_dir(s_picker_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_picker_list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_pad_top(s_picker_list, 8, 0);
    // Thin muted scrollbar via LV_PART_SCROLLBAR (spec §4 scrolling).
    lv_obj_set_style_width(s_picker_list, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_picker_list, tk_lv_color(TK_TOKEN_MUTED), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_picker_list, LV_OPA_40, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_picker_list, 1, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(s_picker_list, 2, LV_PART_SCROLLBAR);
}

static void finalize_touch_layout(lv_obj_t *obj)
{
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
    if (obj != s_settings_scroll && obj != s_picker_list && obj != s_employee_status_list)
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
        finalize_touch_layout(lv_obj_get_child(obj, i));
}

esp_err_t tk_display_init(void)
{
    s_reduce_motion = tk_config_get()->ui_preferences_version == 1 && tk_config_get()->reduce_motion;
    gpio_config_t backlight = { .pin_bit_mask = (1ULL << PIN_LCD_BL) | (1ULL << PIN_LCD_BL_ALT), .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&backlight)); gpio_set_level(PIN_LCD_BL, 0); gpio_set_level(PIN_LCD_BL_ALT, 0);
    spi_bus_config_t lcd_bus = { .sclk_io_num = PIN_LCD_SCLK, .mosi_io_num = PIN_LCD_MOSI, .miso_io_num = PIN_LCD_MISO, .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = H_RES * 40 * 2 };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &lcd_bus, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t io_config = { .dc_gpio_num = PIN_LCD_DC, .cs_gpio_num = PIN_LCD_CS, .pclk_hz = 40 * 1000 * 1000, .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 10 };
    esp_lcd_panel_io_handle_t io;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io));
    // The web palette is RGB; use the panel's RGB order so Coral/Ocean/Lime/
    // Violet render with the same hues on the physical terminal.
    esp_lcd_panel_dev_config_t panel_config = { .reset_gpio_num = PIN_LCD_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16 };
    esp_lcd_panel_handle_t panel;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel)); ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    // The S032 uses a native 240x320 ST7789 portrait panel. Clear MV/MX/MY
    // for the requested 90-degree counter-clockwise portrait orientation.
    // This panel revision already uses normal polarity.  Inverting here makes
    // the palette render as its complementary colors (Coral becomes cyan,
    // Ocean becomes yellow, etc.), so leave inversion disabled.
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false)); ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, false)); ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, false, false)); ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    lv_init();
    s_display = lv_display_create(H_RES, V_RES);
    size_t buffer_size = H_RES * 30 * sizeof(lv_color16_t);
    void *buffer1 = spi_bus_dma_memory_alloc(LCD_HOST, buffer_size, 0), *buffer2 = spi_bus_dma_memory_alloc(LCD_HOST, buffer_size, 0);
    assert(buffer1 && buffer2);
    lv_display_set_buffers(s_display, buffer1, buffer2, buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(s_display, panel); lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565); lv_display_set_flush_cb(s_display, flush_cb);
    esp_lcd_panel_io_callbacks_t callbacks = { .on_color_trans_done = flush_done };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io, &callbacks, s_display));
    // The XPT2046 on this unit is on the dedicated touch SPI bus.
    spi_bus_config_t touch_bus = { .sclk_io_num = PIN_TOUCH_SCLK, .mosi_io_num = PIN_TOUCH_MOSI, .miso_io_num = PIN_TOUCH_MISO, .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 64 };
    ESP_ERROR_CHECK(spi_bus_initialize(TOUCH_HOST, &touch_bus, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t touch_io_config = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(PIN_TOUCH_CS);
    esp_lcd_panel_io_handle_t touch_io; ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)TOUCH_HOST, &touch_io_config, &touch_io));
    // Portrait panel coordinates are correct except for the resistive layer's
    // left/right orientation on this board revision.
    esp_lcd_touch_config_t touch_config = { .x_max = H_RES, .y_max = V_RES, .rst_gpio_num = -1, .int_gpio_num = PIN_TOUCH_IRQ, .flags = { .swap_xy = 0, .mirror_x = 1, .mirror_y = 0 } };
    esp_lcd_touch_handle_t touch; ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(touch_io, &touch_config, &touch));
    s_touch = touch;
    lv_indev_t *indev = lv_indev_create(); lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER); lv_indev_set_display(indev, s_display); lv_indev_set_user_data(indev, touch); lv_indev_set_read_cb(indev, touch_cb);
    // Static style-transition descriptors must exist before the UI builders
    // attach them (they are stored by pointer — API report §5).
    motion_descriptors_init();
    const esp_timer_create_args_t tick_args = { .callback = tick_cb, .name = "lvgl_tick" };
    esp_timer_handle_t timer; ESP_ERROR_CHECK(esp_timer_create(&tick_args, &timer)); ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 2000));
    build_clock_ui(); build_status_ui(); build_setup_ui(); build_settings_ui(); build_calibration_ui(); build_picker_ui(); build_boot_ui(); build_ota_ui(); tk_display_refresh();
    finalize_touch_layout(lv_screen_active());
    press_feedback_set_all(!s_reduce_motion);
    char current_ip[16]; tk_network_ip(current_ip, sizeof(current_ip)); tk_display_set_ip(current_ip);
    if (s_server_label) lv_label_set_text_fmt(s_server_label, "Server: %s", tk_config_get()->server_url[0] ? tk_config_get()->server_url : "not configured");
    xTaskCreate(lvgl_task, "lvgl", 6144, NULL, 5, NULL);
    s_last_activity_us = esp_timer_get_time(); set_backlight(true);
    ESP_LOGI(TAG, "display initialized");
    return ESP_OK;
}

void tk_display_set_network_state(tk_display_network_state_t state)
{
    if (!s_status_label || !s_setup_screen) return;
    _lock_acquire(&s_lvgl_lock);
    bool changed = s_network_state != state;
    s_network_state = state;
    s_online = state == TK_DISPLAY_ONLINE;
    // Five-shape status dot (spec §7). status_dot_set_state cancels the
    // previous state's animation before starting the next.
    if (changed) status_dot_set_state(state);
    // Sync-now value-area feedback follows the same push (no polling timer).
    settings_sync_value_refresh(state);
    if (state != TK_DISPLAY_OFFLINE) {
        if (s_starting && state != TK_DISPLAY_ONLINE && state != TK_DISPLAY_SYNC_RETRYING) { _lock_release(&s_lvgl_lock); return; }
        if (state == TK_DISPLAY_ONLINE || state == TK_DISPLAY_SYNC_RETRYING) { s_starting = false; lv_obj_add_flag(s_boot_screen, LV_OBJ_FLAG_HIDDEN); }
        lv_obj_clear_flag(s_main_screen, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s_setup_screen, LV_OBJ_FLAG_HIDDEN);
        // Health checks run frequently. Preserve useful interaction feedback
        // (such as a successful clock-in) while the connection state is
        // unchanged instead of rewriting it every few seconds.
    } else {
        if (s_starting) { _lock_release(&s_lvgl_lock); return; }
        char details[96]; snprintf(details, sizeof(details), "%s\nPassword: timekeep", tk_network_setup_ssid());
        lv_label_set_text(s_setup_details, details);
        if (!tk_config_get()->configured) {
            lv_obj_add_flag(s_main_screen, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_setup_screen, LV_OBJ_FLAG_HIDDEN);
        }
    }
    _lock_release(&s_lvgl_lock);
}

void tk_display_set_online(bool online)
{
    tk_display_set_network_state(online ? TK_DISPLAY_ONLINE : TK_DISPLAY_OFFLINE);
}

void tk_display_set_ip(const char *ip)
{
    if (!s_ip_label || !ip) return;
    char text[32]; snprintf(text, sizeof(text), "IP: %s", ip);
    _lock_acquire(&s_lvgl_lock); lv_label_set_text(s_ip_label, text); _lock_release(&s_lvgl_lock);
}

void tk_display_refresh(void)
{
    if (!s_count_label) return;
    int employees, pending; tk_state_t *state = tk_state_lock(); employees = state->employee_count; pending = state->event_count; tk_state_unlock();
    char text[64]; snprintf(text, sizeof(text), "%d people - %d pending", employees, pending);
    _lock_acquire(&s_lvgl_lock); lv_label_set_text(s_count_label, text); refresh_employee_status_list(); _lock_release(&s_lvgl_lock);
}

void tk_display_show_setup(void)
{
    if (!s_setup_screen) return;
    _lock_acquire(&s_lvgl_lock);
    s_starting = false;
    char details[96]; snprintf(details, sizeof(details), "%s\nPassword: timekeep", tk_network_setup_ssid());
    lv_label_set_text(s_setup_details, details);
    lv_obj_add_flag(s_main_screen, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(s_setup_screen, LV_OBJ_FLAG_HIDDEN);
    _lock_release(&s_lvgl_lock);
}

void tk_display_show_startup(void)
{
    if (!s_boot_screen) return;
    _lock_acquire(&s_lvgl_lock); s_starting = true;
    lv_obj_add_flag(s_main_screen, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s_status_screen, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s_setup_screen, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s_settings_screen, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(s_boot_screen, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(startup_timeout_timer, 6000, NULL);
    _lock_release(&s_lvgl_lock);
}

void tk_display_show_ota(const char *version)
{
    if (!s_ota_screen) return;
    _lock_acquire(&s_lvgl_lock); s_ota_visible = true; s_starting = false;
    lv_obj_add_flag(s_main_screen, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s_status_screen, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s_setup_screen, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s_settings_screen, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s_boot_screen, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(s_ota_screen, LV_OBJ_FLAG_HIDDEN);
    if (version && version[0]) { char text[48]; snprintf(text, sizeof(text), "Preparing %s", version); lv_label_set_text(s_ota_label, text); }
    _lock_release(&s_lvgl_lock);
}

void tk_display_finish_ota(bool success)
{
    if (!s_ota_screen) return;
    _lock_acquire(&s_lvgl_lock); s_ota_visible = false; lv_obj_add_flag(s_ota_screen, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(s_main_screen, LV_OBJ_FLAG_HIDDEN); status_set_token(success ? "Update complete" : "Update failed - try again", success ? TK_TOKEN_GOOD : TK_TOKEN_ERROR); _lock_release(&s_lvgl_lock);
}

/*
 * A theme change must reach labels and controls that are intentionally local
 * to a screen (settings rows, setup cards, picker choices, and so on), not
 * merely the screen containers.  The UI is static after initialisation, so
 * remapping the existing palette colours in place preserves every object's
 * geometry, visibility, state and event bindings.  The four keypad colours
 * are deliberately absent from both token palettes and therefore untouched.
 */
static bool theme_remap_color(lv_color_t current, lv_color_t *replacement)
{
    for (int token = 0; token < TK_TOKEN_COUNT; ++token) {
        if (lv_color_eq(current, lv_color_hex(TK_COLOR_LIGHT[token])) ||
            lv_color_eq(current, lv_color_hex(TK_COLOR_DARK[token]))) {
            *replacement = tk_lv_color((tk_color_token_t)token);
            return true;
        }
    }
    return false;
}

static void remap_local_theme_color(lv_obj_t *obj, lv_style_prop_t prop,
                                    lv_style_selector_t selector)
{
    lv_style_value_t value;
    if (lv_obj_get_local_style_prop(obj, prop, &value, selector) != LV_STYLE_RES_FOUND)
        return;

    /* #22362A is both the light ink token and the fixed accent foreground.
     * Preserve it on an accent-filled parent and in explicitly pressed text. */
    lv_obj_t *parent = lv_obj_get_parent(obj);
    bool accent_text = parent &&
        lv_color_eq(lv_obj_get_style_bg_color(parent, LV_PART_MAIN),
                    tk_lv_color(TK_TOKEN_ACCENT));
    if (prop == LV_STYLE_TEXT_COLOR &&
        lv_color_eq(value.color, lv_color_hex(TK_ACCENT_FG)) &&
        (accent_text || selector != LV_PART_MAIN))
        return;

    lv_color_t replacement;
    if (theme_remap_color(value.color, &replacement)) {
        value.color = replacement;
        lv_obj_set_local_style_prop(obj, prop, value, selector);
    }
}

static void apply_theme_to_tree(lv_obj_t *obj)
{
    static const lv_style_selector_t selectors[] = {
        LV_PART_MAIN,
        LV_PART_MAIN | LV_STATE_PRESSED,
        LV_PART_MAIN | LV_STATE_CHECKED,
        LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED,
        LV_PART_INDICATOR,
        LV_PART_INDICATOR | LV_STATE_CHECKED,
        LV_PART_KNOB,
    };
    static const lv_style_prop_t color_props[] = {
        LV_STYLE_BG_COLOR,
        LV_STYLE_BORDER_COLOR,
        LV_STYLE_LINE_COLOR,
        LV_STYLE_ARC_COLOR,
        LV_STYLE_TEXT_COLOR,
    };
    for (size_t i = 0; i < sizeof(selectors) / sizeof(selectors[0]); ++i)
        for (size_t j = 0; j < sizeof(color_props) / sizeof(color_props[0]); ++j)
            remap_local_theme_color(obj, color_props[j], selectors[i]);

    uint32_t child_count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < child_count; ++i)
        apply_theme_to_tree(lv_obj_get_child(obj, i));
}

// Theme propagation runs from event callbacks inside lv_timer_handler, so it
// must remain lock-free.  It repaints all descendants without rebuilding UI.
static void apply_theme_styles(void)
{
    bool dark = dark_theme();
    lv_color_t bg = tk_lv_color(TK_TOKEN_BG);
    lv_color_t line = tk_lv_color(TK_TOKEN_LINE);
    lv_color_t ink = tk_lv_color(TK_TOKEN_INK);
    for (size_t i = 0; i < s_all_screen_count; ++i) {
        lv_obj_t *screen = s_all_screens[i];
        if (!screen) continue;
        apply_theme_to_tree(screen);
        lv_obj_set_style_bg_color(screen, bg, 0);
    }
    // Header chrome (every screen that ships with one).
    if (s_header) lv_obj_set_style_bg_color(s_header, bg, 0);
    if (s_settings_header) lv_obj_set_style_bg_color(s_settings_header, bg, 0);
    if (s_calibration_header) lv_obj_set_style_bg_color(s_calibration_header, bg, 0);
    if (s_picker_header) lv_obj_set_style_bg_color(s_picker_header, bg, 0);
    if (s_status_dot_halo_bg) lv_obj_set_style_bg_color(s_status_dot_halo_bg, bg, 0);
    if (s_status_dot_halo_line) lv_obj_set_style_bg_color(s_status_dot_halo_line, line, 0);
    if (s_brand_label) lv_obj_set_style_text_color(s_brand_label, ink, 0);
    if (s_pin_label) lv_obj_set_style_text_color(s_pin_label, ink, 0);
    if (s_count_label) lv_obj_set_style_text_color(s_count_label, tk_lv_color(TK_TOKEN_MUTED), 0);
    // Sequence dots — border takes line colour in both themes.
    for (int i = 0; i < 4; ++i) {
        if (s_sequence_dots[i]) {
            lv_obj_set_style_border_color(s_sequence_dots[i],
                tk_lv_color((size_t)i < strlen(s_pin) ? TK_TOKEN_INK : TK_TOKEN_MUTED), 0);
        }
    }
    // Status dot — the state colours are theme-tokenised, so a theme flip
    // re-runs the state machine to repaint the active shape. This also
    // cancels and restarts the state's animation with the new colours.
    status_dot_set_state(s_network_state);
    // Theme-segment highlight follows the new selection.
    if (s_theme_segments[0] && s_theme_segments[1]) {
        for (int i = 0; i < 2; ++i) {
            bool selected = i == (dark ? 1 : 0);
            lv_obj_t *seg = s_theme_segments[i];
            lv_obj_set_style_bg_color(seg, selected ? tk_lv_color(TK_TOKEN_ACCENT) :
                                      tk_lv_color(TK_TOKEN_SURFACE), 0);
            lv_obj_set_style_bg_color(seg, tk_lv_color(TK_TOKEN_ACCENT), LV_STATE_PRESSED);
            lv_obj_set_style_text_color(seg, selected ? lv_color_hex(TK_ACCENT_FG) : ink, 0);
            lv_obj_set_style_text_color(seg, lv_color_hex(TK_ACCENT_FG), LV_STATE_PRESSED);
        }
    }
    // Reduce-motion switch track follows the theme tokens.
    if (s_reduce_motion_switch) {
        lv_obj_set_style_bg_color(s_reduce_motion_switch, tk_lv_color(TK_TOKEN_MUTED), 0);
        lv_obj_set_style_bg_color(s_reduce_motion_switch, tk_lv_color(TK_TOKEN_GOOD), LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(s_reduce_motion_switch, tk_lv_color(TK_TOKEN_SURFACE), LV_PART_KNOB);
        lv_obj_set_style_bg_color(s_reduce_motion_switch, tk_lv_color(TK_TOKEN_SURFACE), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_reduce_motion_switch, tk_lv_color(TK_TOKEN_GOOD), LV_PART_INDICATOR | LV_STATE_CHECKED);
    }
    // Scrollbar styling is static (LV_PART_SCROLLBAR), no refresh needed.
    // Refresh picker labels so the right-hand value reflects the current theme.
    rebuild_settings_picker_labels();
    (void)dark;
}

void tk_display_apply_settings(void)
{
    if (!s_main_screen) return;
    _lock_acquire(&s_lvgl_lock);
    apply_theme_styles();
    if (s_server_label) lv_label_set_text_fmt(s_server_label, "Server: %s", tk_config_get()->server_url[0] ? tk_config_get()->server_url : "not configured");
    _lock_release(&s_lvgl_lock);
}

void tk_display_set_company_name(const char *name)
{
    if (!s_brand_label || !name || !name[0]) return;
    _lock_acquire(&s_lvgl_lock);
    lv_label_set_text(s_brand_label, name);
    _lock_release(&s_lvgl_lock);
}

bool tk_display_is_sleeping(void) { return s_screen_sleeping; }

void tk_display_submission_status(const char *text, uint32_t color)
{
    if (!s_status_label || !text) return;
    _lock_acquire(&s_lvgl_lock);
    set_status(text, color);
    _lock_release(&s_lvgl_lock);
}
