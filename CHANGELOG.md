# Changelog

All notable changes to this project will be documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.3.4] - 2026-09-05

### Fixed
- Remote FW update ring-log entries `6014` ("check triggered by backend hint") were also logged when the user manually clicked "Installieren" on the Remote FW Update page &ndash; `/installRemoteFw` set the same `g_ota_check_requested` flag that the genuine backend hint (parsed from the meter-value response) also sets, so `handle_remote_ota()` could not tell the two apart and always attributed the check to "backend hint". Introduced a separate `g_ota_manual_install_requested` flag and new log code `6022` ("check triggered by manual install confirmation") so the two trigger sources are distinguishable in the log again.

## [1.3.3] - 2026-08-08

### Changed
- Manual firmware update page (`/update`): file upload now goes through `XMLHttpRequest` with a live progress bar instead of a plain form POST, so large `.bin` uploads no longer look frozen
- After a successful flash the page shows the result inline, waits 15s, then polls `/sysinfo` every 2s (up to 15 attempts) and auto-redirects once the device is back online, instead of leaving the user without feedback after the reboot
- Status text now switches to "Warte auf Geräteneustart …" once the 15s wait begins, instead of leaving the upload-success message on screen until it jumps straight to the redirect message
- `POST /update` response is now sent, flushed, and given a short delay before `ESP.restart()` is called, so the client reliably receives the success/error status instead of the connection sometimes dropping mid-response
- WiFi setup (`/wifiSetup` "connecting" page): a wrong password or out-of-range SSID is now detected within seconds instead of only after a blind 40s poll timeout &ndash; `GET /wifiStatus` reports a definitive `failed:true` as soon as `WiFi.status()` returns `WL_CONNECT_FAILED`/`WL_NO_SSID_AVAIL`, or after a 20s cap if the status never resolves
- On detected failure the firmware calls `WiFi.disconnect()` to stop the STA radio from silently retrying in the background, so the AP stays responsive instead of appearing to hang for ~30s
- The failure screen now shows an actual "Erneut versuchen" button back to `/` instead of just static text with no way to retry
- `/wifiScan` results now include the network's channel; selecting a network and submitting `/wifiSetup` passes it along so `WiFi.begin()` can connect directly on that channel instead of scanning all channels &ndash; the multi-channel scan was hopping the shared AP+STA radio and briefly dropping the client's connection to the device's own AP while it searched for the target SSID (channel is cleared again if the SSID field is hand-edited, falling back to auto-scan)
- Failed connection-attempt detection (`WL_CONNECT_FAILED`/`WL_NO_SSID_AVAIL`/20s timeout) now runs unconditionally from the main loop (`handle_wifi_setup_watchdog()`) instead of only when the browser's `/wifiStatus` poll happens to reach the device &ndash; while the STA is retrying a failed handshake it can starve the AP's radio time badly enough that the very poll needed to detect and abort the attempt doesn't get through, previously letting a wrong password lock out the AP indefinitely
- Fixed a false-positive from that same watchdog: `WL_CONNECT_FAILED` can appear transiently for a moment during a handshake that still goes on to succeed, so the watchdog now requires the failure status to persist for 3s before acting, instead of aborting on the first bad reading (which was rejecting connections even with a correct password)
- WiFi credentials from `/wifiSetup` are now held in RAM only until the connection is confirmed to work, and only then written via `iotWebConf.saveConfig()` &ndash; previously they were saved to flash immediately, so a typo'd password permanently overwrote a previously working configuration
- `/wifiScan` network list is now sorted by signal strength (RSSI), strongest first, and shows a spinner next to "Suche nach Netzwerken…" while scanning
- Spinner on the `/wifiSetup` "connecting" page is now centered above the status text instead of sitting to the side of it (it's a block element, so the card's `text-align:center` didn't affect it)
- Home page WiFi setup card: "Verfügbare WLANs anzeigen" no longer uses the same blue as the meter-reading tile (was easy to miss as a button); it's now an amber/gold CTA matching the card's alert styling. "Verbinden" now uses the app's standard blue instead of a dark brown
- `/wifiScan` and `/wifiSetup`: the page is now sent to the browser *before* `WiFi.scanNetworks()` / `WiFi.begin()` is called, with an explicit `flush()`+`delay(300)` in between &ndash; starting that radio activity first was competing with delivery of the very response describing it, which could stall the page (`/wifiScan`) or reset the connection entirely (`ERR_CONNECTION_RESET` on `/wifiSetup`)
- `/wifiSetup` connecting page: each `/wifiStatus` poll now aborts after 5s (`AbortController`) instead of relying on the browser's own connect timeout, which can be tens of seconds and made a transient AP hiccup look like an indefinite hang
- `/wifiScan`: the SSID field in each network's connect form is now `readonly` &ndash; multiple forms on the page share `name="ssid"`, which could let the browser substitute a previously-submitted value (e.g. showing/submitting the old network name instead of the one just clicked)
- `/wifiStatus` now verifies `WiFi.SSID()` actually matches the requested network before reporting success &ndash; previously any `WL_CONNECTED` state was accepted, so if a new attempt failed and the STA fell back to a still-saved previous network, that was wrongly reported as success (showing the wrong network as connected) and the new, never-verified credentials were saved over the working old ones

### Added
- New ring-log codes for the WiFi setup flow: `7002` (connection attempt failed), `7003` (connected to previously-saved network instead of the requested SSID), `7004` (connection confirmed, credentials saved)
- New ring-log codes for the manual `/update` upload: `6101` (`Update.begin()` failed), `6102` (write error during upload), `6103` (`Update.end()` failed), `6104` (upload successful, rebooting)
- `/sysinfo`: WLAN-Netzwerke is now reachable via a permanent quick-link at the top of the page, regardless of connection state &ndash; previously the only way in was the home page's WiFi card, which is hidden once connected

### Changed
- `/sysinfo`: the top of the page now shows "Systemparameter", "WLAN-Netzwerke", "PIN Assistant" and "PIN Assistant Deluxe" as a 2&times;2 grid of quick-links, replacing the single "Konfigurationsseite" link and the PIN Assistant buttons previously buried in the "Helpers" card
- `/wifiSetup`: a candidate password held in RAM (`g_pendingWifiPassword`) is now cleared on every failure path (timeout, definitive connect failure, fallback-to-old-network), not just on confirmed success &ndash; previously it lingered in RAM until overwritten by the next attempt

## [1.3.2] - 2026-08-13

### Fixed
- Negative temperature readings (below 0 degC) were stored as huge near-`UINT32_MAX` values instead of negative ones. `MeterValue.temperature` was declared `uint32_t` even though sub-zero readings are negative, so assigning a negative `int` wrapped around. Changed the field to `int32_t` throughout the firmware (ring-buffer struct, `MeterValue_write`/`MeterValue_read`, local web UI table). The backend's binary parser also unpacked the `temp` field as unsigned; it now converts it to signed via two's complement. The on-wire byte layout is unchanged, so the backend fix is compatible with firmware already deployed in the field.

## [1.3.1] - 2026-08-05

### Added
- WiFi network scan page (`/wifiScan`): lists available networks with signal strength bars and encryption indicator; selecting a network expands an inline form with pre-filled SSID and password field that POSTs to `/wifiSetup`
- `/wifiScanResults` JSON endpoint for async scan polling (returns `{"state":"scanning"}` while running, then `{"state":"done","networks":[...]}`)
- Link to `/wifiScan` added to the AP-mode WiFi setup card on the home page
- Button "WLAN-Netzwerke" added to the Helpers section of the sysinfo page (`/sysinfo`)

## [1.3.0] - 2026-08-01

### Added
- Remote firmware update (OTA pull): `index.php` returns JSON (`received`, `inserted`, `rejected`, `ota_check`) and sets `ota_check: true` when a manifest file exists for the device ID; the device fetches and flashes the update only then (24 h fallback timer as safety net)
- `index.php` response changed from plain text to JSON; `Content-Type: application/json` and `Content-Length` headers added (prevents nginx chunked transfer encoding, which corrupted JSON parsing in the firmware)
- Server-side: `fwupdate/{ID}/manifest.json` (version, filename, sha256, size) and `fwupdate/{ID}/firmware.bin`
- SHA-256 integrity verification of the downloaded binary before flashing, streamed — no full binary buffered in RAM
- Post-OTA validation: after a successful flash the device sets an NVS flag before restarting; on the next boot it contacts the backend to confirm the new firmware works — if the backend is unreachable it rolls back to the previous OTA slot via `esp_ota_set_boot_partition()` and restarts
- 15-minute OTA cooldown after a rollback to prevent flash-rollback loops (log 6020)
- Log buffer uploaded to backend before OTA restart so no entries are lost on reboot
- New module `src/ota_pull.cpp` / `src/ota_pull.h`
- New log codes 6000–6021 for OTA pull lifecycle events
- "Check Remote FW Update" button now fetches the manifest and shows version info before asking for confirmation; no automatic install without user approval (log 6021)

## [1.2.6] - 2026-08-01

### Fixed
- `smlToWatt`: cast raw value to `int64_t` before scaling so negative sign-extended values (Netzeinspeisung) are divided correctly — previously unsigned arithmetic produced a garbage result, causing feed-in power to display as ~1284 kW instead of ~1284 W
- Log 1208 (new): meter rollback detected but value forwarded to backend — replaces the previous hard block (log 1207) so a corrupt high value in `PrevMeterValue` can no longer permanently lock out all subsequent valid telegrams; backend validates monotonicity via its existing `meter_rollback` rejection
- Telegram watchdog: suppress spurious 3005/3007 alerts when "Get Meter Value from other SMGWLite Client" (remote debug mode) is active — no serial telegram is expected in that mode
- Log 3005 text updated: "No valid telegram parsed for 5 min" (was: "No telegram received for 5 min") — clarifies that serial bytes may be arriving but parsing fails
- Log 3007 (new): "No serial data received for 5 min" — fires when the serial interface receives no bytes at all (optical reader likely disconnected); 3005 is suppressed in this case to avoid double-alerting

## [1.2.5] - 2026-07-29

### Changed
- Switch partition scheme to `min_spiffs.csv` on all boards (OTA slot: 1.25 MB → 1.9 MB, SPIFFS: ~1.5 MB → 64 KB); requires one-time serial flash to update partition table
- Exclude auto-generated `src/build_info.h` from version control
- Log 8001: renamed to "No custom cert, using bundled ISRG Root X1" (was: "Error reading cert file")
- Log 1004: renamed to "Feature config applied — buffer reinitialised" (was: "Buffer layout changed, re-initialising")

## [1.2.4] - 2026-07-21

### Changed
- Modular split: extracted ~2440 lines from `main.cpp` into eight new modules:
  - `debug_log.h` — DLOG/DLOGLN/DLOGF macros
  - `serial_scan.h/.cpp` — baud/parity scanner state machine with accessor API
  - `webserver_optical.h/.cpp` — flash pulse handlers and PIN Assistant pages
  - `webserver_main.h/.cpp` — IotWebConf setup, route registration, WiFi setup, system pages (root, sysinfo, OTA, certs, backend test)
  - `meter_value.h/.cpp` — ring-buffer read/write/init, slot counting, working copies (`LastMeterValue`, `PrevMeterValue`), feature flags
  - `webserver_data.h/.cpp` — telegram display, SML/IEC analysis, meter value table, log buffer display
- `app_globals.h` provides `extern` declarations for all cross-module globals
- Removed all ESP8266 dead code (`#if defined(ESP8266)` blocks, `SoftwareSerial`, `ESP8266WiFi.h` / `ESP8266mDNS.h` / `ESP8266HTTPClient.h`) — only ESP32 is supported
- `main.cpp` reduced from ~4828 lines to ~2390 lines (−50%)

## [1.2.3] - 2026-07-18

### Changed
- Dashboard status cards (PIN/INF, Backend/WiFi) are now live-updated every 2 s via the existing `/showLastMeterValue` poll — no page reload needed when connection state changes
- `/showLastMeterValue` JSON response extended with `wifi_connected`, `backend_called`, `backend_ok`, `backend_ago_min`
- AP-mode WiFi setup card auto-disappears (page reload triggered) when WiFi reconnects while the dashboard is open
- `testBackendConnection` page now loads immediately and fetches results asynchronously via `/testBackendConnectionRun` — no blank loading screen while waiting for the network test

### Fixed
- Dashboard no longer shows "Backend nicht erreichbar" on boot before any call was attempted; shows "Backend noch nicht kontaktiert" until `last_call_backend > 0`
- First backend call now fires immediately after NTP sync on boot (no longer waits for the next minute boundary)
- Backend failure retry interval increased from 30 s to 3 minutes — a misconfigured or unreachable endpoint previously caused a blocking connect attempt every 30 s, making the web UI sluggish
- "Backend-Fehler" status card now shows a "Verbindung testen" button (`.btn` style, consistent with PIN Assistant buttons) linking to `/testBackendConnection`

## [1.2.2] - 2026-07-16

### Fixed
- Extend IotWebConf WiFi connection timeout from 30s to 90s to give hidden SSIDs enough time to associate
- Remove redundant `WiFi.begin()` call in reconnect handler that was interrupting IotWebConf's own association cycle, causing the webserver to remain unresponsive during prolonged reconnects

## [1.2.1] - 2026-07-14

### Fixed
- Block new backend calls during OTA update (`ota_active` flag) to prevent TCP stack conflicts that caused intermittent OTA failures

## [1.2.0] - 2026-07-14

### Added
- `FIRMWARE_VERSION` define in `main.cpp` next to `CONFIG_VERSION` for centralized version management
- Sysinfo page: "Firmware Version" and "Config Version" rows in System Info card
- Firmware version (`fw`) and config schema version (`cfg`) transmitted to backend in log call
- Backend: `fw_version VARCHAR(20)` and `cfg_version VARCHAR(10)` columns in `clients` table (migration SQL in `database_setup.md`)
- Backend: `config.php` split into `config.php` (functions, now git-tracked) and `credentials.php` (DB credentials, gitignored); `credentials.php.TEMPLATE` added as setup guide

### Changed
- Backend: `update_client_endpoint()` removed from `authenticate()` — each endpoint calls it directly with its own params, eliminating the redundant double-UPDATE on data calls
- Backend: `update_client_endpoint()` refactored to dynamic SET clause — no optional column is ever accidentally overwritten with NULL

### Fixed
- Fix SML scaler: normalize raw OBIS values to 0.1 Wh storage unit
- Fix silent data loss when obis280 is NULL in TAF0/TAF1 conversion
- Refactor SML extraction: split obisExtractor into raw extraction and unit normalization

## [1.1.0] - initial tagged release

### Added
- TAF7 high-priority snapshots, TAF14 interval readings, dynamic TAF (power delta / ratio)
- MyStrom solar PV integration
- DS18B20 temperature sensor support
- Binary packed ring-buffer with self-describing `fields=` wire format
- HTTPS backend with optional ISRG Root X1 certificate verification
- IotWebConf-based web configuration UI
- Log buffer with periodic POST to `/log.php`
- NTP time synchronization
- OTA firmware update support
