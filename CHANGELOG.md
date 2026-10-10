# Changelog

All notable changes to this project will be documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.4.5] - 2026-10-10

### Code structure
- `main.cpp` split into modules, code moved unchanged (no behaviour change): `webserver_pages.cpp` (`/`, `/sysinfo`).

### Removed
- Dynamic TAF (`handle_dynTaf()`, disabled behind `#if 0`) and its three `tafdyn_*` parameters. They were never registered with IotWebConf, so the stored config layout is unchanged (`CONFIG_VERSION` stays `2906`). Log code `1018` keeps its text for old logs.
- The parked TAF7 timestamp flooring (`#if 0` in `MeterValue_store()`, since 1.3.5). Stored values keep the real telegram time, as before.
- Duplicate `CONFIG_PIN` / `STATUS_PIN` / `LED_BUILTIN` defines in `main.cpp`; they are defined once in `app_globals.h` (same values).
- Unused libraries `NTPClient`, `ESPAsyncWebServer` and `AsyncTCP` from `lib_deps` (time sync uses `configTime()`, the web server is the synchronous `WebServer`), and the `NTPClient.h` / `WiFiUdp.h` includes.

### Changed
- The log upload no longer sends `token=header` in the URL. It was a switch from the move of the token into the `X-Auth-Token` header (March 2026); the backend reads only the header since then.
- `telegramTask` stack raised from 2048 to 3072 bytes. On an esp32-nodemcu parsing SML telegrams only ~700 bytes were left (Xtensa needs more stack than the RISC-V ESP32-C3 for the same code). Costs 1 KB heap; the lowest free heap seen on an ESP32-C3 in the field was ~104 KB.

### Fixed
- "Get Meter Value from other SMGWLite Client" (debug mode) now also takes over the 2.8.0 value of the other device; before, it was always stored as `0`.
- The meter model (`meter_model`, sent as `model=` with the log upload) was written by two tasks: the parsers in `telegramTask` and the telegram pages of the web server. Only the parsers set it now, and they build it locally and assign it once, so other tasks never read a string that is being built.
- On the dual-core ESP32 (esp32-nodemcu) a stored value or `/showLastMeterValue` could mix fields of two consecutive telegrams (e.g. the new 1.8.0 with the previous timestamp): the parsers replace `LastMeterValue` with a struct copy that is not atomic across cores. Parsers and the remote debug mode now write it, and the store and `/showLastMeterValue` copy it, under a spinlock (`MeterValue_setLast()` / `MeterValue_getLast()`). The ESP32-C3 has one core and was not affected.
- The web handlers that restart the meter UART (`/setSerialConfig`, `/flash`, `/flashlong`) called `mySerial.end()`/`begin()` while `telegramTask` might be reading from it. A new mutex `Sema_Serial` is held by `telegramTask` while it reads (or runs the serial scan) and by these handlers while the UART is off, so the driver is never removed mid-read. Tested on an esp32-nodemcu with a meter attached: 30 `/setSerialConfig` calls in a row, no restart, telegrams continued.
- The WiFi setup page (`POST /wifiSetup`) inserted the selected SSID into the HTML unescaped. A network in range named e.g. `<svg onload=fetch('/restart')>` ran its script in the browser once a user picked it. The SSID is now HTML-escaped there (the scan list already escaped it).
- Backend: `credentials.php.TEMPLATE` reported a failed database connection with `mysqli_error()`, which needs a connection and is a fatal error on PHP 8.0; now `mysqli_connect_error()`. From PHP 8.1 on `mysqli_connect()` throws before that, so existing `credentials.php` files need no change.

## [1.4.4] - 2026-10-08

### Changed
- `1200` (store refused: meter value `<= 0`) is logged at most every 30 min, like `3005`/`3007`. Without a 1.8.0 value the store is retried with every trigger, so a device without a valid meter value (e.g. no meter connected) logged it several times a minute: the log ring filled with it and, since 1.4.2 uploads the log after every non-routine entry, the log went out with every backend call. It still reaches the backend; a valid value restarts the 30-min cycle, so a new occurrence is logged right away.
- Backend: `fw_fetch.php` reports why writing a release failed (e.g. `cannot write smartmeterlite_v1.4.3_esp32c3.bin.tmp: Failed to open stream: Permission denied`) instead of only `writing v1.4.3/esp32c3 failed`.

## [1.4.3] - 2026-10-08

### Added
- Backend: `fw_fetch.php?version=<version>&key=<key>` fetches a release from GitHub into `fw_releases/` &ndash; the manifests from the tag `v<version>`, the binaries from the release &ndash; and writes the binary and `manifest.json` per build target only if size and SHA-256 match. Nothing is fetched until the script is called; it is disabled unless `credentials.php` sets `$_fw_fetch_key` (new in `credentials.php.TEMPLATE`). An existing, different release is only replaced with `&replace=1`. See `backend/FW_UPDATE.md`.

### Fixed
- A firmware pulled from the backend that crashed or hung before its post-update validation was never rolled back: the Arduino core confirmed every new image before `setup()`, and the validation only ran after the first WiFi connect, so such a firmware ended in a restart loop that needed a serial flash. The image now stays unconfirmed (`PENDING_VERIFY`, `verifyRollbackLater()`) until the validation succeeds; if the device restarts before that (crash, watchdog, self-recovery restart, power loss), the bootloader boots the previous firmware, which logs `6028`. Images uploaded via espota or `/update` are confirmed at boot as before. Tested on an ESP32-C3 with a build that aborts in `setup()`: one crash, then the previous firmware ran again.
- A single failed validation attempt rolled back a working firmware and blocked its version for good, e.g. if the router or the backend was briefly unavailable right after the restart. The validation is now retried once per minute (`6027`) and the firmware is only rolled back (`6011`) if it hasn't succeeded within 15 min after boot &ndash; also if WiFi never connects in that time. Meter value and log uploads keep running meanwhile; no other update is started until the validation is done (`6029`), and the "Check Remote FW Update" page says so instead of offering an install.
- The version of a pulled update is now written to `bad_ver` before the restart and removed once it is validated, so a version rolled back by the bootloader is blocked as well (`6025` when it is offered again); a higher version is installed normally. No new NVS keys. An older blocked version is replaced by this and may be installed automatically once more. This is done by the firmware that installs the update, so for the update from 1.4.2 or older to 1.4.3 the rollback works, but 1.4.3 is not blocked afterwards and would be installed again.
- The NVS flags for the validation are now written right after flashing, before the log upload; a restart during that upload booted the new firmware without validation.

## [1.4.2] - 2026-10-04

### Changed
- The log is now uploaded whenever a non-routine entry was logged. `Log_AddEntry()` is replaced by two functions: `Log_Add()`, the normal call, writes the entry and marks the log for upload (`b_send_log_to_backend`); `Log_AddWithoutTransmit()` only writes it and is used at the call sites of routine entries that occur in every cycle (backend call, TAF triggers, store, log upload, time sync, protocol detection, OTA check without result, value count). The log then goes out with the next backend call, i.e. within one interval (usually 2 min). Before, the log was only uploaded after a WiFi (re)connect, a failed log upload, OTA events or `3005`/`3007` &ndash; with the ring buffer covering only ~1 h of normal operation, events like a full meter value buffer, backend failures (`4000`&ndash;`4005`), meter rollbacks, OTA download errors or myStrom errors were usually overwritten before they reached the backend. New log calls are reported by default, since `Log_Add()` is the normal call.
- Regular log uploads only carry the entries written since the last acknowledged upload (oldest first) instead of the whole 200-entry ring, so a device with a persistent error sends a few entries per backend call instead of 2.4 KB. The ring on the device is not cleared &ndash; `/showLogBuffer` and `/sysinfo` still show all entries; only a counter of the acknowledged position is kept (RAM, reset on reboot like the ring). `/sendLog_Task` and `/sendboth_Task` still upload the whole ring. `log.php` needs no change (it restores the order via the lowest uptime, which keeps a chronological subset in order; duplicates are ignored by the unique key).
- `3005`/`3007` (meter silent for 5 min) no longer have their own immediate log upload (`b_send_log_urgent`); like any non-routine code they go out with the next backend call. They are still logged every 30 min while the meter stays silent.
- The number of values to transmit is logged as `100000` + n instead of n: with more than 999 values in the buffer (e.g. after ~16 h without backend) the bare number collided with status codes (1024 showed as "Boot snapshot triggered"). Dashboards understand both ranges.

### Fixed
- A log entry written while a log upload was running could be dropped from the next upload: the upload cleared the pending flag again after success. It is now only cleared right before the buffer is copied &ndash; and no longer when the upload task is skipped because of an OTA update.
- A failed allocation of the log upload buffer is logged (`1002`), so the log stays marked for upload.

## [1.4.1] - 2026-10-04

### Added
- Self-recovery restart: a new supervisor task (priority above `loop()`, so a busy-looping `loop()` can't starve it on the single-core ESP32-C3) restarts the device if `loop()` has not started a new pass for 10 min (`1120`) or a meter value / log upload task has held `Sema_Backend` for 10 min (`1121`/`1122`; timed from taking the semaphore, so waiting for another holder doesn't count). A hung upload task holds `Sema_Backend`, so the device would never call the backend again. All regular blocking is bounded well below 10 min (worst case ~7 min for an OTA pull, ~4 min for a meter upload on a nearly dead link), see the comment at `supervisorTask()`. K12 went silent like this after a normal, acknowledged call without restarting (cause unknown); such a device now recovers by itself. The cause is kept in NVS across the restart and logged on boot (not in RTC_NOINIT memory, see Fixed).
- The reset reason is logged at every boot as `1100` + `esp_reset_reason()` (e.g. `1101` power on, `1104` panic, `1106` task watchdog, `1109` brownout), so it reaches the backend with the first log upload instead of only being shown on `/sysinfo`.
- If no backend call has been acknowledged for 30 min while WiFi reports connected, WiFi is disconnected and IotWebConf reconnects (log `7000`, repeated while the backend stays unreachable). With a backend interval of 13 min or more the limit is two intervals + 5 min instead, so a regular gap between calls never triggers it. Unlike a restart this keeps the RAM buffer, so a backend outage costs no data.

### Fixed
- `index.php` rejects entries with a timestamp more than 10 min in the future (`future_timestamp`). A device with a wrong clock (e.g. garbage system time after a software reset) would otherwise store an entry far in the future, which then becomes the reference for all later entries &ndash; every correct value afterwards was rejected as `older_than_db_prev`. Seen during development of this version: a first draft kept the restart cause in two `RTC_NOINIT_ATTR` variables, which shifted the IDF's own RTC_NOINIT variables that carry the system time across a software reset (`s_esp_rtc_time_us`, `s_rtc_last_ticks`). After an OTA update to a firmware with a different RTC_NOINIT layout the clock read 2055 and the boot snapshot was uploaded with that timestamp. The restart cause is therefore kept in NVS; firmware must not add `RTC_NOINIT_ATTR` variables.
- myStrom fetch blocked `loop()` for up to ~17 min if the myStrom was unreachable or didn't answer: `WiFiClient::setTimeout()` takes seconds, so `setTimeout(1000)` meant 1000 s for the connect and every read. Now 3 s (the debug fetch from another device: 2 s).
- TLS handshakes could take up to 120 s (framework default) for the meter upload, the OTA pull and the connection test page; now 30 s, 30 s and 10 s (the log upload already used 10 s).
- The OTA download had no overall limit &ndash; its 30 s timeout restarts with every received byte, so a trickling download could block `loop()` indefinitely. It is now aborted after 4 min (`6026`, retried after 1 h like any failed download).

## [1.4.0] - 2026-10-02

### Added
- Central firmware assignment: the backend reads the version each device should run from `fw_targets.php` (a `default` for all devices plus per-device entries, `null` exempts a device) next to `config.php`, outside the web root. Every meter value call now reports `fw` and `hw` (build target `esp32c3` / `esp32-nodemcu`, from the IDF target); if the assigned version differs &ndash; newer or older &ndash; the response contains `fw_update` (version, sha256, size) from `fw_releases/v<version>/<hw>/manifest.json`. The device downloads the binary from the new authenticated `fw_download.php` (X-Auth-Token) and flashes it as before (SHA-256 check, post-update validation, rollback). See `backend/FW_UPDATE.md`.
- A version that fails its post-update validation and is rolled back is stored in NVS (`ota`/`bad_ver`) and never installed automatically again (new log code `6025`), so the device can't loop flash &rarr; rollback. It can still be installed from the "Check Remote FW Update" page, which now shows the version assigned in the backend. A failed download is retried after 1 h at the earliest.
- New log codes `4004`/`4005` (HTTP 200 without valid acknowledgement), `6023`/`6024` (download connection failed / non-200).

### Changed
- Uploads only count as successful if the backend acknowledges exactly the sent payload: `index.php` and `log.php` answer with `bytes` and `crc32` (PHP `hash('crc32b')`) of the received body, and the device compares both with what it sent. Before, any HTTP 200 cleared the meter value buffer &ndash; a misconfigured server answering 200 without running the backend made devices drop their data. The response body is now read up to `Content-Length` instead of line by line, and the buffer is cleared only after the body was checked.
- `index.php` stores each batch in one transaction; on a database error it rolls back and answers 500 without acknowledgement, so the device resends. `log.php` also answers 500 if an insert fails. Before, database errors were ignored and still answered with 200.
- `backend_test` answers `{"ok":true,"id":...}` (plus `fw_update`); the "Test Backend Connection" page and the post-update validation require this body instead of just HTTP 200 (the connection test even accepted any response containing "200").
- The backend is now called in every interval even without stored values (empty payload), so a device that receives no telegrams still gets firmware updates and shows up in `clients.last_reading`.

### Removed
- Per-device manifest `fwupdate/<ID>/manifest.json`, the `ota_check` hint and the 24 h fallback check are no longer used by the firmware. The backend keeps serving them for devices up to 1.3.12 (log codes `6001`, `6012`, `6015` only occur there).

## [1.3.12] - 2026-10-01

### Fixed
- Since 1.3.10 there was no TAF7 value at the mark: TAF14 runs on its own seconds interval and drifts to a fixed second (e.g. `:55`), so the "TAF7 fulfilled by TAF14" rule (`1030`, &plusmn;10 s) always applied and the only reading near the mark was the TAF14 one 5 s before it. TAF7 now always stores its own value at the TAF7 mark again, also when the counters haven't changed since the last stored value (only a frozen reading &ndash; no new telegram since &ndash; is still refused and its trigger dropped). Log code `1030` is gone.
- A TAF14 reading stored less than 10 s before the TAF7 value at the mark (by telegram timestamp) is removed again (log `1025`), so the TAF7 mark isn't preceded by a near-duplicate. Unlike the removal logic dropped in 1.3.10, it runs only after the TAF7 store succeeded &ndash; at most once per TAF7 mark, never on a retry &ndash; and only removes the slot if it still holds exactly that TAF14 entry (not after an upload cleared the buffer). It only applies to TAF7 mark stores, not to the boot snapshot or a manual TAF6 store.
- The first value after a reboot (boot snapshot) still had temperature `0` despite the 1.3.11 fix: the parser carries the previous `LastMeterValue.temperature` (still 0) into the first reading and only sets the sensor value after the first-telegram diagnostics, while `loop()` on the other core already stores the boot snapshot as soon as `startup_print_done` is set. `setup()` now also seeds `LastMeterValue.temperature` with the initial sensor reading.

## [1.3.11] - 2026-10-01

### Fixed
- After a reboot the first meter values were stored with temperature `0`: `current_temperature` started at 0 and the DS18B20 was only read later from `loop()`, while telegrams were already being parsed. The sensor is now read once (blocking, ~750 ms) in `setup()` when the temperature sensor is enabled. This also avoids the sensor's 85&nbsp;&deg;C power-on value, which the first read in `handle_temperature()` could return because it happened without a prior `requestTemperatures()`.

## [1.3.10] - 2026-09-28

### Fixed
- Data gaps of up to ~10 minutes around TAF7 marks: the "remove recent non-override entry" logic re-enabled in 1.3.5 removed one more TAF14 entry on every retry of a TAF7 store. When a TAF14 reading with the same counter value had just been stored (low consumption), the TAF7 store was refused by the 1-min cooldown (`1201`) and retried every second, and each retry cleared the next older TAF14 slot (`meter_value_NON_override_i++`) &ndash; observed as 9 consecutive `1025`/`1201` pairs wiping the previous 9 TAF14 values.
- A TAF14 reading taken exactly at the TAF7 mark was deleted and replaced by a TAF7 reading 2&ndash;3 s later. The removal logic is gone entirely (`MeterValue_ClearSlot()` removed). Instead TAF7 is considered fulfilled and stores nothing (new log code `1030`) when the last stored value is a TAF14 reading whose telegram timestamp is within &plusmn;10 s of the TAF7 mark. Otherwise TAF7 stores its own value immediately &ndash; unless the counters (1.8.0, 2.8.0, solar, with myStrom fetched fresh) haven't changed since the last stored value, see next entry.
- TAF7 values could land up to a minute after their TAF7 mark, and the same value could be stored twice with different timestamps: override stores (TAF7, boot snapshot, manual TAF6) with an unchanged value were refused for 1 min, retried every second and then stored late as a duplicate. An override store with an unchanged value is now never stored and its trigger is dropped instead of retried. TAF14 keeps storing an unchanged value again after 15 min as an "alive" heartbeat.

## [1.3.9] - 2026-09-27

### Fixed
- `/showLogBuffer` returned HTTP 200 with an empty body once the log ring buffer was full (it fills in ~1 h of normal operation: ~7 entries per 2 min) and the heap had fragmented. The page was built as one ~28 KB `String` and copied twice on return, needing several such blocks at once; when an allocation fails, the Arduino `String` silently empties itself. The page is now streamed row by row (chunked, like `/showMeterValues`). `Log_BufferToString()` is reduced to the short table excerpt used by `/sysinfo`; rows come from the new `Log_EntryRowByAge()`.

## [1.3.8] - 2026-09-27

### Fixed
- Meter values were stored with 1970 timestamps until the first NTP sync (e.g. after a power outage when the ESP boots faster than the router, or while the internet stays down). The backend rejects them as `older_than_db_prev`, and for a device without DB history they were inserted as 1970 rows. TAF7/TAF14 triggers now only fire once the system time is plausible (new `Time_isSynced()` in `time_utils`), and `MeterValue_store()` additionally refuses a reading whose telegram timestamp predates the sync (trigger stays set, the next telegram is stored). The boot snapshot takes the first value after the sync. A software reset (OTA, restart) keeps the system time, so it is unaffected.
- The first backend call after boot fired immediately instead of "as soon as NTP is ready" as the comment claimed &ndash; the condition `Time_getEpochTime() > 0` was always true. It now waits for `Time_isSynced()`.
- Home page live update computed the telegram age with the browser clock (`Date.now()`) against the device timestamp &ndash; while the device was not time-synced (1970) or whenever the two clocks differed by more than 30s, the status flipped between "PIN eingegeben" (server-rendered) and "Kein Telegramm empfangen" / "Empfange Bytes, kann Telegramm nicht lesen" (script). `/showLastMeterValue` now returns `age_s` computed with the device clock, and the page uses it.

### Changed
- Time sync is more robust, since meter values now depend on it: the three NTP servers now come from three different operators (`ptbtime1.ptb.de`, `de.pool.ntp.org`, `time.cloudflare.com`) instead of all from PTB, and if NTP never succeeds (e.g. UDP/123 blocked) the time is taken from the `Date:` header of the backend's HTTPS response (log and meter-value uploads) &ndash; only while NTP is not synced and only on a deviation of more than 2 s. TLS works without a valid time (`MBEDTLS_HAVE_TIME_DATE` is off in the Arduino SDK), so the log upload right after WiFi connect can deliver the time. SNTP setup moved to `Time_begin()` in `time_utils`.

### Added
- Home page shows a "Warte auf Zeitsynchronisation" status card while the system time is not synced; `/showLastMeterValue` gained a `time_synced` field and the page reloads when it flips.
- Ring-log codes `1027` ("Store skipped: system time not synced"), `1028` ("Time synced via NTP", first sync only) and `1029` ("Time set from backend HTTP Date header"), mapped in the Grafana log query; `1027`/`1029` are suppressed on consecutive repeats.
- Test-only build flag `TEST_NTP_BLOCKED` replaces the NTP servers with unresolvable names to exercise the Date-header fallback without blocking UDP/123 on the router; usage is documented at `Time_begin()`.

## [1.3.7] - 2026-09-27

### Fixed
- Meter values stored while a backend upload was in flight could be lost: the upload task clears the whole ring buffer after HTTP 200, but `loop()` kept writing new entries into it between sending the payload and receiving the response, so those entries were wiped without ever being transmitted. A new mutex (`Sema_MeterBuffer`) now guards the buffer from computing the upload ranges until it is cleared (or the send fails); `handle_MeterValue_store()` only try-locks it and, if the upload holds it, leaves the trigger set so the store is simply retried ~1 s later instead of blocking `loop()`.

### Added
- Ring-log code `1026` ("Store deferred: meter value upload in progress"), suppressed on consecutive repeats; added `1025` and `1026` to the Grafana log query mapping.

## [1.3.6] - 2026-09-15

### Fixed
- `/flashlong` (used by the "Zustand wechseln" button on both PIN Assistant pages to toggle `InF`/`PIN` on/off on the meter's optical interface) sent a 1500&nbsp;ms pulse, but the FNN "Lastenheft Basiszähler" spec that all certified German mME meters implement requires a "langer Tastendruck" of at least ~5 seconds &ndash; a 1.5s pulse falls below even the short-press threshold (&lt;2s) these meters use, so it never registered as a long press and the toggle silently did nothing. Verified against six manufacturers' official manuals (Landis+Gyr, DZG, Iskraemeco, Apator, EFR, ZPA) &ndash; all specify the same ~5s long-press threshold, confirming this wasn't a single-meter quirk. Default long-pulse duration raised to 6000&nbsp;ms.
- `Webserver_Flashlight`'s numpad `digit(0)` button skipped the ~3s settle wait entirely (it returned immediately instead of calling the countdown), so a `0` in the PIN could be advanced past before the meter had accepted it. Now waits the full settle time like every other digit.
- `/sysinfo`'s 2&times;2 quick-link grid (`Systemparameter`/`WLAN-Netzwerke`/`PIN Assistant`/`PIN Assistant Deluxe`) overflowed off-screen on narrow phones &ndash; the `.cfg-link` cards had no `min-width:0`, so the grid tracks grew to fit each card's unshrinkable content instead of wrapping the text. Fixed by letting the cards shrink (`min-width:0` on `.cfg-link`/`.cfg-text`, `overflow-wrap:break-word` on the title) and, since a single long unbreakable word like "Systemparameter" still wrapped awkwardly mid-word in a squeezed 2-column card, by stacking the grid to one column below 480&nbsp;px width (new `.cfg-grid` class replacing the inline grid style in `Webserver_HandleSysInfo()`) so every card gets full width instead.

### Added
- `/flash` and `/flashlong` now accept an optional `ms` query arg to override the pulse duration (clamped to sane bounds server-side), since the "long press" threshold and other timings are meter-model-specific and previously hardcoded for one model.
- Both PIN Assistant pages gained a collapsible "Timing-Einstellungen" panel (short pulse, long pulse, inter-pulse gap, digit-settle wait, all in ms) so a different meter model's thresholds can be dialed in from the UI without a firmware rebuild; values persist per-browser via `localStorage`.
- Hint text for "Zustand wechseln" now explains the actual confirm step: the long pulse only changes the displayed `InF`/`PIN` option, then a single short "Weiter" press commits it and advances the menu &ndash; confirmed against real hardware and cross-checked against six manufacturers' official manuals, so the same instructions work across meter brands, not just one.
- Both PIN Assistant pages gained a 👀 hint reminding the customer to watch the meter's own display during the procedure (the firmware has no way to read it back), and were reworked to fit within one screen on common phones without scrolling (smaller buttons/paddings, shortened hint text, status/progress text sharing one row).

## [1.3.5] - 2026-09-06

### Fixed
- TAF7 (15-min mark) and TAF14 (regular interval) readings could both fire within seconds of the same quarter-hour boundary, producing two closely-spaced buffer entries and shifting the TAF14 cadence to a new, effectively random second-of-minute offset until the next boundary &ndash; `handle_MeterValue_trigger()` now holds TAF14 back for the whole 15s TAF7 window instead of only when TAF7 has already claimed the slot. (A timestamp-flooring approach that also snapped TAF7 entries to the exact TAF7 mark was prototyped in `MeterValue_store()` but parked behind `#if 0` &ndash; it trades real telegram-arrival precision, usually within ~2s of the mark, for a cosmetically exact one.)
- Re-enabled the "remove recent non-override entry" logic in `handle_MeterValue_store()` (previously disabled behind `#if 0`): a TAF14 reading stored in the 10s immediately before a TAF7 trigger is now deleted from the ring buffer so the TAF7 mark isn't cluttered with an almost-duplicate value a few seconds apart &ndash; added `MeterValue_ClearSlot()` to `meter_value.cpp`/`meter_value.h` since the buffer's internal offset helper is no longer reachable from `main.cpp` after the modular split

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
