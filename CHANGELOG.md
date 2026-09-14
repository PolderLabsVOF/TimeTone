# Changelog

All notable TimeTone releases are documented here. Versions follow
[Semantic Versioning](https://semver.org/): `MAJOR.MINOR.PATCH`.

## [0.3.6-dev] - 2026-09-14

- Add a custom accessible date picker for manual time entries and dashboard filters.
- Add a full 24-hour quarter-hour time picker for manual time entries.
- Anchor the edit entry popover to its trigger, clamp it to the viewport, and make it scrollable.
- Fix terminal stability for connected event uploads, reduce display flicker, and add safety for persisted state.
- Explain the terminal's five connection status icons on the Devices page.
- Start native installations automatically after reboot through a systemd user service, and enable the Docker daemon for Docker installs.
- Use a system-level service for root-run native installs, including Debian LXC containers without a user D-Bus session.
- Prevent the terminal's idle HTTPS warm-up from exhausting heap before its configuration sync, and allow the first interactive HTTPS request enough time for DNS/TLS setup.
- Add a `--dev` installer path that fetches a selected `dev` commit and builds the dashboard locally without publishing a release.
- Package Next.js static and public assets into the native dev runtime so the dashboard is styled after a dev install.
- Let users clear an entry's end time in the edit and manual-entry forms to save it as an open shift.
- Reset the manual-entry form after a successful save and show a confirmation message.
- Restructure the Devices page into fleet summary, terminal cards, onboarding, and status guidance sections.
- Restructure Settings into workspace summaries, grouped controls, a settings map, and separate security/migration/update areas.
- Replace Settings anchor navigation with real keyboard-accessible tabbed sections; workspace fields remain associated with one shared save action across tabs.
- Keep inactive Settings panels mounted so saving from Reports submits the complete workspace settings form.
- Fix Settings tab strip sizing so labels and active/focus states are not clipped on narrow or wide layouts.
- Overhaul the Who's here canvas with distributed node placement, independent drift, spacing forces, drag momentum, and keyboard movement.
- Tone down the Who's here canvas with softer motion, quieter connections, smaller translucent nodes, and a calmer background.

## [0.3.3] - 2026-09-14

- Point the installer, updater, and dashboard links at the canonical `PolderLabsVOF/TimeTone` repository instead of relying on GitHub's redirect from the old location.
- Build and test the `dev` integration branch alongside `beta` and `main`.
- Support pre-release tags in the release pipeline, so release candidates publish as GitHub pre-releases and stay out of the installer's stable version resolution.
- Document the branch model, contribution guidelines, and versioning standards.

## [0.3.2] - 2026-09-14

- Add a release preview before installation or update work begins, including the exact stable version selected from GitHub.
- Add a colored TimeTone ASCII installer mark, stage bars, and dependency-free download animation while keeping redirected logs readable.
- Render the branded installer mark before the update handoff, so the header appears ahead of any download rather than only after it completes.
- Preserve failed download exit codes so the installer stops instead of continuing after an unsuccessful release fetch.

## [0.3.1] - 2026-09-14

- Restore scheduled terminal heartbeats and configuration refreshes so a stable terminal receives device settings, sync requests, OTA availability, and queued-event retries without needing a reconnect or wake.
- Keep dashboard sync requests pending until the authenticated terminal has successfully fetched configuration.
- Preserve locally queued events unless the server individually acknowledges them, and make OTA startup failures retryable.
- Synchronize supported terminal timezones and device theme settings, including a dashboard theme selector.
- Add batch manual entries, quarter-hour time selection, and a rounded Now action.
- Add confirmed device removal while retaining recorded time entries.

## [0.3.0] - 2026-09-14

- Port the approved portrait terminal design with touch-sized controls, custom settings pickers, connection-state indicators, light/dark themes and reduced motion.
- Persist local terminal preferences with an option to resume web-managed settings.
- Harden terminal reconnection and updater progress reporting.
- Distribute the standalone web runtime and firmware as prebuilt release assets.
- Make native Settings updates install the standalone runtime without compiling on the user's machine.

## [0.2.32] - 2026-09-04

- Make terminal wake-up resilient after long idle periods: restore full Wi-Fi radio power, verify the current access-point association, and trigger the normal reconnect flow when it has expired.
- Drop idle retained HTTPS connections on wake and establish a fresh connection before the next colour code, avoiding a first-entry timeout without reintroducing periodic server syncs.

## [0.2.31] - 2026-09-03

- Calculate report averages from days with actual attendance only, using merged office-occupation intervals so overlapping employee sessions are counted once.

## [0.2.30] - 2026-09-03

- Restore reliable terminal wake-on-touch by polling the XPT2046 controller while the display is off instead of depending on its low-power IRQ level.
- Warm the retained interactive server connection after low-power wake without restoring periodic settings syncs.

## [0.2.29] - 2026-09-03

- Delay the in-place updater handoff until the install API response has passed through the reverse proxy, preventing a false Cloudflare 502 when the old server stops.
- Show a useful recovery message if a proxy interrupts an update request.

## [0.2.28] - 2026-09-03

- Make manual time-entry updates transactional and preserve open entries without applying automatic merge or close rules afterward.
- Show actionable validation errors in the entries page instead of failing the server action.

## [0.2.27] - 2026-09-03

- Re-trigger the initial server setup after Wi-Fi receives an IP address instead of leaving an event-driven terminal waiting indefinitely.
- Keep the keypad available after a short startup animation while a slow network or TLS connection completes in the background.

## [0.2.26] - 2026-09-02

- Make terminal server traffic event-driven: no periodic health checks or full configuration downloads after setup; code entries contact the server immediately, and settings refresh only at boot or when manually requested.
- Wake the api task the moment the station acquires an IP so terminals that boot before Wi-Fi is ready do not get stuck on the start-up screen waiting on a one-shot sync that already fired.

## [0.2.25] - 2026-09-02

- Restore one-shot background API connections after a stale reverse-proxy keep-alive socket could leave a terminal stuck retrying.

## [0.2.24] - 2026-09-02

- Reuse retained HTTPS clients for the terminal's health, settings, and event syncs instead of opening a new TLS connection for every request.
- Warm the clock connection before delivering a queued code after startup or wake, reducing the first-entry delay.

## [0.2.23] - 2026-09-02

- Fix a terminal restart when opening the team-status page by keeping its employee snapshot out of the LVGL task stack.

## [0.2.22] - 2026-09-02

- Add a swipe-right employee status page to the terminal, with cached IN/OUT state, large coloured presence dots, scrolling, and a swipe-left return gesture.

## [0.2.21] - 2026-09-02

- Store pending code submissions in a separate compact NVS record instead of expanding the cached employee state.
- Safely migrate state blobs across size changes without discarding cached employees.

## [0.2.20] - 2026-09-02

- Persist colour-code submissions before network delivery, provide immediate queue feedback, and retry safely after failures or restarts.
- Deduplicate retried code submissions on the server with terminal-generated request IDs.
- Re-warm the authenticated clock connection after terminal wake.

## [0.2.19] - 2026-09-02

- Keep a dedicated HTTPS connection warm for terminal clock submissions, avoiding a TLS handshake for each person.
- Add an authenticated, side-effect-free clock-route warmup response.

## [0.2.18] - 2026-09-02

- Remove full historical maintenance scans from the terminal's interactive clock endpoint.
- Prioritize code submissions over background health traffic and bound clock-request failures to 4.5 seconds.
- Add terminal-side timing diagnostics for clock requests slower than one second.

## [0.2.17] - 2026-09-02

- Remove the non-interactive colour-code dot field and expand the terminal keypad.
- Show concise selection progress in the existing status line instead.

## [0.2.16] - 2026-09-02

- Prevent company names and the clock from colliding in the terminal header.
- Reclaim the clipped bottom footer for a larger, easier-to-tap clear action.

## [0.2.15] - 2026-09-02

- Modernize the terminal home, startup, offline setup, settings, and local browser-configuration experiences.
- Introduce a cohesive card system, refined status hierarchy, and better touch-target spacing for the CYD portrait display.

## [0.2.14] - 2026-09-02

- Replace the custom-drawn terminal settings cog with LVGL's bundled Font Awesome settings icon.

## [0.2.13] - 2026-09-02

- Reorganize the portrait terminal settings screen to prevent overlapping controls.
- Add a dedicated connection card and compact action layout.
- Replace the settings glyph with a custom-drawn cog that does not depend on icon fonts.
- Fix calibration instructions to render on the visible calibration screen.

## [0.2.12] - 2026-09-02

- Overhaul terminal connection recovery, startup authentication, and full-sync scheduling.
- Keep Wi-Fi reconnection active while the fallback configuration portal is available.
- Prevent routine health checks and successful clock-ins from triggering disruptive full syncs.
- Add bounded HTTPS timeouts, HTTP keep-alive, device request identification, and retry throttling.
- Force dynamic, no-cache device API responses.

## [0.2.11] - 2026-09-02

- Harden dashboard software updates with install-root discovery and actionable API errors.
- Prevent update requests from surfacing as opaque proxy errors.

## [0.2.10] - 2026-09-02

- Remove browser USB firmware flashing from the dashboard.
- Manage terminal firmware updates from the Devices page through explicit OTA requests.
- Keep firmware downloads and installation terminal-controlled after an administrator starts an update.

## [0.2.9] - 2026-09-02

- Fix native in-place updates to replace the compiled web bundle instead of serving stale UI assets.

## [0.2.8] - 2026-09-02

- Speed up terminal startup by making health checks independent from full configuration downloads.
- Trigger a full employee/settings refresh after wake or with the new Sync now controls.
- Improve Wi-Fi fast-scan and connection stability settings.
- Add a clearer terminal settings layout and manual sync action.
- Add a visible sidebar version and GitHub feedback link.
- Publish a prebuilt production web bundle for native installs.

## [0.2.7] - 2026-09-02

- Complete a dashboard UI/UX consistency pass across light and dark themes.
- Fix public asset routing so the TimeTone logo loads before authentication.
- Improve overview copy for singular attendance and add a reproducible demo fixture.
- Add polished README screenshots for the overview, reports, employees, and devices pages.

## [0.2.6] - 2026-09-02

- Confirm Docker update completion after the old container is replaced.

## [0.2.5] - 2026-09-02

- Fix installed-version detection in containerized deployments.
- Make update status resilient across the server restart.

## [0.2.4] - 2026-09-02

- Make software updates installable from the dashboard for native and Docker deployments.
- Add persistent update status, progress stages, and restart feedback.
- Add separate health-check and full-settings sync controls.

## [0.2.3] - 2026-09-02

- Improve terminal connection status visibility with a persistent colored dot.
- Apply local display, power, theme, and server settings without unnecessary restarts.
- Add reliable save result handling and asynchronous color-code submission.

## [0.2.2] - 2026-09-02

- Add animated OTA update progress screen on terminals.

## [0.2.1] - 2026-09-02

- Add animated connecting and syncing state feedback on terminals.
- Keep offline, connecting, syncing, and online states distinct during retries.

## [0.2.0] - 2026-09-02

- Add secure HTTPS OTA firmware updates from approved GitHub release assets.
- Add per-terminal update prompts and update requests in the Devices dashboard.

## [0.1.0] - 2026-08-31

Initial public release.

- ESP32-2432S032 CYD terminal with LVGL touchscreen and four-colour keypad
- Offline event queueing with automatic synchronization
- Employee, device, attendance, audit-event, report, and manual time-entry management
- Configurable rounding, timezone, power, screen, and terminal settings
- Live dashboard updates, dark mode, exports, and USB firmware updates
- Complete workspace migration export/import
