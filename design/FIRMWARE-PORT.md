# Terminal UI port

Reference: terminal-design-board.html, Option 1, 240 × 320 portrait.

The firmware uses the existing ST7789/XPT2046 drivers and pin mapping.
The repository identifies the hardware as an ESP32-2432S032; the mock's
2.8-inch caption does not change the board configuration.

## Implemented

- Fixed 2 × 2 A/B/C/D keypad, 104 × 84 tiles, 216 × 44 Clear control.
- Gear navigation, sequence feedback, five network-state shapes.
- Scrollable settings and full-page choices for both power timeouts and
  health/full-sync intervals.
- Light/dark palette propagation and persisted reduce-motion preference.
- Four-step calibration with one target visible and a cancel path.
- Local settings override portal values; Use web settings restores portal
  ownership on the next successful sync. Reduce motion stays device-local.
- Existing code submission, employee status, provisioning and OTA paths remain.

Submission happens when the fourth colour is entered. The 1400ms input lock
is feedback only; Clear cannot retract an already queued attendance request.
Unlike the browser demo, feedback comes from the actual queue/API result.

## Validation

Run from repository root:

    node --test design/firmware-port.test.mjs
    source /home/drb0rk/.espressif/v6.0.1/esp-idf/export.sh
    idf.py -C firmware -B build-ui-port build

The Node checks verify source-level geometry and wiring, not LVGL runtime
behaviour. The ESP-IDF build verifies compilation, linking and partition fit.
The legacy touch-coordinate API produces deprecation warnings.

## Hardware checks before release

- Confirm font legibility, gear visibility and all touch targets on the panel.
- Test repeated theme changes while settings are scrolled or a picker is open.
- Confirm timeout and motion preferences survive reboot, with Wi-Fi retained.
- Exercise offline queueing, retry, reconnect and sync success.
- Confirm calibration cancellation, four valid points and invalid-point rejection.
- Exercise sleep/wake, setup, employee-status navigation and OTA.
- Measure animation responsiveness and free heap on the ESP32.

The development build was USB-flashed on 2026-09-14; hashes verified, display
initialization completed, and saved Wi-Fi credentials reconnected. The first
server heartbeat timed out. Physical visual verification remains outstanding.
The v0.3.0 release rebuild has not been reflashed onto that device.
