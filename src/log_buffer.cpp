#include "log_buffer.h"
#include "time_utils.h"
#include <Arduino.h>
#if defined(ESP32)
#include <esp_system.h>
#endif

static LogEntry logBuffer[LOG_BUFFER_SIZE];
static int logIndex = -1;

// ---------------------------------------------------------------------------
// Log suppression — consecutive duplicate filtering
// Status codes listed here are suppressed when they repeat back-to-back.
// The first occurrence is always written; subsequent identical codes are
// dropped until a different code is logged.
// ---------------------------------------------------------------------------
static const int LOG_SUPPRESS_IDS[] = {1200, 1201, 1206, 1022, 1026, 1027, 1029, 3006, 6000, 6002};
static int last_logged_statusCode = -1; // last code actually written to the buffer

void LogBuffer_reset()
{
  for (int i = 0; i < LOG_BUFFER_SIZE; ++i) {
    logBuffer[i].timestamp  = 0;
    logBuffer[i].uptime     = 0;
    logBuffer[i].statusCode = -1;
  }
  logIndex = -1;
  last_logged_statusCode = -1;
}

void Log_AddEntry(int statusCode)
{
  // Suppress consecutive duplicates for known noisy status codes.
  // Check if this code is in the suppression list.
  bool suppressable = false;
  for (size_t i = 0; i < sizeof(LOG_SUPPRESS_IDS) / sizeof(LOG_SUPPRESS_IDS[0]); i++) {
    if (statusCode == LOG_SUPPRESS_IDS[i]) { suppressable = true; break; }
  }
  if (suppressable && statusCode == last_logged_statusCode) return;
  last_logged_statusCode = statusCode;

  // Advance ring-buffer write pointer, overwriting oldest entry on wrap.
  logIndex = (logIndex + 1) % LOG_BUFFER_SIZE;

  logBuffer[logIndex].timestamp  = Time_getEpochTime();
  logBuffer[logIndex].uptime     = millis(); // ms since boot — monotonic tiebreaker for same-second entries
  logBuffer[logIndex].statusCode = statusCode;
}

const LogEntry* Log_getRawBuffer() { return logBuffer; }
int             Log_getIndex()     { return logIndex; }

String Log_StatusCodeToString(int statusCode)
{
  switch (statusCode)
  {
  case 1001: return "setup()";
  case 1002: return "Memory Allocation failed";
  case 1003: return "Config saved";
  case 1004: return "Feature config applied - buffer reinitialised";
  case 1005: return "call_backend()";
  case 1006: return "Taf 6 meter reading trigger";
  case 1008: return "WiFi returned";
  case 1009: return "WiFi lost";
  case 1010: return "Taf 7 meter reading trigger";
  case 1011: return "Taf 14 meter reading trigger";
  case 1012: return "call backend trigger";
  case 1013: return "MeterValues_clear_Buffer()";
  case 1014: return "Taf 7-900s meter reading trigger";
  case 1015: return "not enough heap to store value";
  case 1016: return "Buffer full, cannot store non-override value";
  case 1017: return "Meter Value stored";
  case 1018: return "dynamic Taf trigger";
  case 1019: return "Sending Log";
  case 1020: return "Sending Log successful";
  case 1021: return "call_backend successful";
  case 1022: return "taf14 trigger not possible, buffer full";
  case 1023: return "No backend host configured, skipping";
  case 1024: return "Boot snapshot triggered";
  case 1025: return "TAF7: removed TAF14 entry stored <10s before TAF7 value";
  case 1026: return "Store deferred: meter value upload in progress";
  case 1027: return "Store skipped: system time not synced";
  case 1028: return "Time synced via NTP";
  case 1029: return "Time set from backend HTTP Date header (NTP not synced)";
  // 1100 + esp_reset_reason(), logged once at boot
  case 1100: return "Boot: reset reason unknown";
  case 1101: return "Boot: power on";
  case 1102: return "Boot: external reset (reset pin)";
  case 1103: return "Boot: software restart (OTA, user, self-recovery)";
  case 1104: return "Boot: exception/panic";
  case 1105: return "Boot: interrupt watchdog";
  case 1106: return "Boot: task watchdog";
  case 1107: return "Boot: other watchdog";
  case 1108: return "Boot: wake from deep sleep";
  case 1109: return "Boot: brownout (supply voltage dropped)";
  case 1110: return "Boot: SDIO reset";
  case 1111: return "Boot: USB reset";
  case 1112: return "Boot: JTAG reset";
  case 1120: return "Self-recovery restart: main loop hung for 10 min";
  case 1121: return "Self-recovery restart: meter upload hung for 10 min";
  case 1122: return "Self-recovery restart: log upload hung for 10 min";
  case 1200: return "meter value <= 0";
  case 1201: return "current Meter value = previous meter value";
  case 1203: return "Suffix Must not be 0";
  case 1204: return "prefix suffix not correct";
  case 1205: return "Error Buffer Size Exceeded";
  case 1206: return "Buffer Full, cannot store non-override value";
  case 1207: return "meter_value_180 < PrevMeterValue — truncated telegram discarded";
  case 1208: return "meter rollback detected — value forwarded to backend";
  case 3000: return "Complete Telegram received";
  case 3001: return "Telegram Buffer overflow";
  case 3002: return "Telegram timeout";
  case 3003: return "SML Protocoll";
  case 3004: return "IEC Protocoll";
  case 3005: return "No valid telegram parsed for 5 min";
  case 3007: return "No serial data received for 5 min";
  case 3006: return "Serial Msg received but parse failed (check baud/parity)";
  case 3010: return "Serial scan: valid config(s) found — activate manually";
  case 3011: return "Serial scan: no valid configuration found";
  case 3012: return "Serial config: manually set via web UI";
  case 4000: return "Connection to server failed (Cert!?)";
  case 4001: return "Error transmitting Buffer Chunk";
  case 4002: return "Meter values send failed (no HTTP 200)";
  case 4003: return "Log send failed (no HTTP 200)";
  case 4004: return "Meter values: HTTP 200 without valid acknowledgement (bytes/crc32) — buffer kept";
  case 4005: return "Log: HTTP 200 without valid acknowledgement (bytes/crc32)";
  case 5000: return "myStrom_get_Meter_value Connection failed";
  case 5001: return "Failed to connect to myStrom";
  case 5002: return "myStrom_get_Meter_value deserializeJson() failed";
  case 7000: return "WiFi reconnect: no acknowledged backend call for 30 min (or 2 backend intervals)";
  case 7001: return "Restarting Wifi";
  case 7002: return "WiFi-Setup: connection attempt failed";
  case 7003: return "WiFi-Setup: connected to previously-saved network instead of requested SSID";
  case 7004: return "WiFi-Setup: connection confirmed, credentials saved";
  case 8000: return "Spiffs not mounted";
  case 8001: return "No custom cert, using bundled ISRG Root X1";
  case 8002: return "Cert saved";
  case 8003: return "Error reading cert file";
  case 8004: return "No Cert received";
  case 6000: return "OTA pull: check started";
  case 6001: return "OTA pull: manifest — connection failed (until 1.3.12)";
  case 6002: return "OTA pull: firmware is up to date";
  case 6003: return "OTA pull: update available, starting download";
  case 6004: return "OTA pull: Update.begin() failed (check partition table)";
  case 6005: return "OTA pull: stream write error";
  case 6006: return "OTA pull: SHA256 mismatch — download aborted";
  case 6007: return "OTA pull: Update.end() failed";
  case 6008: return "OTA pull: flash successful — rebooting";
  case 6009: return "OTA pull: post-OTA validation — contacting backend";
  case 6010: return "OTA pull: validation successful — firmware confirmed";
  case 6011: return "OTA pull: validation failed — rolling back";
  case 6012: return "OTA pull: manifest — server returned non-200 (until 1.3.12)";
  case 6013: return "OTA pull: fw_update / manifest — invalid or missing fields";
  case 6014: return "OTA pull: check triggered by backend (fw_update offer / ota_check hint)";
  case 6015: return "OTA pull: check triggered by 24 h fallback (until 1.3.12)";
  case 6016: return "OTA pull: skipped — WiFi not connected";
  case 6017: return "OTA pull: skipped — ota_active flag set";
  case 6018: return "OTA pull: skipped — backend ID empty";
  case 6019: return "OTA pull: skipped — backend host empty";
  case 6020: return "OTA pull: cooldown after rollback — skipping for 15 min";
  case 6021: return "OTA pull: check triggered manually";
  case 6022: return "OTA pull: check triggered by manual install confirmation";
  case 6023: return "OTA pull: download — connection failed";
  case 6024: return "OTA pull: download — server returned non-200 (release missing?)";
  case 6025: return "OTA pull: offered version was rolled back before — not installed automatically";
  case 6026: return "OTA pull: download took longer than 4 min — aborted";
  case 6101: return "Manual update: Update.begin() failed";
  case 6102: return "Manual update: write error during upload";
  case 6103: return "Manual update: Update.end() failed";
  case 6104: return "Manual update: upload successful, rebooting";
  }
  if (statusCode < 1000) return "# meter slots to transfer";
  return "Unknown status code";
}

static String Log_EntryToString(int i)
{
  if (logBuffer[i].statusCode == -1) return "";
  String s = "<tr><td>";
  s += String(i) + "</td><td>";
  s += String(logBuffer[i].timestamp) + "</td><td>";
  s += Time_formatTimestamp(logBuffer[i].timestamp) + "</td><td>";
  s += String(logBuffer[i].uptime) + "</td><td>";
  s += String(logBuffer[i].statusCode) + "</td><td>";
  s += Log_StatusCodeToString(logBuffer[i].statusCode);
  s += "</td></tr>";
  return s;
}

const char LOG_TABLE_HEADER_HTML[] =
  "<table><tr><th>Index</th><th>Timestamp</th><th>Timestamp</th><th>Uptime</th><th>Statuscode</th><th>Status</th></tr>";

// Returns the HTML table row of the n-th newest entry (0 = newest), or "" if
// that slot is unused. Walking n = 0..LOG_BUFFER_SIZE-1 visits the ring buffer
// newest-first, including the wrap-around.
String Log_EntryRowByAge(int n)
{
  if (logIndex < 0 || n < 0 || n >= LOG_BUFFER_SIZE) return "";
  return Log_EntryToString((logIndex - n + LOG_BUFFER_SIZE) % LOG_BUFFER_SIZE);
}

// Short excerpt for /sysinfo. The full log page is streamed row by row
// instead (Webserver_ShowLogBuffer) — building all 200 rows as one String
// needs several ~30 KB blocks at once and fails on a fragmented heap.
String Log_BufferToString(int showNumber)
{
  String logString = LOG_TABLE_HEADER_HTML;
  for (int n = 0; n < showNumber; n++) logString += Log_EntryRowByAge(n);
  logString += "</table>";
  return logString;
}

#if defined(ESP32)
String Log_get_reset_reason()
{
  switch (esp_reset_reason())
  {
  case ESP_RST_UNKNOWN:   return "Unknown";
  case ESP_RST_POWERON:   return "Power on";
  case ESP_RST_EXT:       return "External reset";
  case ESP_RST_SW:        return "Software reset";
  case ESP_RST_PANIC:     return "Exception/panic";
  case ESP_RST_INT_WDT:   return "Interrupt watchdog";
  case ESP_RST_TASK_WDT:  return "Task watchdog";
  case ESP_RST_WDT:       return "Other watchdogs";
  case ESP_RST_DEEPSLEEP: return "Deep sleep";
  case ESP_RST_BROWNOUT:  return "Brownout";
  case ESP_RST_SDIO:      return "SDIO";
  default:                return "Unknown";
  }
}
#endif
