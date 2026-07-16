# Changelog

All notable changes to this project will be documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
