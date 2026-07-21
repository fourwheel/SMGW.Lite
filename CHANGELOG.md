# Changelog

All notable changes to this project will be documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.2.4] - 2026-07-21

### Changed
- Modular split: extracted ~2440 lines from `main.cpp` into eight new modules:
  - `debug_log.h` — DLOG/DLOGLN/DLOGF macros
  - `serial_scan.h/.cpp` — baud/parity scanner state machine with accessor API
  - `webserver_optical.h/.cpp` — flash pulse handlers and PIN Assistant pages
  - `webserver_config.h/.cpp` — IotWebConf setup, route registration, WiFi setup handlers
  - `meter_value.h/.cpp` — ring-buffer read/write/init, slot counting, working copies (`LastMeterValue`, `PrevMeterValue`), feature flags
  - `webserver_telegram.h/.cpp` — telegram display and SML/IEC analysis handlers
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
