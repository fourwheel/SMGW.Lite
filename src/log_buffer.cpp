#include "log_buffer.h"
#include "time_utils.h"
#include "html_style.h"
#include <Arduino.h>
#if defined(ESP32)
#include <esp_system.h>
#endif

static LogEntry logBuffer[LOG_BUFFER_SIZE];
static int logIndex = -1;

static const int LOG_SUPPRESS_IDS[] = {1200, 1201, 1206, 1022, 3006};
static int last_logged_statusCode = -1;

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
  bool suppressable = false;
  for (size_t i = 0; i < sizeof(LOG_SUPPRESS_IDS) / sizeof(LOG_SUPPRESS_IDS[0]); i++) {
    if (statusCode == LOG_SUPPRESS_IDS[i]) { suppressable = true; break; }
  }
  if (suppressable && statusCode == last_logged_statusCode) return;
  last_logged_statusCode = statusCode;

  logIndex = (logIndex + 1) % LOG_BUFFER_SIZE;
  logBuffer[logIndex].timestamp  = Time_getEpochTime();
  logBuffer[logIndex].uptime     = millis();
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
  case 1004: return "Buffer layout changed, re-initialising";
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
  case 1200: return "meter value <= 0";
  case 1201: return "current Meter value = previous meter value";
  case 1203: return "Suffix Must not be 0";
  case 1204: return "prefix suffix not correct";
  case 1205: return "Error Buffer Size Exceeded";
  case 1206: return "Buffer Full, cannot store non-override value";
  case 1207: return "meter_value_180 < PrevMeterValue — truncated telegram discarded";
  case 3000: return "Complete Telegram received";
  case 3001: return "Telegram Buffer overflow";
  case 3002: return "Telegram timeout";
  case 3003: return "SML Protocoll";
  case 3004: return "IEC Protocoll";
  case 3005: return "No telegram received for 5 min";
  case 3006: return "Serial Msg received but parse failed (check baud/parity)";
  case 3010: return "Serial scan: valid config(s) found — activate manually";
  case 3011: return "Serial scan: no valid configuration found";
  case 3012: return "Serial config: manually set via web UI";
  case 4000: return "Connection to server failed (Cert!?)";
  case 4001: return "Error transmitting Buffer Chunk";
  case 4002: return "Meter values send failed (no HTTP 200)";
  case 4003: return "Log send failed (no HTTP 200)";
  case 5000: return "myStrom_get_Meter_value Connection failed";
  case 5001: return "Failed to connect to myStrom";
  case 5002: return "myStrom_get_Meter_value deserializeJson() failed";
  case 7000: return "Stopping Wifi, Backendcall unsuccessful";
  case 7001: return "Restarting Wifi";
  case 8000: return "Spiffs not mounted";
  case 8001: return "Error reading cert file";
  case 8002: return "Cert saved";
  case 8003: return "Error reading cert file";
  case 8004: return "No Cert received";
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

String Log_BufferToString(int showNumber)
{
  bool fullPage = showNumber > 10;
  int showed_number = 0;
  String logString;

  if (fullPage) {
    logString  = R"rawliteral(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>SmartMeterLite &ndash; Log Buffer</title>)rawliteral";
    logString += String(HTML_STYLE_MODERN);
    logString += R"rawliteral(</head>
<body>
<div class="logo">&#9889; SmartMeterLite</div>
<a class="back" href="/sysinfo">&#8592; Zur&uuml;ck</a>
<div class="card" style="max-width:800px;">
<div class="card-title">Log Buffer</div>
<div class="tbl">)rawliteral";
  }

  logString += "<table><tr><th>Index</th><th>Timestamp</th><th>Timestamp</th><th>Uptime</th><th>Statuscode</th><th>Status</th></tr>";

  for (int i = logIndex; i >= 0; i--) {
    logString += Log_EntryToString(i);
    if (++showed_number >= showNumber)
      return logString + "</table>" + (fullPage ? "</div></div></body></html>" : "");
  }
  if (logIndex < LOG_BUFFER_SIZE - 1) {
    for (int i = LOG_BUFFER_SIZE - 1; i > logIndex; i--) {
      logString += Log_EntryToString(i);
      if (++showed_number >= showNumber) break;
    }
  }
  return logString + "</table>" + (fullPage ? "</div></div></body></html>" : "");
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
