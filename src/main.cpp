/* MIT License

Copyright (c) [2025] Laurin Vierrath

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is provided to
do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <IotWebConf.h>
#include <IotWebConfUsing.h> // This loads aliases for easier class names.
#include <IotWebConfMultipleWifi.h>
#include <SPIFFS.h>
#if defined(ESP32)
#include <WiFi.h>
#include <ESPmDNS.h>
#include <HardwareSerial.h>
#include <HTTPClient.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <SoftwareSerial.h>
#include <ESP8266HTTPClient.h>
#endif
#include <ArduinoJson.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include "NTPClient.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include "Arduino.h"

// ---------------------------------------------------------------------------
// Serial debug macros
// Build with -DSERIAL_DEBUG (env:esp32c3_debug) to enable Serial output.
// In the release build (env:esp32c3) all DLOG* calls compile to nothing.
// OTA progress/error output intentionally uses Serial directly so it always
// works regardless of this flag.
// ---------------------------------------------------------------------------
#ifdef SERIAL_DEBUG
  #define DLOG(x)    Serial.print(x)
  #define DLOGLN(x)  Serial.println(x)
  #define DLOGF(...) Serial.printf(__VA_ARGS__)
#else
  #define DLOG(x)    do {} while(0)
  #define DLOGLN(x)  do {} while(0)
  #define DLOGF(...) do {} while(0)
#endif

const String BUILD_TIMESTAMP = String(__DATE__) + " " + String(__TIME__);

// -- Initial name of the Thing. Used e.g. as SSID of the own Access Point.
const char thingName[] = "SMGWLite";

// -- Initial password to connect to the Thing, when it creates an own Access Point.
const char wifiInitialApPassword[] = "password";

#define STRING_LEN 128
#define ID_LEN 4
#define NUMBER_LEN 5

// -- Configuration specific key. The value should be modified if config structure was changed.
#define CONFIG_VERSION "2906"

// -- When CONFIG_PIN is pulled to ground on startup, the Thing will use the initial
//      password to build an AP. (E.g. in case of lost password)
#ifndef D2
#define D2 3
#endif

#define CONFIG_PIN D2

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

#define STATUS_PIN LED_BUILTIN

// Telegram vars
const uint8_t SML_SIGNATURE_START[] = {0x1b, 0x1b, 0x1b, 0x1b, 0x01, 0x01, 0x01, 0x01};
const uint8_t SML_SIGNATURE_END[]   = {0x1b, 0x1b, 0x1b, 0x1b, 0x1a};
#define TELEGRAM_LENGTH 1024
#define TELEGRAM_TIMEOUT_MS 30                    // timeout for telegram in ms
size_t TelegramSizeUsed = 0;                      // actual size of stored telegram
uint8_t telegram_receive_buffer[TELEGRAM_LENGTH]; // buffer for serial data
size_t telegram_receive_bufferIndex = 0;          // position in serial data buffer
bool readingExtraBytes = false;                   // reading additional bytes?
uint8_t extraBytes[3];                            // additional bytes after end signature
size_t extraIndex = 0;                            // index of additional bytes
unsigned long lastByteTime = 0;                   // timestamp of last received byte
unsigned long timestamp_telegram;                 // timestamp of telegram


// ---------------------------------------------------------------------------
// MeterValue "working copy" struct
// This struct is only used for the most recent reading (LastMeterValue /
// PrevMeterValue). It always contains all fields regardless of which features
// are enabled. The ring-buffer uses a compact packed binary layout instead
// (see MeterValueBuffer below), so unused fields cost only 8 bytes here,
// not N * buffer_size bytes.
// ---------------------------------------------------------------------------
struct MeterValue
{
  uint32_t timestamp;        // Unix epoch, seconds
  uint32_t meter_value_180;  // OBIS 1.8.0 consumption counter, unit: 0.1 Wh
  uint32_t temperature;      // temperature * 100 (e.g. 2150 = 21.50 degC)
  uint32_t solar;            // MyStrom / solar energy counter
  uint32_t meter_value_280;  // OBIS 2.8.0 feed-in counter, unit: 0.1 Wh
};

// Working copies — always kept in the full struct format for easy access
MeterValue LastMeterValue = {0, 0, 0, 0, 0};
MeterValue PrevMeterValue = {0, 0, 0, 0, 0};

bool MeterValue_trigger_override     = false;
bool MeterValue_trigger_non_override = false;

unsigned long last_meter_value_successful = 0;
unsigned long last_taf7_meter_value       = 0;
unsigned long last_taf14_meter_value      = 0;

// ---------------------------------------------------------------------------
// Runtime feature flags — set from IotWebConf config parameters.
// These control which optional fields are packed into the ring-buffer and
// sent to the backend. Changing them at runtime triggers a buffer re-init.
//
// WARNING: changing these flags clears the buffer. The webserver UI warns
// the user to send pending values before saving a config change.
// ---------------------------------------------------------------------------
bool config_temperature_enabled = false; // store temperature field in buffer
bool config_solar_enabled       = false; // store PV / MyStrom solar field in buffer
bool config_obis280_enabled     = false; // store OBIS 2.8.0 feed-in field in buffer

// ---------------------------------------------------------------------------
// Packed ring-buffer  (Written by Claude)
// Instead of an array of MeterValue structs (fixed 20 bytes each), we use a
// raw byte buffer whose entry size is calculated at runtime from the feature
// flags. This saves significant RAM on installations that don't need all fields.
//
// Binary layout per entry — fields always appear in this fixed order,
// but only the enabled ones are actually stored:
//
//   Offset  Field            Size  Always?
//   ------  ---------------  ----  -------
//   0       timestamp        4     yes
//   4       meter_value_180  4     yes
//   8       temperature      4     only if config_temperature_enabled
//   next    solar            4     only if config_solar_enabled
//   next    meter_value_280  4     only if config_obis280_enabled
//
// The PHP backend reconstructs the same layout using the "fields=" URL
// parameter sent with every POST. See Webclient_send_meter_values_to_backend().
// ---------------------------------------------------------------------------
uint8_t *MeterValueBuffer = nullptr; // heap-allocated packed ring-buffer

int Meter_Value_Buffer_Size      = 234;
bool meter_value_buffer_overflow = false;
bool meter_value_buffer_full     = false;
// write pointer for override (TAF7) entries — grows upward from 0
int meter_value_override_i       = 0;
// write pointer for non-override (TAF14) entries — grows downward from end
int meter_value_NON_override_i   = Meter_Value_Buffer_Size - 1;

float currentPower, LastPower = 0.0;

int staticDelay = 0;

// Cached integer versions of config string params — updated in Param_configSaved() and setup().
// Avoids calling atoi() on every loop() tick.
int cached_taf7_param          = 15;
int cached_taf14_param         = 60;
int cached_backend_call_minute = 2;

// Backend vars
bool call_backend_successfull = true;
SemaphoreHandle_t Sema_Backend;       // Mutex / Semaphore for backend call
unsigned long last_call_backend = 0;

// Temperature vars
#define ONE_WIRE_BUS 4
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature Temp_sensors(&oneWire);
unsigned long last_temperature  = 0;
bool read_temperature           = false;
int current_temperature         = 123;

// Log vars
const int LOG_BUFFER_SIZE = 200;

// Task watermark vars
int watermark_meter_buffer = 0;
int watermark_log_buffer   = 0;
int watermark_telegram     = 0;

// WiFi vars
unsigned long wifi_reconnection_time = 0;
unsigned long last_wifi_retry        = 0;
unsigned long last_wifi_check;
bool wifi_connected;
int IPlastOctet = -1;

unsigned long last_remote_meter_value = 0;

// -- Forward declarations.
void handle_call_backend();
void handle_check_wifi_connection();
void handle_MeterValue_trigger();
void handle_MeterValue_store();
void handle_temperature();
void Led_update_Blink();
String Log_BufferToString(int showNumber = LOG_BUFFER_SIZE);
String Log_EntryToString(int i);
String Log_StatusCodeToString(int statusCode);
#if defined(ESP32)
String Log_get_reset_reason();
#endif
bool MeterValue_store(bool override);
void MeterValues_clear_Buffer();
void resetMeterValue(MeterValue &val);
int MeterValue_Num();
int MeterValue_Num2();
int32_t MeterValue_get_from_remote();
bool Telegram_parse_SML(uint8_t* buffer, size_t length);
bool Telegram_parse_IEC(uint8_t* buffer, size_t length);
void OTA_setup();
void Param_configSaved();
void Param_setup();
String Time_formatTimestamp(unsigned long timestamp);
String Time_formatUptime();
String Time_getFormattedTime();
unsigned long Time_getEpochTime();
int Time_getMinutes();
bool Telegram_prefix_suffix_correct();
void Telegram_ResetReceiveBuffer();
void handle_Telegram_receive();
void Webclient_send_log_to_backend();
void Webclient_Send_Log_to_backend_Task(void *pvParameters);
void Webclient_Send_Log_to_backend_wrapper();
void Webclient_send_meter_values_to_backend();
void Webclient_Send_Meter_Values_to_backend_Task(void *pvParameters);
void Webclient_Send_Meter_Values_to_backend_wrapper();
void Webclient_splitHostAndPath(const String &url, String &host, String &path);
void Webserver_HandleCertUpload();
void Webserver_HandleRoot();
void Webserver_LocationHrefHome(int delay = 0);
void Webserver_SetCert();
void Webserver_ShowCert();
void Webserver_ShowLastMeterValue();
void Webserver_ShowLogBuffer();
void Webserver_ShowMeterValues();
void Webserver_ShowTelegram();
void Webserver_ShowTelegram_Raw();
void Webserver_TestBackendConnection();
void Webserver_UrlConfig();

DNSServer dnsServer;
WebServer server(80);

#if defined(ESP32)
HardwareSerial mySerial(1); // RX, TX
#elif defined(ESP8266)
SoftwareSerial mySerial(D5, D6); // RX, TX
#endif

// Params, which you can set via webserver
char backend_endpoint[STRING_LEN];
char led_blink[STRING_LEN];
char UseSslCertValue[STRING_LEN];
char DebugSetOfflineValue[STRING_LEN];
char DebugMeterValueFromOtherClient[STRING_LEN];
char DebugMeterValueFromOtherClientIP[STRING_LEN];
char mystrom_PV[STRING_LEN];
char mystrom_PV_IP[STRING_LEN];
char temperature_checkbock[STRING_LEN];
char backend_token[STRING_LEN];
char b_taf7[STRING_LEN];
char taf7_param[NUMBER_LEN];
char b_taf14[STRING_LEN];
char taf14_param[NUMBER_LEN];
char b_tafdyn[STRING_LEN];
char tafdyn_absolute[NUMBER_LEN];
char tafdyn_multiplicator[NUMBER_LEN];
char backend_call_minute[NUMBER_LEN];
char backend_ID[ID_LEN];
char activate_IEC_Parser[STRING_LEN];
char Meter_Value_Buffer_Size_Char[NUMBER_LEN] = "0";    // 0 = auto (16 KB reference budget), >0 = manual KB

// ---------------------------------------------------------------------------
// Config storage for the three optional packed-buffer field flags.
// Persisted by IotWebConf; read back on every boot and every config-save.
// The checkboxes appear in the "Additional Meters & Sensors" config group.
// ---------------------------------------------------------------------------
char config_temperature_char[STRING_LEN]; // "true" when temperature field is stored in buffer
char config_solar_char[STRING_LEN];       // "true" when solar/PV field is stored in buffer
char config_280_char[STRING_LEN];         // "true" when OBIS 2.8.0 field is stored in buffer

IotWebConf iotWebConf(thingName, &dnsServer, &server, wifiInitialApPassword, CONFIG_VERSION);
// -- You can also use namespace formats e.g.: iotwebconf::TextParameter

iotwebconf::ChainedWifiParameterGroup chainedWifiParameterGroups[] = {
  iotwebconf::ChainedWifiParameterGroup("wifi1")
};

iotwebconf::MultipleWifiAddition multipleWifiAddition(
  &iotWebConf,
  chainedWifiParameterGroups,
  sizeof(chainedWifiParameterGroups) / sizeof(chainedWifiParameterGroups[0]));

iotwebconf::OptionalGroupHtmlFormatProvider optionalGroupHtmlFormatProvider;

IotWebConfParameterGroup groupTelegram      = IotWebConfParameterGroup("groupTelegram",      "Telegram Param");
IotWebConfParameterGroup groupBackend       = IotWebConfParameterGroup("groupBackend",       "Backend Config");
IotWebConfParameterGroup groupTaf           = IotWebConfParameterGroup("groupTaf",           "Taf config");
IotWebConfParameterGroup groupAdditionalMeter = IotWebConfParameterGroup("groupAdditionalMeter", "Additional Meters & Sensors");
IotWebConfParameterGroup groupSys           = IotWebConfParameterGroup("groupSys",           "Advanced Sys Config");
IotWebConfParameterGroup groupDebug         = IotWebConfParameterGroup("groupDebug",         "Debug Helpers");

IotWebConfCheckboxParameter activate_IEC_Parser_object = IotWebConfCheckboxParameter("activate IEC Parser (instead of SML)", "activate_IEC_Parser", activate_IEC_Parser, STRING_LEN, false);

IotWebConfTextParameter     backend_endpoint_object     = IotWebConfTextParameter("backend endpoint", "backend_endpoint", backend_endpoint, STRING_LEN);
IotWebConfCheckboxParameter led_blink_object            = IotWebConfCheckboxParameter("LED Blink", "led_blink", led_blink, STRING_LEN, DNS_FALLBACK_SERVER_INDEX);
IotWebConfTextParameter     backend_ID_object           = IotWebConfTextParameter("backend ID", "backend_ID", backend_ID, ID_LEN);
IotWebConfTextParameter     backend_token_object        = IotWebConfTextParameter("backend token", "backend_token", backend_token, STRING_LEN);

IotWebConfCheckboxParameter taf7_b_object               = IotWebConfCheckboxParameter("Taf 7 activated", "b_taf7", b_taf7, STRING_LEN, true);
IotWebConfNumberParameter   taf7_param_object           = IotWebConfNumberParameter("Taf 7 minute", "taf7_param", taf7_param, NUMBER_LEN, "15", "60...1", "min='1' max='60' step='1'");
IotWebConfCheckboxParameter taf14_b_object              = IotWebConfCheckboxParameter("Taf 14 activated", "b_taf14", b_taf14, STRING_LEN, true);
IotWebConfNumberParameter   taf14_param_object          = IotWebConfNumberParameter("Taf 14 Meter Interval (s)", "taf14_param", taf14_param, NUMBER_LEN, "60", "1..100 s", "min='1' max='100' step='1'");
IotWebConfCheckboxParameter tafdyn_b_object             = IotWebConfCheckboxParameter("Dyn Taf activated", "b_tafdyn", b_tafdyn, STRING_LEN, true);
IotWebConfNumberParameter   tafdyn_absolute_object      = IotWebConfNumberParameter("Dyn Taf absolute Delta", "tafdyn_absolute", tafdyn_absolute, NUMBER_LEN, "100", "Power Delta in Watts", "min='10' max='10000' step='1'");
IotWebConfNumberParameter   tafdyn_multiplicator_object = IotWebConfNumberParameter("Dyn Taf multiplicator", "tafdyn_multiplicator", tafdyn_multiplicator, NUMBER_LEN, "2", "Power n bigger or 1/n smaller", "min='1' max='10' step='0.1'");
IotWebConfNumberParameter   backend_call_minute_object  = IotWebConfNumberParameter("backend Call Minute", "backend_call_minute", backend_call_minute, NUMBER_LEN, "2", "", "");

IotWebConfCheckboxParameter mystrom_PV_object       = IotWebConfCheckboxParameter("MyStrom PV", "mystrom_PV", mystrom_PV, STRING_LEN, false);
IotWebConfTextParameter     mystrom_PV_IP_object    = IotWebConfTextParameter("MyStrom PV IP", "mystrom_PV_IP", mystrom_PV_IP, STRING_LEN);
IotWebConfCheckboxParameter temperature_object      = IotWebConfCheckboxParameter("Temperature Sensor active", "temperature_checkbock", temperature_checkbock, STRING_LEN, false);
IotWebConfCheckboxParameter UseSslCert_object       = IotWebConfCheckboxParameter("Wirk-PKI (Use SSL Cert)", "UseSslCertValue", UseSslCertValue, STRING_LEN, true);

IotWebConfCheckboxParameter DebugSetOffline_object           = IotWebConfCheckboxParameter("Set Device offline (Pretend no Wifi)", "DebugWifi", DebugSetOfflineValue, STRING_LEN, false);
IotWebConfCheckboxParameter DebugFromOtherClient_object      = IotWebConfCheckboxParameter("Get Meter Value from other SMGWLite Client", "DebugFromOtherClient", DebugMeterValueFromOtherClient, STRING_LEN, false);
IotWebConfTextParameter     DebugMeterValueFromOtherClientIP_object = IotWebConfTextParameter("IP to get Meter Values From", "DebugMeterValueFromOtherClientIP", DebugMeterValueFromOtherClientIP, STRING_LEN);

// Buffer memory budget in KB. The slot count is derived automatically from
// the entry size (which depends on which fields are enabled), so the total
// RAM used stays constant regardless of the feature configuration.
// Reference: 1000 slots * 16 bytes (ts+m180+temp+solar) = 16 KB worked well.
// 0 = auto: use the reference budget of 16 KB.
IotWebConfNumberParameter   Meter_Value_Buffer_Size_object   = IotWebConfNumberParameter("Buffer budget (KB, 0=auto)", "Meter_Value_Buffer_Size", Meter_Value_Buffer_Size_Char, NUMBER_LEN, "0", "0=auto (16 KB), or 1...64", "min='0' max='64' step='1'");

// ---------------------------------------------------------------------------
// Checkboxes for the three optional packed-buffer fields.  (Written by Claude)
// Shown in the "Additional Meters & Sensors" config group so they appear
// alongside the sensor settings they relate to.
// Changing any of these saves & triggers MeterValue_init_Buffer(), which
// clears the buffer — the UI warns the user if values are pending.
// ---------------------------------------------------------------------------
IotWebConfCheckboxParameter config_temperature_object = IotWebConfCheckboxParameter("Store Temperature in buffer", "config_temperature", config_temperature_char, STRING_LEN, false);
IotWebConfCheckboxParameter config_solar_object       = IotWebConfCheckboxParameter("Store myStrom in buffer",  "config_solar",       config_solar_char,       STRING_LEN, false);
IotWebConfCheckboxParameter config_280_object         = IotWebConfCheckboxParameter("Store Infeed (2.8.0) in buffer",  "config_280",         config_280_char,         STRING_LEN, false);

const char HTML_STYLE[] PROGMEM = R"rawliteral(
<style>
  html, body {
    background: #fdfdfd;
    margin: 0;
    padding: 15px;
    box-sizing: border-box;
  }

  body {
    font-family: sans-serif;
    /* 3-column layout for desktop */
    column-count: 3;
    column-gap: 25px;
    column-rule: 1px solid #eee;
  }

  /* Single column fallback for mobile */
  @media (max-width: 900px) {
    body { column-count: 1; }
  }

  /* Keep related blocks inside one column */
  h2, h3, table, ul, p, div {
    break-inside: avoid;
    display: block;
    max-width: 100%;
    overflow-x: auto;
  }

  table {
    border-collapse: collapse;
    width: 100%;
    background: white;
    margin-bottom: 1.2em;
    font-size: 0.85em;
  }

  th, td {
    border: 1px solid #ccc;
    padding: 5px 8px;
    text-align: left;
    word-break: break-word;
  }

  th { background: #eee; }
  ul { padding-left: 1.5em; margin-bottom: 1.2em; }
  li { margin-bottom: 0.3em; }
  a { color: #0066cc; text-decoration: none; }
  a:hover { text-decoration: underline; }
  .section {
    display: inline-block;
    width: 100%;
    break-inside: avoid;
    margin-bottom: 20px;
  }
</style>
)rawliteral";

unsigned long Time_getEpochTime()
{
  return static_cast<unsigned long>(time(nullptr));
}

int Time_getMinutes()
{
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  return timeinfo.tm_min; // Extract minutes (0-59)
}

struct LogEntry
{
  unsigned long timestamp; // unix timestamp
  unsigned long uptime;
  int statusCode;
};
LogEntry logBuffer[LOG_BUFFER_SIZE];
int logIndex = -1;

// ---------------------------------------------------------------------------
// Log suppression — consecutive duplicate filtering
// Status codes listed here are suppressed when they repeat back-to-back.
// The first occurrence is always written; subsequent identical codes are
// dropped until a different code is logged.
// ---------------------------------------------------------------------------
const int LOG_SUPPRESS_IDS[] = {1201, 1206, 1022};
int last_logged_statusCode = -1; // last code actually written to the buffer

void LogBuffer_reset()
{
  for (int i = 0; i < LOG_BUFFER_SIZE; ++i)
  {
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
  for (size_t i = 0; i < sizeof(LOG_SUPPRESS_IDS) / sizeof(LOG_SUPPRESS_IDS[0]); i++)
  {
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

// ---------------------------------------------------------------------------
// MeterValue_EntrySize  (Written by Claude)
// Returns the number of bytes per entry in the packed ring-buffer.
// Must match the entry-size calculation in the PHP backend (index.php) and
// the "fields=" URL parameter sent with every POST request.
// ---------------------------------------------------------------------------
size_t MeterValue_EntrySize()
{
  size_t s = 4 + 4; // timestamp + meter_value_180 (always present)
  if (config_temperature_enabled) s += 4; // optional: temperature
  if (config_solar_enabled)       s += 4; // optional: myStrom
  if (config_obis280_enabled)     s += 4; // optional: OBIS 2.8.0
  return s;
}

// Returns the byte offset of entry [index] in the packed buffer.
inline size_t MeterValue_Offset(int index)
{
  return (size_t)index * MeterValue_EntrySize();
}

// ---------------------------------------------------------------------------
// MeterValue_BuildFieldsParam  (Written by Claude)
// Builds the "fields=" URL query parameter that tells the backend exactly
// which fields are present in the binary payload and in what order.
//
// Format: fields=ts,m180[,temp][,solar][,m280]
//
// This is the single authoritative source-of-truth for the wire format.
// The PHP backend parses this string to reconstruct entry size and field
// offsets, making the protocol self-describing and forward-compatible.
// Old clients that don't send this parameter are handled by the legacy
// PV_included / 280_included fallback in the backend.
// ---------------------------------------------------------------------------
String MeterValue_BuildFieldsParam()
{
  // ts and m180 are always present
  String fields = "fields=ts,m180";
  if (config_temperature_enabled) fields += ",temp";
  if (config_solar_enabled)       fields += ",solar";
  if (config_obis280_enabled)     fields += ",m280";
  return fields;
}

// ---------------------------------------------------------------------------
// MeterValue_write  (Written by Claude)
// Packs one MeterValue into the ring-buffer at the given slot index.
// Only fields that are currently enabled are written; disabled fields are
// simply omitted, keeping each entry as compact as possible.
// Guard: does nothing safely if the buffer has not been allocated yet.
// ---------------------------------------------------------------------------
void MeterValue_write(int index, uint32_t ts, uint32_t m180,
                      uint32_t temp, uint32_t solar, uint32_t m280)
{
  if (!MeterValueBuffer) return; // buffer not yet allocated — skip silently
  size_t o = MeterValue_Offset(index);
  memcpy(MeterValueBuffer + o, &ts,   4); o += 4;
  memcpy(MeterValueBuffer + o, &m180, 4); o += 4;
  if (config_temperature_enabled) { memcpy(MeterValueBuffer + o, &temp,  4); o += 4; }
  if (config_solar_enabled)       { memcpy(MeterValueBuffer + o, &solar, 4); o += 4; }
  if (config_obis280_enabled)     { memcpy(MeterValueBuffer + o, &m280,  4); }
}

// ---------------------------------------------------------------------------
// MeterValue_read  (Written by Claude)
// Reads one packed entry from the ring-buffer back into individual fields.
// Fields that are not enabled are returned as 0.
// Guard: returns all-zeros safely if the buffer has not been allocated yet.
// ---------------------------------------------------------------------------
void MeterValue_read(int index, uint32_t &ts, uint32_t &m180,
                     uint32_t &temp, uint32_t &solar, uint32_t &m280)
{
  ts = 0; m180 = 0; temp = 0; solar = 0; m280 = 0;
  if (!MeterValueBuffer) return; // buffer not yet allocated — return zeros safely
  size_t o = MeterValue_Offset(index);
  memcpy(&ts,   MeterValueBuffer + o, 4); o += 4;
  memcpy(&m180, MeterValueBuffer + o, 4); o += 4;
  if (config_temperature_enabled) { memcpy(&temp,  MeterValueBuffer + o, 4); o += 4; }
  if (config_solar_enabled)       { memcpy(&solar, MeterValueBuffer + o, 4); o += 4; }
  if (config_obis280_enabled)     { memcpy(&m280,  MeterValueBuffer + o, 4); }
}

// ---------------------------------------------------------------------------
// MeterValue_slot_empty  (Written by Claude)
// A slot is considered empty when it was zeroed out by memset during init or
// clear (timestamp == 0 && meter_value_180 == 0).
// This replaces the struct field access used in the original MeterValue_store.
// ---------------------------------------------------------------------------
bool MeterValue_slot_empty(int index)
{
  uint32_t ts, m180, temp, solar, m280;
  MeterValue_read(index, ts, m180, temp, solar, m280);
  return (ts == 0 && m180 == 0);
}

// ---------------------------------------------------------------------------
// Reference budget  (Written by Claude)
// 1000 slots * 16 bytes (ts + m180 + temp + solar) = 16384 bytes.
// This is the proven working configuration. The buffer size is now expressed
// as a KB budget — the slot count is always derived from the budget divided
// by the current entry size, so total RAM usage stays constant regardless of
// which fields are enabled.
//
// Example with 16 KB budget:
//   fields=ts,m180          (8  bytes/slot) -> 2048 slots
//   fields=ts,m180,temp     (12 bytes/slot) -> 1365 slots
//   fields=ts,m180,temp,solar (16 bytes/slot) -> 1024 slots  ← reference
//   fields=ts,m180,temp,solar,m280 (20 bytes/slot) -> 819 slots
// ---------------------------------------------------------------------------
const size_t BUFFER_REFERENCE_BYTES = 16384; // 1000 * 16 = proven working budget

// ---------------------------------------------------------------------------
// MeterValue_slots_from_budget  (Written by Claude)
// Converts a byte budget into a slot count for the current entry size.
// Clamps to a minimum of 8 slots.
// ---------------------------------------------------------------------------
int MeterValue_slots_from_budget(size_t budgetBytes)
{
  size_t entrySize = MeterValue_EntrySize();
  if (entrySize == 0) return 8; // should never happen, but guard against div/0
  int slots = (int)(budgetBytes / entrySize);
  if (slots < 8) slots = 8;
  return slots;
}

// ---------------------------------------------------------------------------
// MeterValue_calc_max_slots_for_display  (Written by Claude)
// Returns the slot count the current budget would give, for the info display.
// ---------------------------------------------------------------------------
int MeterValue_calc_max_slots_for_display()
{
  int configuredKB = atoi(Meter_Value_Buffer_Size_Char);
  size_t budget = (configuredKB > 0)
                  ? (size_t)configuredKB * 1024
                  : BUFFER_REFERENCE_BYTES;
  return MeterValue_slots_from_budget(budget);
}

// ---------------------------------------------------------------------------
// MeterValue_init_Buffer  (Written by Claude)
// (Re-)allocates the packed ring-buffer using the current feature flags and
// KB budget from config.
//
// Budget modes:
//   0 (or empty)  -> AUTO: use the 16 KB reference budget
//   > 0           -> MANUAL: use exactly configuredKB * 1024 bytes
//
// The slot count is always derived from budget / entry_size, so enabling or
// disabling fields automatically adjusts the number of slots while keeping
// total RAM usage constant.
// ---------------------------------------------------------------------------
// Tracks whether auto (reference) budget or a manual KB value is active.
bool meter_value_buffer_is_auto = false;

// Tracks the KB config value and feature flags at the time of the last
// MeterValue_init_Buffer() call — used by Param_configSaved() to detect
// whether a layout-relevant setting actually changed.
int  last_init_buffer_kb   = -1;   // -1 = not yet initialised
bool last_init_temp        = false;
bool last_init_solar       = false;
bool last_init_280         = false;

void MeterValue_init_Buffer()
{
  // Apply runtime feature flags first — MeterValue_EntrySize() depends on them.
  config_temperature_enabled = config_temperature_object.isChecked();
  config_solar_enabled       = config_solar_object.isChecked();
  config_obis280_enabled     = config_280_object.isChecked();

  // Determine the byte budget from config.
  int configuredKB = atoi(Meter_Value_Buffer_Size_Char);
  size_t budget;
  if (configuredKB <= 0)
  {
    // Auto mode: use the proven 16 KB reference budget
    budget                     = BUFFER_REFERENCE_BYTES;
    meter_value_buffer_is_auto = true;
  }
  else
  {
    // Manual mode: user specified a KB value
    budget                     = (size_t)configuredKB * 1024;
    meter_value_buffer_is_auto = false;
  }

  // Derive slot count from budget and current entry size
  Meter_Value_Buffer_Size = MeterValue_slots_from_budget(budget);

  // Free old buffer if one exists
  if (MeterValueBuffer)
  {
    delete[] MeterValueBuffer;
    MeterValueBuffer = nullptr;
  }

  size_t total = (size_t)Meter_Value_Buffer_Size * MeterValue_EntrySize();
  MeterValueBuffer = new uint8_t[total];
  if (!MeterValueBuffer)
  {
    DLOGLN("MeterValue buffer allocation failed!");
    Log_AddEntry(1002);
    return;
  }

  // Zero-fill so that MeterValue_slot_empty() works correctly
  memset(MeterValueBuffer, 0, total);

  // Reset both ring-buffer write pointers
  meter_value_override_i      = 0;
  meter_value_NON_override_i  = Meter_Value_Buffer_Size - 1;
  meter_value_buffer_overflow = false;
  meter_value_buffer_full     = false;

  DLOGF("MeterValue buffer init: %d slots x %d bytes = %d bytes total (%s)\n",
        Meter_Value_Buffer_Size, (int)MeterValue_EntrySize(), (int)total,
        meter_value_buffer_is_auto ? "auto" : "manual");
  DLOGF("  Fields: ts,m180%s%s%s\n",
        config_temperature_enabled ? ",temp"  : "",
        config_solar_enabled       ? ",solar" : "",
        config_obis280_enabled     ? ",m280"  : "");

  // Record the settings used for this init so Param_configSaved() can
  // detect whether a layout-relevant change actually happened.
  last_init_buffer_kb = atoi(Meter_Value_Buffer_Size_Char);
  last_init_temp      = config_temperature_enabled;
  last_init_solar     = config_solar_enabled;
  last_init_280       = config_obis280_enabled;
}

bool b_send_log_to_backend = false;

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
  case 1200: return "meter value <= 0";
  case 1201: return "current Meter value = previous meter value";
  case 1203: return "Suffix Must not be 0";
  case 1204: return "prefix suffix not correct";
  case 1205: return "Error Buffer Size Exceeded";
  case 1206: return "Buffer Full, cannot store non-override value";
  case 3000: return "Complete Telegram received";
  case 3001: return "Telegram Buffer overflow";
  case 3002: return "Telegram timeout";
  case 3003: return "SML Protocoll";
  case 3004: return "IEC Protocoll";
  
  case 4000: return "Connection to server failed (Cert!?)";
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
  if (statusCode < 1000)
  {
    return "# meter slot";
  }
  return "Unknown status code";
}

String Time_getFormattedTime()
{
  time_t now = time(nullptr);
  char timeStr[64];
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
  return String(timeStr);
}

String Time_formatTimestamp(unsigned long timestamp)
{
  time_t rawTime = static_cast<time_t>(timestamp);
  struct tm timeinfo;
  localtime_r(&rawTime, &timeinfo);
  char buffer[20];
  strftime(buffer, sizeof(buffer), "%D %H:%M:%S", &timeinfo);
  return String(buffer);
}

String Log_EntryToString(int i)
{
  if (logBuffer[i].statusCode == -1)
    return ""; // don't show empty entries
  String logString = "<tr><td>";
  logString += String(i) + "</td><td>";
  logString += String(logBuffer[i].timestamp) + "</td><td>";
  logString += Time_formatTimestamp(logBuffer[i].timestamp) + "</td><td>";
  logString += String(logBuffer[i].uptime) + "</td><td>";
  logString += String(logBuffer[i].statusCode) + "</td><td>";
  logString += Log_StatusCodeToString(logBuffer[i].statusCode);
  logString += "</td></tr>";
  return logString;
}

String Log_BufferToString(int showNumber)
{
  int showed_number = 0;
  String logString;
  if (showNumber > 10)
  {
    logString  = "<html><head><title>SMGWLite - Log Buffer</title><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
    logString += String(HTML_STYLE);
    logString += "</head><body>";
  }
  logString += "<table border=1><tr><th>Index</th><th>Timestamp</th><th>Timestamp</th><th>Uptime</th><th>Statuscode</th><th>Status</th></tr>";

  // First loop: most recent entries, from logIndex down to 0
  for (int i = logIndex; i >= 0; i--)
  {
    logString += Log_EntryToString(i);
    showed_number++;
    if (showed_number >= showNumber)
      return logString + "</table>";
  }

  // Second loop: older entries that wrapped around, from buffer end to logIndex
  if (logIndex < LOG_BUFFER_SIZE - 1)
  {
    for (int i = LOG_BUFFER_SIZE - 1; i > logIndex; i--)
    {
      logString += Log_EntryToString(i);
      showed_number++;
      if (showed_number >= showNumber) break;
    }
  }
  return logString + "</table>";
}

void resetMeterValue(MeterValue &val)
{
  val = MeterValue{}; // reset all fields to zero
}

// ---------------------------------------------------------------------------
// MeterValues_clear_Buffer  (Written by Claude)
// Zeros the entire packed buffer and resets both write pointers.
// Called after all values have been successfully sent to the backend.
// ---------------------------------------------------------------------------
void MeterValues_clear_Buffer()
{
  if (!MeterValueBuffer) return; // nothing to clear if buffer was never allocated
  memset(MeterValueBuffer, 0, (size_t)Meter_Value_Buffer_Size * MeterValue_EntrySize());
  meter_value_override_i      = 0;
  meter_value_NON_override_i  = Meter_Value_Buffer_Size - 1;
  meter_value_buffer_overflow = false;
  meter_value_buffer_full     = false;
}

// ---------------------------------------------------------------------------
// MeterValue_Num
// Returns the number of values currently stored in the ring-buffer, based on
// the two write-pointer positions. Returns Meter_Value_Buffer_Size when the
// buffer has overflowed (all slots in use).
// ---------------------------------------------------------------------------
int MeterValue_Num()
{
  if (meter_value_buffer_full == true || meter_value_buffer_overflow == true)
    return Meter_Value_Buffer_Size;
  return (meter_value_override_i + ((Meter_Value_Buffer_Size - 1) - meter_value_NON_override_i));
}

// ---------------------------------------------------------------------------
// MeterValue_Num2
// Alternative count: iterates the entire buffer and counts non-empty slots.
// Slower but independent of the write-pointer logic — useful for debugging.
// ---------------------------------------------------------------------------
int MeterValue_Num2()
{
  int count = 0;
  for (int i = 0; i < Meter_Value_Buffer_Size; i++)
  {
    if (!MeterValue_slot_empty(i)) count++;
  }
  return count;
}

void Webserver_LocationHrefHome(int delay)
{
  String call = "<meta http-equiv='refresh' content='" + String(delay) + ";url=/'>";
  server.send(200, "text/html", call);
}

/**
 * SML CRC16 (X25) Algorithm
 * Initial: 0xFFFF, Poly: 0x1021 (reversed 0x8408), Final XOR: 0xFFFF
 */
uint16_t calculateSML_CRC16(uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 0x0001) crc = (crc >> 1) ^ 0x8408;
      else crc >>= 1;
    }
  }
  return crc ^ 0xFFFF;
}

/**
 * Helper: Searches for OBIS code and returns a table row
 */
String getObisRow(uint8_t* buffer, int prefix, int suffix, uint8_t* code, const char* label, const char* unit, float scaler) {
  for (size_t i = (size_t)prefix; i < (size_t)suffix - 12; i++) {
    if (memcmp(&buffer[i], code, 6) == 0) {
      for (int j = i + 6; j < i + 25; j++) {
        if (buffer[j] == 0x52) { // Scaler anchor found
          uint8_t typeByte = buffer[j + 2];
          int vLen   = (typeByte & 0x0F) - 1;
          int vStart = j + 3;
          int32_t raw = 0;
          for (int k = 0; k < vLen; k++) raw = (raw << 8) | buffer[vStart + k];
          // Sign correction for Active Power (W)
          if (vLen <= 4 && (buffer[vStart] & 0x80) && (strcmp(unit, "W") == 0)) {
            if (vLen == 1) raw -= 256;
            else if (vLen == 2) raw -= 65536;
          }
          return "<tr><td>" + String(label) + "</td>" +
                 "<td>" + String(vStart) + "</td>" +
                 "<td>" + String(vLen) + " Bytes</td>" +
                 "<td><strong>" + String(raw * scaler, 1) + " " + unit + "</strong></td></tr>";
        }
      }
    }
  }
  return ""; // Return empty if register is not found
}

String analyzeSML(uint8_t* buffer, size_t length) {
  // Inject predefined style and charset
  String s = "<meta charset='UTF-8'>";
  s += String(HTML_STYLE);
  s += "<title>SML Live Analysis</title>";
  s += "<div class='section'><h2>SML Live Analysis</h2>";

  // 1. Find Prefix and Suffix
  int px = -1;
  for (size_t i = 0; i < (int)length - 4; i++) {
    if (buffer[i] == 0x1b && buffer[i+1] == 0x1b && buffer[i+2] == 0x1b && buffer[i+3] == 0x1b) { px = i; break; }
  }
  int sx = -1;
  if (px != -1) {
    for (size_t i = px; i < (int)length - 5; i++) {
      if (buffer[i] == 0x1b && buffer[i+1] == 0x1b && buffer[i+2] == 0x1b && buffer[i+3] == 0x1b && buffer[i+4] == 0x1a) { sx = i; break; }
    }
  }
  if (px == -1 || sx == -1)
    return s + "<p><font color='red'><strong>Error:</strong> Telegram incomplete (Prefix/Suffix missing).</font></p>";

  // 2. CRC check
  int      crcLen  = (sx + 6) - px;
  uint16_t compCRC = calculateSML_CRC16(&buffer[px], crcLen);
  uint16_t recvCRC = (buffer[sx + 6] << 8) | buffer[sx + 7];
  bool     crcOk   = (compCRC == recvCRC);

  // 3. Status table
  s += "<h3>Telegram Status</h3>";
  s += "<table><tr><th>Parameter</th><th>Value</th></tr>";
  s += "<tr><td>Index (Start/End)</td><td>" + String(px) + " / " + String(sx) + "</td></tr>";
  s += "<tr><td>Payload Length</td><td>" + String(crcLen + 2) + " Bytes</td></tr>";
  s += "<tr><td>Checksum (CRC)</td><td>";
  if (recvCRC == 0)   s += "Meter sent no CRC (0x0000)";
  else if (crcOk)     s += "0x" + String(recvCRC, HEX) + " (Valid)";
  else                s += "<font color='red'>0x" + String(recvCRC, HEX) + " (Invalid! Expected: " + String(compCRC, HEX) + ")</font>";
  s += "</td></tr></table>";

  // 4. Data table
  s += "<h3>Registers</h3>";
  s += "<table><tr><th>OBIS Code</th><th>Index</th><th>Length</th><th>Reading</th></tr>";
  uint8_t obis180[] = {0x01, 0x00, 0x01, 0x08, 0x00, 0xff}; // Total Consumption
  uint8_t obis280[] = {0x01, 0x00, 0x02, 0x08, 0x00, 0xff}; // Total Delivery
  uint8_t obis167[] = {0x01, 0x00, 0x10, 0x07, 0x00, 0xff}; // Active Power
  s += getObisRow(buffer, px, sx, obis180, "Consumption (1.8.0)", "Wh", 0.1);
  s += getObisRow(buffer, px, sx, obis280, "Delivery (2.8.0)",    "Wh", 0.1);
  s += getObisRow(buffer, px, sx, obis167, "Active Power",        "W",  1.0);
  s += "</table></div>";
  return s;
}

void Webserver_SML_Analysis()
{
  server.send(200, "text/html", analyzeSML(telegram_receive_buffer, TELEGRAM_LENGTH));
}

void Webserver_MeterValue_Num2()
{
  String s = "<html><head><title>SMGWLite - Alternative Amount Meter Value</title> " + String(HTML_STYLE) + "</head><body>" + String(MeterValue_Num2()) + "</body></html>";
  server.send(200, "text/html", s);
}

// ---------------------------------------------------------------------------
// Webserver_ShowMeterValues  (Written by Claude)
// Renders the packed ring-buffer as an HTML table for debugging.
// Reads each slot using MeterValue_read() so it works regardless of which
// optional fields are currently enabled.
// ---------------------------------------------------------------------------
void Webserver_ShowMeterValues()
{
  String s;
  s.reserve(Meter_Value_Buffer_Size * 80 + 1024); // ~80 chars per data row + header
  s = "<html><head><title>SMGWLite - Meter Values</title>" + String(HTML_STYLE) + "</head><body>";

  // Show active fields and entry size so the table header always matches
  s += "<p>Entry size: " + String(MeterValue_EntrySize()) + " bytes | ";
  s += MeterValue_BuildFieldsParam() + "</p>";

  s += "<table border='1'><tr><th>Index</th><th>Count</th><th>Timestamp</th><th>Timestamp</th><th>Consumption (1.8.0)</th>";
  if (config_temperature_enabled) s += "<th>Temperature</th>";
  if (config_solar_enabled)       s += "<th>myStrom (solar)</th>";
  if (config_obis280_enabled)     s += "<th>Infeed (2.8.0)</th>";
  s += "</tr>";

  int count = 1;
  bool first = true;
  for (int m = 0; m < Meter_Value_Buffer_Size; m++)
  {
    uint32_t ts, m180, temp, solar, m280;
    MeterValue_read(m, ts, m180, temp, solar, m280);
    if (ts == 0 && m180 == 0) // skip empty slots
    {
      if (first) { first = false; s += "<tr><td>-----</td></tr>"; }
      continue;
    }
    s += "<tr><td>" + String(m) + "</td><td>" + String(count++) + "</td>";
    s += "<td>" + String(Time_formatTimestamp(ts)) + "</td><td>" + String(ts) + "</td>";
    s += "<td>" + String(m180) + "</td>";
    if (config_temperature_enabled) s += "<td>" + String(temp)  + "</td>";
    if (config_solar_enabled)       s += "<td>" + String(solar) + "</td>";
    if (config_obis280_enabled)     s += "<td>" + String(m280)  + "</td>";
    s += "</tr>";
  }
  s += "</table>";
  server.send(200, "text/html", s);
}

void Webserver_ShowLogBuffer()
{
  server.send(200, "text/html", Log_BufferToString());
}

void Webclient_splitHostAndPath(const String &url, String &host, String &path)
{
  int slashIndex = url.indexOf('/');
  if (slashIndex == -1) { host = url; path = "/"; }
  else { host = url.substring(0, slashIndex); path = url.substring(slashIndex); }
}

String backend_host;
String backend_path;

char FullCert[2000];

void Webserver_SetCert()
{
  server.send(200, "text/html", "<form action='/upload' method='POST'><textarea name='cert' rows='10' cols='80'>" + String(FullCert) + "</textarea><br><input type='submit'></form>");
}

void Webserver_TestBackendConnection()
{
  WiFiClientSecure client;
  client.setCACert(FullCert);
  String res = "<html><head><title>SMGWLite - Backend Test</title>" + String(HTML_STYLE) + "</head><body><div class='section'>";

  if (client.connect(backend_host.c_str(), 443))
  {
    res += "&#9989; Host reachable,<br>&#9989; Cert correct";
  }
  else
  {
    client.setInsecure(); // If cert not accepted, try without
    if (client.connect(backend_host.c_str(), 443))
      res += "&#9989; Host reachable<br>&#10060; Cert not working.";
    else
    {
      res += "Host not reachable.";
      server.send(200, "text/html", res);
      return;
    }
  }

  String url = String(backend_path) + "?backend_test=true&token=header&ID=" + String(backend_ID);
  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + String(backend_host) + "\r\n" +
               "X-Auth-Token: " + String(backend_token) + "\r\n" +
               "Connection: close\r\n\r\n");

  unsigned long timeout = millis();
  while (client.available() == 0)
  {
    if (millis() - timeout > 5000) { res += "<br>No response from server."; server.send(200, "text/html", res); return; }
  }

  String response = "";
  while (client.available()) response += client.readString();

  if (response.indexOf("200") != -1) res += "<br>&#9989; ID & Token valid.";
  else                                res += "<br>&#10060; ID & Token invalid!";

  res += "</div></body></html>";
  server.send(200, "text/html", res);
}

void Webserver_HandleCertUpload()
{
  if (server.hasArg("cert"))
  {
    String cert = server.arg("cert");
    File file = SPIFFS.open("/cert.pem", FILE_WRITE);
    if (file)
    {
      file.println(cert);
      file.close();
      Log_AddEntry(8002);
      Webserver_LocationHrefHome();
    }
    else
    {
      Log_AddEntry(8003);
      Webserver_LocationHrefHome();
      server.send(500, "text/plain", "Cannot Open File!");
    }
  }
  else
  {
    Log_AddEntry(8004);
    Webserver_LocationHrefHome();
  }
}

void Webclient_loadCertToChar()
{
  File file = SPIFFS.open("/cert.pem", FILE_READ);
  if (!file) { Log_AddEntry(8001); return; }
  size_t size = file.size();
  file.readBytes(FullCert, size);
  FullCert[size] = '\0';
  file.close();
}

void Webclient_Send_Meter_Values_to_backend_Task(void *pvParameters)
{
  if (xSemaphoreTake(Sema_Backend, portMAX_DELAY))
  {
    Webclient_send_meter_values_to_backend();
    xSemaphoreGive(Sema_Backend);
  }
  watermark_meter_buffer = uxTaskGetStackHighWaterMark(NULL);
  vTaskDelete(NULL); // delete task when finished
}

void Webclient_Send_Log_to_backend_Task(void *pvParameters)
{
  b_send_log_to_backend = false;
  if (xSemaphoreTake(Sema_Backend, portMAX_DELAY))
  {
    Webclient_send_log_to_backend();
    xSemaphoreGive(Sema_Backend);
  }
  watermark_log_buffer = uxTaskGetStackHighWaterMark(NULL);
  vTaskDelete(NULL); // delete task when finished
}

void Webserver_UrlConfig()
{
  server.on("/",                    Webserver_HandleRoot);
  server.on("/showTelegram",        Webserver_ShowTelegram);
  server.on("/showSMLAnalysis",     Webserver_SML_Analysis);
  server.on("/showTelegramRaw",     Webserver_ShowTelegram_Raw);
  server.on("/showLastMeterValue",  Webserver_ShowLastMeterValue);
  server.on("/showCert",            Webserver_ShowCert);
  server.on("/setCert",             Webserver_SetCert);
  server.on("/testBackendConnection", Webserver_TestBackendConnection);
  server.on("/showMeterValues",     Webserver_ShowMeterValues);
  server.on("/showLogBuffer",       Webserver_ShowLogBuffer);
  server.on("/MeterValue_Num2",     Webserver_MeterValue_Num2);

  server.on("/upload", [] { Webserver_HandleCertUpload(); Webclient_loadCertToChar(); });
  server.on("/config", [] { iotWebConf.handleConfig(); });
  server.on("/restart", [] { Webserver_LocationHrefHome(5); delay(100); ESP.restart(); });
  server.on("/resetLogBuffer", [] { Webserver_LocationHrefHome(); LogBuffer_reset(); });
  server.on("/StoreMeterValue", [] { Webserver_LocationHrefHome(); Log_AddEntry(1006); MeterValue_trigger_override = true; });
  server.on("/MeterValue_init_Buffer", [] { MeterValue_init_Buffer(); Webserver_LocationHrefHome(); });
  server.on("/sendboth_Task", [] { Webserver_LocationHrefHome(2); Webclient_Send_Meter_Values_to_backend_wrapper(); Webclient_Send_Log_to_backend_wrapper(); });
  server.on("/sendLog_Task", [] { Webserver_LocationHrefHome(2); Webclient_Send_Log_to_backend_wrapper(); });
  server.on("/sendMeterValues_Task", [] { Webserver_LocationHrefHome(2); Webclient_Send_Meter_Values_to_backend_wrapper(); });
  server.on("/setOffline", [] { wifi_connected = false; Webserver_LocationHrefHome(); });
  server.onNotFound([]() { iotWebConf.handleNotFound(); });

  // OTA update handler
  server.on("/update", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html",
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='update'><input type='submit' value='Update'></form>");
  });
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "Update Failed" : "Update Successful. Rebooting...");
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Update Start: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
      else Update.printError(Serial);
    }
  });
}

void telegramTask(void * pvParameters) {
  for(;;) {
    handle_Telegram_receive();
    vTaskDelay(pdMS_TO_TICKS(10));
    watermark_telegram = uxTaskGetStackHighWaterMark(NULL);
  }
}

void setup()
{
  Sema_Backend = xSemaphoreCreateMutex();
  LogBuffer_reset();
  Log_AddEntry(1001);
  Serial.begin(115200);
  mySerial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);
  DLOGLN();
  DLOGLN("Starting up...Hello!");

  Param_setup();
  cached_taf7_param          = max(1, atoi(taf7_param));
  cached_taf14_param         = max(1, atoi(taf14_param));
  cached_backend_call_minute = max(1, atoi(backend_call_minute));
  Led_update_Blink();
  Webserver_UrlConfig();
  OTA_setup();

  if (!SPIFFS.begin(true)) Log_AddEntry(8000);
  Webclient_loadCertToChar();
  Webclient_splitHostAndPath(String(backend_endpoint), backend_host, backend_path);

  // MeterValue_init_Buffer reads the feature flags from IotWebConf, so it must
  // be called after iotWebConf.init() (inside Param_setup) has restored them.
  MeterValue_init_Buffer();

  configTime(0, 0, "ptbnts1.ptb.de", "ptbtime1.ptb.de", "ptbtime2.ptb.de");
  Temp_sensors.begin();
  DLOG("Temp sensors found: ");
  DLOGLN(Temp_sensors.getDeviceCount());

  uint8_t mac[6];
  WiFi.macAddress(mac);
  uint16_t lastTwoBytes = (mac[4] << 8) | mac[5];
  // Spread backend call times across devices using the MAC address as a seed,
  // so not all devices on the same network call the server simultaneously.
  staticDelay = lastTwoBytes % 60;

  vTaskPrioritySet(NULL, 3);
  xTaskCreate(telegramTask, "TelegramBot", 2048, NULL, 0, NULL);
}

void Param_setup()
{
  groupTelegram.addItem(&activate_IEC_Parser_object);
  groupBackend.addItem(&backend_endpoint_object);
  groupBackend.addItem(&backend_ID_object);
  groupBackend.addItem(&backend_token_object);
  groupTaf.addItem(&taf7_b_object);
  groupTaf.addItem(&taf7_param_object);
  groupTaf.addItem(&taf14_b_object);
  groupTaf.addItem(&taf14_param_object);
  groupBackend.addItem(&backend_call_minute_object);
  groupTelegram.addItem(&Meter_Value_Buffer_Size_object);
  groupSys.addItem(&led_blink_object);
  groupDebug.addItem(&DebugSetOffline_object);
  groupDebug.addItem(&DebugFromOtherClient_object);
  groupDebug.addItem(&DebugMeterValueFromOtherClientIP_object);

  groupAdditionalMeter.addItem(&mystrom_PV_object);
  groupAdditionalMeter.addItem(&mystrom_PV_IP_object);
  groupAdditionalMeter.addItem(&temperature_object);
  // Buffer field checkboxes — grouped with the sensors they relate to.
  // Changing these triggers MeterValue_init_Buffer() via Param_configSaved().
  groupAdditionalMeter.addItem(&config_temperature_object);
  groupAdditionalMeter.addItem(&config_solar_object);
  groupAdditionalMeter.addItem(&config_280_object);

  groupBackend.addItem(&UseSslCert_object);

  iotWebConf.setStatusPin(STATUS_PIN);
  iotWebConf.setConfigPin(CONFIG_PIN);
  iotWebConf.addParameterGroup(&groupSys);
  iotWebConf.addParameterGroup(&groupTelegram);
  iotWebConf.addParameterGroup(&groupBackend);
  iotWebConf.addParameterGroup(&groupTaf);
  iotWebConf.addParameterGroup(&groupAdditionalMeter);
  iotWebConf.addParameterGroup(&groupDebug);

  iotWebConf.setConfigSavedCallback(&Param_configSaved);
  iotWebConf.getApTimeoutParameter()->visible = true;
  iotWebConf.skipApStartup();
  multipleWifiAddition.init();
  iotWebConf.init();
}

void OTA_setup()
{
  ArduinoOTA
    .onStart([]() {
      String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
      Serial.println("Start updating " + type);
    })
    .onEnd([]()   { Serial.println("\nEnd"); })
    .onProgress([](unsigned int progress, unsigned int total) { Serial.printf("Progress: %u%%\r", (progress / (total / 100))); })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if      (error == OTA_AUTH_ERROR)    Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR)   Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR)     Serial.println("End Failed");
    });
}

void Webclient_Send_Meter_Values_to_backend_wrapper()
{
  xTaskCreate(Webclient_Send_Meter_Values_to_backend_Task, "Send_Meter task", 8192, NULL, 2, NULL);
}

void Webclient_Send_Log_to_backend_wrapper()
{
  xTaskCreate(Webclient_Send_Log_to_backend_Task, "send log task", 8192, NULL, 2, NULL);
}

/**
 * Internal Helper: Extracting specific OBIS values
 * This function is written by Gemini3
 */
bool obisExtractor(uint8_t* buffer, int px, int sx, uint8_t* code, uint32_t* result) {
  for (int i = px; i < sx - 12; i++) {
    if (memcmp(&buffer[i], code, 6) == 0) {
      for (int j = i + 6; j < i + 40 && j < sx; j++) {
        if (buffer[j] == 0x52) {
          uint8_t typeByte  = buffer[j + 2];
          uint8_t typeGroup = typeByte & 0xF0;
          // Accept 0x5x (Signed) and 0x6x (Unsigned)
          if (typeGroup == 0x50 || typeGroup == 0x60) {
            int vLen   = (typeByte & 0x0F) - 1;
            int vStart = j + 3;
            if (vStart + vLen > sx || vLen <= 0 || vLen > 8) continue;
            uint64_t raw64 = 0;
            for (int k = 0; k < vLen; k++) raw64 = (raw64 << 8) | buffer[vStart + k];
            // Return the raw value without any scaling
            *result = (uint32_t)raw64;
            return true;
          }
        }
      }
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Protocol detection and unified parser  (Written by Claude)
//
// Both parsers now share the same signature:
//   bool Telegram_parse_*(uint8_t* buffer, size_t length)
//   Returns: true  = meter value successfully extracted into LastMeterValue
//            false = telegram not recognised or parse error
//
// Protocol detection heuristics:
//   SML: starts with 0x1b 0x1b 0x1b 0x1b (binary escape sequence)
//   IEC: starts with '/' (ASCII 0x2F) — IEC 62056-21 mode C identifier
//
// The detected protocol is stored in last_detected_protocol for display
// in the webserver "Telegram Parse Config" section.
// ---------------------------------------------------------------------------

// Tracks which protocol was last successfully detected
enum class TelegramProtocol { UNKNOWN, SML, IEC };
TelegramProtocol last_detected_protocol = TelegramProtocol::UNKNOWN;
TelegramProtocol prev_detected_protocol = TelegramProtocol::UNKNOWN;

// Returns a human-readable string for the detected protocol
String Telegram_protocol_to_string(TelegramProtocol p)
{
  switch (p) {
    case TelegramProtocol::SML: return "SML (detected)";
    case TelegramProtocol::IEC: return "IEC 62056-21 (detected)";
    default:                    return "Unknown (no valid telegram yet)";
  }
}

/**
 * SML parser — extracts OBIS 1.8.0 and optionally 2.8.0 into LastMeterValue.
 * This function is written by Gemini3, signature unified by Claude.
 */
bool Telegram_parse_SML(uint8_t* buffer, size_t length)
{
  // 1. Locate Prefix
  int px = -1;
  for (int i = 0; i < (int)length - 4; i++) {
    if (buffer[i] == 0x1b && buffer[i+1] == 0x1b && buffer[i+2] == 0x1b && buffer[i+3] == 0x1b) { px = i; break; }
  }
  if (px == -1) return false;

  // 2. Locate Suffix
  int sx = -1;
  for (int i = px; i < (int)length - 5; i++) {
    if (buffer[i] == 0x1b && buffer[i+1] == 0x1b && buffer[i+2] == 0x1b && buffer[i+3] == 0x1b && buffer[i+4] == 0x1a) { sx = i; break; }
  }
  if (sx == -1) return false;

  // 3. Extract OBIS Data
  uint8_t obis180[] = {0x01, 0x00, 0x01, 0x08, 0x00, 0xff};
  uint8_t obis280[] = {0x01, 0x00, 0x02, 0x08, 0x00, 0xff};
  uint32_t temp180 = 0, temp280 = 0;
  bool found180 = obisExtractor(buffer, px, sx, obis180, &temp180);
  bool found280 = obisExtractor(buffer, px, sx, obis280, &temp280);

  // Only update globals if the main consumption register was found.
  // OBIS 2.8.0 is optional — some meters don't transmit it.
  if (found180) {
    resetMeterValue(LastMeterValue);
    LastMeterValue.meter_value_180 = temp180;
    LastMeterValue.timestamp       = Time_getEpochTime();
    if (found280) LastMeterValue.meter_value_280 = temp280;
    return true;
  }
  return false;
}

/**
 * IEC 62056-21 parser — extracts OBIS 1.8.0 and optionally 2.8.0 into LastMeterValue.
 * This function is written by ChatGPT, extended and signature unified by Claude.
 */
bool Telegram_parse_IEC(uint8_t* buffer, size_t length)
{
  static char telegram_str[TELEGRAM_LENGTH + 1];
  size_t copy_len = (length <= TELEGRAM_LENGTH) ? length : TELEGRAM_LENGTH;
  memcpy(telegram_str, buffer, copy_len);
  telegram_str[copy_len] = '\0';

  // Extract OBIS 1.8.0 (consumption) — required
  const char *obis180 = strstr(telegram_str, "1-0:1.8.0");
  if (!obis180) return false;

  const char *openParen = strchr(obis180, '(');
  const char *star      = (openParen) ? strchr(openParen, '*') : nullptr;
  if (!openParen || !star || openParen > star) return false;

  char valueStr[16];
  size_t len = star - openParen - 1;
  if (len >= sizeof(valueStr)) return false;

  strncpy(valueStr, openParen + 1, len);
  valueStr[len] = '\0';
  for (int i = 0; valueStr[i]; i++) if (valueStr[i] == ',') valueStr[i] = '.';

  float kWh180 = atof(valueStr);
  if (kWh180 <= 0.0f) return false; // implausible value

  resetMeterValue(LastMeterValue);
  LastMeterValue.meter_value_180 = (uint32_t)(kWh180 * 10000.0f);
  LastMeterValue.timestamp       = Time_getEpochTime();

  // Extract OBIS 2.8.0 (feed-in / delivery) — optional
  const char *obis280 = strstr(telegram_str, "1-0:2.8.0");
  if (obis280)
  {
    const char *p280 = strchr(obis280, '(');
    const char *s280 = (p280) ? strchr(p280, '*') : nullptr;
    if (p280 && s280 && p280 < s280)
    {
      char v280[16];
      size_t l280 = s280 - p280 - 1;
      if (l280 < sizeof(v280))
      {
        strncpy(v280, p280 + 1, l280);
        v280[l280] = '\0';
        for (int i = 0; v280[i]; i++) if (v280[i] == ',') v280[i] = '.';
        LastMeterValue.meter_value_280 = (uint32_t)(atof(v280) * 10000.0f);
      }
    }
  }

  return true;
}


void myStrom_get_Meter_value()
{
  if (!mystrom_PV_object.isChecked()) { LastMeterValue.solar = 0; return; }

  DLOGLN(F("myStrom_get_Meter_value Connecting..."));
  WiFiClient client;
  client.setTimeout(1000);
  if (!client.connect(mystrom_PV_IP, 80)) { Log_AddEntry(5000); DLOGLN(F("myStrom_get_Meter_value Connection failed")); LastMeterValue.solar = -1; return; }

  DLOGLN(F("myStrom_get_Meter_value Connected!"));
  client.println(F("GET /report HTTP/1.0"));
  client.print(F("Host: ")); client.println(mystrom_PV_IP);
  client.println(F("Connection: close"));
  if (client.println() == 0) { Log_AddEntry(5001); client.stop(); LastMeterValue.solar = -2; return; }

  char status[32] = {0};
  client.readBytesUntil('\r', status, sizeof(status));
  if (strcmp(status + 9, "200 OK") != 0) { client.stop(); LastMeterValue.solar = -3; return; }

  char endOfHeaders[] = "\r\n\r\n";
  if (!client.find(endOfHeaders)) { client.stop(); LastMeterValue.solar = -4; return; }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, client);
  if (error) { Log_AddEntry(5002); client.stop(); LastMeterValue.solar = -5; return; }

  LastMeterValue.temperature = doc["temperature"].as<float>() * 100;
  LastMeterValue.solar       = doc["energy_since_boot"].as<int>();
  client.stop();
}

int32_t MeterValue_get_from_remote()
{
  DLOGLN("MeterValue_get_from_remote Connecting...");
  WiFiClient client;
  client.setTimeout(2000);
  if (!client.connect(DebugMeterValueFromOtherClientIP, 80)) { DLOGLN(F("Connection failed")); return -1; }

  DLOGLN(F("Connected!"));
  client.println(F("GET /showLastMeterValue HTTP/1.0"));
  client.print(F("Host: ")); client.println(F(DebugMeterValueFromOtherClientIP));
  client.println(F("Connection: close"));
  client.println();
  DLOGLN(F("Request sent"));

  String fullResponse = "";
  unsigned long startTime = millis();
  while (client.connected() || client.available())
  {
    if (client.available()) fullResponse += (char)client.read();
    else
    {
      if (millis() - startTime > 5000) { DLOGLN(F("Timeout while reading response")); break; }
      delay(10);
    }
  }

  int bodyIndex = fullResponse.indexOf("\r\n\r\n");
  if (bodyIndex == -1) { DLOGLN(F("No HTTP body found")); return -2; }

  String body = fullResponse.substring(bodyIndex + 4);
  body.trim();

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, body);
  if (error) { DLOG(F("deserializeJson() failed: ")); DLOGLN(error.f_str()); return -3; }

  int32_t meter_value_180_i32 = doc["meter_value_180"] | -4;
  int32_t timestamp           = doc["timestamp"] | 0;

  DLOG(F("Meter Value 180: ")); DLOGLN(meter_value_180_i32);
  DLOG(F("Timestamp: "));      DLOGLN(timestamp);

  resetMeterValue(LastMeterValue);
  LastMeterValue.meter_value_180 = doc["meter_value_180"];
  LastMeterValue.timestamp       = doc["timestamp"];
  LastMeterValue.temperature     = doc["temperature"];
  LastMeterValue.solar           = doc["solar"];
  client.stop();
  timestamp_telegram = timestamp;
  return meter_value_180_i32;
}

void Telegram_ResetReceiveBuffer()
{
  telegram_receive_bufferIndex = 0;
  readingExtraBytes = false;
  extraIndex = 0;
}

void handle_Telegram_receive()
{
  if (DebugFromOtherClient_object.isChecked())
  {
    if (last_remote_meter_value + 5000 < millis())
    {
      last_remote_meter_value = millis();
      MeterValue_get_from_remote();
    }
    return;
  }

  while (mySerial.available() > 0)
  {
    uint8_t incomingByte = mySerial.read();
    lastByteTime = millis();
    if (telegram_receive_bufferIndex < TELEGRAM_LENGTH)
      telegram_receive_buffer[telegram_receive_bufferIndex++] = incomingByte;
    else
    {
      Log_AddEntry(3001);
      Telegram_ResetReceiveBuffer();
      continue;
    }
  }

  if (telegram_receive_bufferIndex > 0 && (millis() - lastByteTime > TELEGRAM_TIMEOUT_MS))
  {
    // Try SML parser first. If it finds the SML prefix+suffix, it is
    // authoritative and IEC is not attempted.
    // If SML finds no valid frame, fall through to the IEC parser.
    bool parsed = Telegram_parse_SML(telegram_receive_buffer, telegram_receive_bufferIndex);
    if (parsed)
    {
      last_detected_protocol = TelegramProtocol::SML;
    }
    else
    {
      parsed = Telegram_parse_IEC(telegram_receive_buffer, telegram_receive_bufferIndex);
      if (parsed) last_detected_protocol = TelegramProtocol::IEC;
    }
    if(last_detected_protocol != prev_detected_protocol){
      if(last_detected_protocol == TelegramProtocol::SML) Log_AddEntry(3003);
      else if(last_detected_protocol == TelegramProtocol::IEC) Log_AddEntry(3004);

      prev_detected_protocol = last_detected_protocol;
    }
    if (parsed && temperature_object.isChecked())
      LastMeterValue.temperature = current_temperature;

    Telegram_ResetReceiveBuffer();
  }
}

void Webclient_send_log_to_backend()
{
  DLOGLN("Send Log to Backend");
  Log_AddEntry(1019);
  WiFiClientSecure client;
  client.setHandshakeTimeout(10); // 10 s SSL handshake timeout (takes seconds)
  if (UseSslCert_object.isChecked()) client.setCACert(FullCert);
  else client.setInsecure();

  if (!client.connect(backend_host.c_str(), 443)) { DLOGLN("Connection to server failed"); Log_AddEntry(4000); return; }

  size_t logBufferSize = LOG_BUFFER_SIZE * sizeof(LogEntry);
  uint8_t *logDataBuffer = (uint8_t *)malloc(logBufferSize);
  if (!logDataBuffer) { DLOGLN("Log buffer allocation failed"); return; }
  memcpy(logDataBuffer, logBuffer, logBufferSize);

  String logHeader  = "POST " + String(backend_path) + "log.php";
  logHeader += "?ID=" + String(backend_ID) + "&token=header&IP=" + String(IPlastOctet);
  logHeader += " HTTP/1.1\r\nHost: " + backend_host + "\r\n";
  logHeader += "X-Auth-Token: " + String(backend_token) + "\r\n";
  logHeader += "Content-Type: application/octet-stream\r\n";
  logHeader += "Content-Length: " + String(logBufferSize) + "\r\n";
  logHeader += "Connection: close\r\n\r\n";
  DLOGLN(logHeader);

  client.print(logHeader);
  client.write(logDataBuffer, logBufferSize);
  free(logDataBuffer);

  unsigned long log_deadline = millis() + 15000;
  while ((client.connected() || client.available()) && millis() < log_deadline)
  {
    if (client.available())
    {
      String line = client.readStringUntil('\n');
      DLOGLN(line);
      if (line.startsWith("HTTP/1.1 200")) { DLOGLN("Log successfully sent"); Log_AddEntry(1020); b_send_log_to_backend = false; break; }
      else b_send_log_to_backend = true;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  client.stop();
}

// ---------------------------------------------------------------------------
// Webclient_send_meter_values_to_backend  (Written by Claude)
// Sends the entire packed ring-buffer to the PHP backend in a single POST.
//
// Wire format declaration:
//   The URL parameter "fields=ts,m180[,temp][,solar][,m280]" tells the backend
//   exactly which fields are present in each binary entry and in what order.
//   This replaces the old separate PV_included / 280_included flags and is
//   self-describing — adding a new field in the future only requires appending
//   a new token here and in the PHP parser.
//
// The buffer is already in the correct wire format, so it is sent directly
// without any intermediate copy.
// ---------------------------------------------------------------------------
void Webclient_send_meter_values_to_backend()
{
  Log_AddEntry(1005);
  Log_AddEntry(MeterValue_Num());
  DLOGLN("call_backend_V2");
  last_call_backend = millis();

  if (MeterValue_Num() == 0) { DLOGLN("Zero Values to transmit"); Log_AddEntry(0); call_backend_successfull = true; return; }

  call_backend_successfull = false;

  WiFiClientSecure client;
  client.setTimeout(10000); // 10 s read timeout for readStringUntil()
  if (UseSslCert_object.isChecked()) client.setCACert(FullCert);
  else client.setInsecure();

  if (!client.connect(backend_host.c_str(), 443, 10000)) { DLOGLN("Connection to server failed"); Log_AddEntry(4000); return; }

  // Send the full buffer including empty (zero) slots in the middle.
  // The backend skips slots where meter_value_180 == 0, so the wire format
  // is always Meter_Value_Buffer_Size * entrySize bytes regardless of how
  // many slots are actually filled.
  size_t bufferSize = (size_t)Meter_Value_Buffer_Size * MeterValue_EntrySize();

  String header  = "POST " + String(backend_path);
  header += "?ID=" + String(backend_ID);
  header += "&token=header";
  header += "&uptime=" + String(millis() / 60000);
  header += "&time=" + String(Time_getFormattedTime());
  // Self-describing field manifest — the backend uses this to parse the binary payload.
  // Format: fields=ts,m180[,temp][,solar][,m280]
  header += "&" + MeterValue_BuildFieldsParam();
  header += "&heap=" + String(ESP.getFreeHeap());
  header += "&transmittedValues=" + String(MeterValue_Num());
  header += "&IP=" + String(IPlastOctet);
  header += " HTTP/1.1\r\n";
  header += "Host: " + backend_host + "\r\n";
  header += "X-Auth-Token: " + String(backend_token) + "\r\n";
  header += "Content-Type: application/octet-stream\r\n";
  header += "Content-Length: " + String(bufferSize) + "\r\n";
  header += "Connection: close\r\n\r\n";

  client.print(header);
  client.write(MeterValueBuffer, bufferSize);

  unsigned long mv_deadline = millis() + 15000;
  while ((client.connected() || client.available()) && millis() < mv_deadline)
  {
    if (client.available())
    {
      String line = client.readStringUntil('\n');
      DLOGLN(line);
      if (line.startsWith("HTTP/1.1 200"))
      {
        DLOGLN("MeterValues successfully sent");
        call_backend_successfull = true;
        MeterValues_clear_Buffer();
        last_call_backend = millis();
        Log_AddEntry(1021);
        break; // don't wait for the rest of the response — we have what we need
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  client.stop();
}

// ---------------------------------------------------------------------------
// MeterValue_store  (Written by Claude)
// Stores LastMeterValue into the packed ring-buffer.
//
// Ring-buffer strategy (unchanged from original):
//   override=true  (TAF7,  high-priority): writes ascending from index 0 upward.
//   override=false (TAF14, low-priority):  writes descending from the last index.
// This ensures TAF7 values always have space and are never overwritten by TAF14.
//
// The only structural change vs. the original: instead of assigning a struct
// slot directly (MeterValues[i] = LastMeterValue), we call MeterValue_write()
// which packs only the enabled fields into the byte buffer.
// Bugfix: the original non-override wrap-around check tested meter_value_override_i
// instead of meter_value_NON_override_i — corrected here.
// ---------------------------------------------------------------------------
bool MeterValue_store(bool override)
{
  if (ESP.getFreeHeap() < 1000) { Log_AddEntry(1015); DLOGLN("Not enough free heap to store another value"); return false; }

  if (mystrom_PV_object.isChecked()) myStrom_get_Meter_value();

  if (LastMeterValue.meter_value_180 <= 0) { Log_AddEntry(1200); return false; }

  // Skip storing if the value has not changed since the last successful store.
  // Time thresholds differ: TAF14 (non-override) waits 15 min,
  // TAF7 (override) only 1 min, to capture short power spikes.
  if (((override == false && millis() - last_meter_value_successful < 900000) ||
       (override == true  && millis() - last_meter_value_successful < 60000))
    && LastMeterValue.meter_value_180 == PrevMeterValue.meter_value_180
    && LastMeterValue.meter_value_280 == PrevMeterValue.meter_value_280
    && LastMeterValue.solar           == PrevMeterValue.solar)
  {
    Log_AddEntry(1201);
    return false;
  }

  // Select write index based on priority:
  //   override (TAF7)      -> ascending from front
  //   non-override (TAF14) -> descending from back
  int write_i = override ? meter_value_override_i : meter_value_NON_override_i;
  DLOGLN("where to write: " + String(write_i));

  // Check if the target slot is still empty — empty means free space remains
  meter_value_buffer_full = !MeterValue_slot_empty(write_i);

  if (override == true || meter_value_buffer_full == false)
  {
    // Write the current reading into the packed buffer at the selected slot.
    // Fields that are disabled (temperature, solar, obis280) are silently
    // skipped inside MeterValue_write() — they consume no bytes.
    MeterValue_write(write_i,
      LastMeterValue.timestamp,
      LastMeterValue.meter_value_180,
      LastMeterValue.temperature,
      LastMeterValue.solar,
      LastMeterValue.meter_value_280
    );

    if (override)
    {
      // TAF7: advance write pointer upward; wrap around on overflow.
      // Intentionally overwrites TAF14 data when the buffer is full —
      // TAF7 (timed snapshots) has higher priority than TAF14 (interval readings).
      meter_value_override_i++;
      if (meter_value_override_i >= Meter_Value_Buffer_Size)
      {
        meter_value_override_i = 0;
        meter_value_buffer_overflow = true;
      }
    }
    else
    {
      // TAF14: advance write pointer downward; wrap around on underflow.
      // Bugfix: original code checked meter_value_override_i here by mistake.
      meter_value_NON_override_i--;
      if (meter_value_NON_override_i < 0)
      {
        meter_value_NON_override_i = Meter_Value_Buffer_Size - 1;
        meter_value_buffer_overflow = true;
      }
    }
  }
  else
  {
    Log_AddEntry(1016);
    MeterValue_trigger_non_override = false; // prevent immediate re-trigger
    DLOGLN("Buffer full, no space to write new value!");
    return false;
  }

  PrevMeterValue = LastMeterValue; // remember last stored value for change detection
  return true;
}

void handle_check_wifi_connection()
{
  wl_status_t current_wifi_status = WiFi.status();
  if (DebugSetOffline_object.isChecked()) current_wifi_status = WL_CONNECTION_LOST;

  if (millis() - last_wifi_check > 500)
  {
    last_wifi_check = millis();

    if (current_wifi_status == WL_CONNECTED && wifi_connected)
    {
      // Still connected — nothing to do
    }
    else if (current_wifi_status == WL_CONNECTED && !wifi_connected)
    {
      Log_AddEntry(1008);
      DLOGLN("Connection has returned: Resetting Backend Timer, starting OTA");
      ArduinoOTA.begin();
      wifi_connected         = true;
      wifi_reconnection_time = millis();
      call_backend_successfull = false;
      b_send_log_to_backend  = true; // send after the 60 s reconnect delay, not immediately
      IPAddress localIP = WiFi.localIP();
      IPlastOctet = localIP[3];
    }
    else if (current_wifi_status != WL_CONNECTED && wifi_connected)
    {
      Log_AddEntry(1009);
      wifi_connected = false;
    }
    else
    {
      // Still offline
    }
  }
}

void handle_temperature()
{
  if (temperature_object.isChecked())
  {
    if (read_temperature == true && millis() - last_temperature > 20000)
    {
      last_temperature = millis();
      Temp_sensors.requestTemperatures();
      read_temperature = false;
    }
    else if (read_temperature == false && millis() - last_temperature > 1000)
    {
      last_temperature = millis();
      float raw_temp = Temp_sensors.getTempCByIndex(0) * 100;
      if (raw_temp > -10000) // filter out -127°C sensor error (-12700 in raw units)
        current_temperature = (int)raw_temp;
      read_temperature = true;
    }
  }
}

void handle_call_backend()
{
  if (wifi_connected && millis() - wifi_reconnection_time > 60000)
  {
    if ((!call_backend_successfull && millis() - last_call_backend > 30000) ||
        ((Time_getMinutes()) % cached_backend_call_minute == 0 &&
          Time_getEpochTime() % 60 > staticDelay && // device-individual delay to stagger calls
          millis() - last_call_backend > 60000))
    {
      last_call_backend = millis(); // prevent loop() from spawning another task before this one starts
      Webclient_Send_Meter_Values_to_backend_wrapper();
      if (b_send_log_to_backend == true) Webclient_Send_Log_to_backend_wrapper();
    }
  }
}

// void dynTaf()
// {
//   int dT = LastMeterValue.timestamp - PrevMeterValue.timestamp;
//   if (dT > 0 && LastMeterValue.meter_value_180 > PrevMeterValue.meter_value_180)
//   {
//     currentPower = (float)(360 * (LastMeterValue.meter_value_180 - PrevMeterValue.meter_value_180)) / (dT);
//     if (currentPower > LastPower * int(tafdyn_multiplicator) ||
//         currentPower < LastPower / int(tafdyn_multiplicator) ||
//         abs(currentPower - LastPower) >= int(tafdyn_absolute))
//     {
//       MeterValue_store(false);
//       Log_AddEntry(1018);
//     }
//     LastPower = currentPower;
//   }
// }

unsigned long last_meter_value_store   = 0;
unsigned long last_meter_value_trigger = 0;

void handle_MeterValue_store()
{
  if (!MeterValue_trigger_override && !MeterValue_trigger_non_override) return; // nothing to do
  if (millis() - last_meter_value_store < 1000) return;
  last_meter_value_store = millis();

  bool retVal = false;
  if (MeterValue_trigger_override == true)
  {
    retVal = MeterValue_store(true);
    if (retVal == true) last_taf7_meter_value = millis();
  }
  else if (MeterValue_trigger_non_override == true)
  {
    retVal = MeterValue_store(false);
    if (retVal == true) last_taf14_meter_value = millis();
  }

  if (retVal == true)
  {
    Log_AddEntry(1017);
    last_taf14_meter_value      = millis();
    last_meter_value_successful = millis();
    MeterValue_trigger_override     = false;
    MeterValue_trigger_non_override = false;
  }
}

void handle_MeterValue_trigger()
{
  if (MeterValue_trigger_override == false &&
      taf7_b_object.isChecked() &&
      ((Time_getEpochTime() - 1) % ((unsigned long)cached_taf7_param * 60) < 15) &&
      (millis() - last_taf7_meter_value > 45000) &&
      (millis() - last_meter_value_successful >= 20000))
  {
    Log_AddEntry(1010);
    MeterValue_trigger_override     = true;
    MeterValue_trigger_non_override = false;
  }
  else if (MeterValue_trigger_override == false &&
           MeterValue_trigger_non_override == false &&
           taf14_b_object.isChecked() &&
           millis() - last_meter_value_successful >= 1000UL * (unsigned long)cached_taf14_param &&
           millis() - last_taf14_meter_value      >= 1000UL * (unsigned long)cached_taf14_param)
  {
    if (meter_value_buffer_full == true) { last_taf14_meter_value = millis(); Log_AddEntry(1206); }
    else { Log_AddEntry(1011); MeterValue_trigger_non_override = true; }
  }
  // if (tafdyn_b_object.isChecked()) { dynTaf(); }
}

void loop()
{
  iotWebConf.doLoop();
  ArduinoOTA.handle();
  handle_temperature();
  // handle_Telegram_receive();  // handled by dedicated FreeRTOS task
  handle_check_wifi_connection();
  handle_MeterValue_trigger();
  handle_MeterValue_store();
  handle_call_backend();
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

String Time_formatUptime()
{
  int64_t uptimeMicros  = esp_timer_get_time();       // time in microseconds
  int64_t uptimeMillis  = uptimeMicros / 1000;        // convert to milliseconds
  int64_t uptimeSeconds = uptimeMillis / 1000;        // convert to seconds

  // Calculate days, hours, minutes, seconds
  int days    = uptimeSeconds / 86400; uptimeSeconds %= 86400;
  int hours   = uptimeSeconds / 3600;  uptimeSeconds %= 3600;
  int minutes = uptimeSeconds / 60;
  int seconds = uptimeSeconds % 60;

  char buffer[20];
  sprintf(buffer, "%02dd %02dh%02dm%02ds", days, hours, minutes, seconds);
  return String(buffer);
}

void Webserver_HandleRoot()
{
  if (iotWebConf.handleCaptivePortal()) return;

  String s;
  s.reserve(8000);
  s += R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no" />
  <title>)rawliteral";
  s += thingName;
  s += R"rawliteral(</title>)rawliteral";
  s += HTML_STYLE;
  s += R"rawliteral(</head><body>
<p>Go to <a href='config'><b>configuration page</b></a> to change <i>italic</i> values.</p>

<h2>Last Meter Value</h2>
<p><small>Green = included in wire format</small></p>
<table>)rawliteral";
  {
    // W = in wire (light green), N = not in wire
    const char* W = " style='background:#e6ffe6'";
    const char* N = "";
    const char* w280  = config_obis280_enabled     ? W : N;
    const char* wTemp = config_temperature_enabled ? W : N;
    const char* wSol  = config_solar_enabled        ? W : N;
    s += String("<tr><th") + N    + ">Time</th>"
       + "<th" + N    + ">Consumption (1.8.0)</th>"
       + "<th" + N  + ">Infeed (2.8.0)</th>"
       + "<th" + N + ">Temperature</th>"
       + "<th" + N  + ">MyStrom (solar)</th></tr>";
    s += String("<tr><td") + W    + ">" + String(Time_getEpochTime() - LastMeterValue.timestamp) + " s ago</td>"
       + "<td" + W    + ">" + String(LastMeterValue.meter_value_180) + "</td>"
       + "<td" + w280  + ">" + String(LastMeterValue.meter_value_280) + "</td>"
       + "<td" + wTemp + ">" + String(LastMeterValue.temperature / 100.0) + " \xc2\xb0""C</td>"
       + "<td" + wSol  + ">" + String(LastMeterValue.solar) + "</td></tr>";
  }
  s += R"rawliteral(</table>

<p><a href='StoreMeterValue'>Store Meter Value Now (Taf6)</a></p>

<h3>Meter Value Buffer</h3>
<ul>
  <li>Used / Size: )rawliteral";
  s += String(MeterValue_Num()) + " / " + String(Meter_Value_Buffer_Size);
  {
    int cfgKB = atoi(Meter_Value_Buffer_Size_Char);
    size_t budget = (cfgKB <= 0) ? BUFFER_REFERENCE_BYTES : (size_t)cfgKB * 1024;
    s += meter_value_buffer_is_auto
         ? " (auto &mdash; " + String(BUFFER_REFERENCE_BYTES / 1024) + " KB reference budget)"
         : " (manual &mdash; " + String(cfgKB) + " KB budget)";
    s += "<br><small>Budget: " + String(budget) + " bytes &nbsp;|&nbsp; "
         + String(MeterValue_EntrySize()) + " bytes/slot &nbsp;&rarr;&nbsp; "
         + String(MeterValue_slots_from_budget(budget)) + " slots</small>";
  }
  s += R"rawliteral(</li>
  <li>Slots with current budget: )rawliteral";
  {
    // Calculate maximum possible slots for this field configuration given
    // the current free heap. Shown as a capacity reference for the user.
    int maxSlots = MeterValue_calc_max_slots_for_display();
    int maxMinutes_taf7  = (maxSlots > 0 && atoi(taf7_param)  > 0) ? (maxSlots * atoi(taf7_param))  : 0;
    int maxMinutes_taf14 = (maxSlots > 0 && atoi(taf14_param) > 0) ? (maxSlots * atoi(taf14_param) / 60) : 0;
    s += String(maxSlots) + " slots";
    { int d = maxMinutes_taf7 / 1440, h = (maxMinutes_taf7 % 1440) / 60, m = maxMinutes_taf7 % 60;
      s += " &nbsp;|&nbsp; TAF7: ~";
      if (d > 0) s += String(d) + "d";
      s += String(h) + "h" + String(m) + "min"; }
    { int d = maxMinutes_taf14 / 1440, h = (maxMinutes_taf14 % 1440) / 60, m = maxMinutes_taf14 % 60;
      s += " &nbsp;|&nbsp; TAF14: ~";
      if (d > 0) s += String(d) + "d";
      s += String(h) + "h" + String(m) + "min"; }
  }
  s += R"rawliteral(</li>
  <li>Wire format: )rawliteral";
  s += MeterValue_BuildFieldsParam();
  s += R"rawliteral(</li>
  <li>Entry size: )rawliteral";
  s += String(MeterValue_EntrySize()) + " bytes";
  s += R"rawliteral(</li>
  <li>Free heap: )rawliteral";
  s += String(ESP.getFreeHeap() / 1024) + " KB  (buffer uses " + String((size_t)Meter_Value_Buffer_Size * MeterValue_EntrySize() / 1024) + " KB)";
  s += R"rawliteral(</li>
  <li>Temperature in buffer: )rawliteral";
  s += String(config_temperature_enabled ? "yes" : "no");
  s += R"rawliteral(</li>
  <li>MyStrom / Solar in buffer: )rawliteral";
  s += String(config_solar_enabled ? "yes" : "no");
  s += R"rawliteral(</li>
  <li>Infeed (2.8.0) in buffer: )rawliteral";
  s += String(config_obis280_enabled ? "yes" : "no");
  s += R"rawliteral(</li>
  <li>i override: )rawliteral";
  s += String(meter_value_override_i);
  s += R"rawliteral(</li>
  <li>i non override: )rawliteral";
  s += String(meter_value_NON_override_i);
  s += R"rawliteral(</li>
  <li>Buffer Overflow: )rawliteral";
  s += String(meter_value_buffer_overflow);
  s += R"rawliteral(</li>
  <li>Buffer Full: )rawliteral";
  s += String(meter_value_buffer_full);
  s += R"rawliteral(</li>
  <li><a href='MeterValue_Num2'>Calculate # Meter Values (alternative)</a></li>
  <li><a href='showMeterValues'>Show Meter Values</a></li>
)rawliteral";

  {
    // In auto mode (config value == 0) the buffer size is calculated from the
    // heap at init time, so it will never equal the config value of 0.
    // Only show the "buffer size changed" warning when the user has set a
    // manual slot count AND it differs from what is currently allocated.
    // In KB-budget mode there is no "size changed" scenario: the slot count
    // always matches the budget / entry_size. The warning is only relevant if
    // the user switches between auto and manual, which triggers a re-init via
    // Param_configSaved() automatically. So we never need to show it.
    bool isAutoMode  = (atoi(Meter_Value_Buffer_Size_Char) <= 0);
    bool sizeChanged = false; // budget-based sizing always matches

    if (sizeChanged)
    {
      s += "<li><font color='red'>Buffer Size changed, please ";
      if (MeterValue_Num() > 0)
        s += "<a href='sendMeterValues_Task'>Send Meter Values to Backend</a> to not lose (" + String(MeterValue_Num()) + ") values and ";
      s += "<a href='MeterValue_init_Buffer'>Re-Init Meter Value Buffer</a></font></li>\n";
    }
    else
    {
      s += "<li><a href='MeterValue_init_Buffer'>Re-Init Meter Value Buffer</a></li>\n";
    }
  }

  s += R"rawliteral(  <li><a href='showLastMeterValue'>Show Last Meter Value (JSON)</a></li>
</ul>

<h3>Telegram Parse Config</h3>
<ul>
  <li><i>Protocol (auto-detected):</i> )rawliteral";
  s += Telegram_protocol_to_string(last_detected_protocol);
  s += R"rawliteral(</li>
  <li><a href='showTelegram'>Show Telegram</a> (<a href='showTelegramRaw'>Raw</a>)</li>
  <li><a href='showSMLAnalysis'>Show SML Analysis</a></li>
</ul>

<div class="section">
<h3>Backend Config</h3>
<ul>
  <li><i>Backend Endpoint:</i> )rawliteral";
  s += backend_endpoint;
  s += R"rawliteral(</li>
  <li>Host: )rawliteral";
  s += backend_host;
  s += R"rawliteral(</li>
  <li>Path: )rawliteral";
  s += backend_path;
  s += R"rawliteral(</li>
  <li><i>Backend Call Minute:</i> )rawliteral";
  s += String(atoi(backend_call_minute));
  s += R"rawliteral(</li>
  <li><i>Backend ID:</i> )rawliteral";
  s += backend_ID;
  s += R"rawliteral(</li>
  <li><i>Use SSL Cert:</i> )rawliteral";
  s += (UseSslCert_object.isChecked() ? "true" : "false");
  s += R"rawliteral(</li>
  <li><a href='showCert'>Show Cert</a></li>
  <li><a href='setCert'>Set Cert</a></li>
  <li><a href='testBackendConnection'>Test Backend Connection</a></li>
  <li>Last Backend Call ago (min): )rawliteral";
  s += String((millis() - last_call_backend) / 60000);
  s += R"rawliteral(</li>
  <li>Static Delay: )rawliteral";
  s += String(staticDelay);
  s += R"rawliteral( s</li>
  <li><a href='sendLog_Task'>Send Log Files to backend</a></li>
  <li><a href='sendMeterValues_Task'>Send Meter Values to backend</a></li>
  <li><a href='sendboth_Task'>Send Both to backend</a></li>
</ul>
</div>

<h3>Taf Config</h3>
<ul>
  <li><i>Taf 7:</i> )rawliteral";
  s += (taf7_b_object.isChecked() ? "activated" : "not activated");
  s += R"rawliteral(</li>
  <li><i>Taf7 minute param:</i> )rawliteral";
  s += String(atoi(taf7_param));
  s += R"rawliteral(</li>
  <li><i>Taf 14:</i> )rawliteral";
  s += (taf14_b_object.isChecked() ? "activated" : "not activated");
  s += R"rawliteral(</li>
  <li><i>Taf14 Interval:</i> )rawliteral";
  s += String(atoi(taf14_param));
  s += R"rawliteral(</li>
</ul>

<h3>Additional Meters</h3>
<ul>
  <li><i>Temperature Sensor:</i> )rawliteral";
  s += (temperature_object.isChecked() ? "activated" : "deactivated");
  s += R"rawliteral(</li>
  <li><i>MyStrom (solar):</i> )rawliteral";
  s += (mystrom_PV_object.isChecked() ? "activated" : "deactivated");
  s += R"rawliteral(</li>
  <li><i>MyStrom IP:</i> )rawliteral";
  s += mystrom_PV_IP;
  s += R"rawliteral(</li>
</ul>

<h3>Debug Helpers</h3>
<ul>
  <li><i>Set Device Offline:</i> )rawliteral";
  s += (DebugSetOffline_object.isChecked() ? "activated" : "deactivated");
  s += R"rawliteral(</li>
  <li><i>Get Values from other SMGWLite:</i> )rawliteral";
  s += (DebugFromOtherClient_object.isChecked() ? "activated" : "deactivated");
  s += R"rawliteral(</li>
  <li><i>Remote Client IP:</i> )rawliteral";
  s += String(DebugMeterValueFromOtherClientIP);
  s += R"rawliteral(</li>
</ul>

<h3>System Info</h3>
<ul>
  <li><i>LED blink:</i> )rawliteral";
  s += (led_blink_object.isChecked() ? "activated" : "deactivated");
  s += R"rawliteral(</li>
  <li>Watermark Main Task: )rawliteral";
  s += String(uxTaskGetStackHighWaterMark(NULL));
  s += R"rawliteral(</li>
  <li>Watermark Meter Values: )rawliteral";
  s += String(watermark_meter_buffer);
  s += R"rawliteral(</li>
  <li>Watermark Logs: )rawliteral";
  s += String(watermark_log_buffer);
  s += R"rawliteral(</li>
  <li>Watermark Telegram: )rawliteral";
  s += String(watermark_telegram);
  s += R"rawliteral(</li>
  <li>Uptime: )rawliteral";
  s += Time_formatUptime();
  s += R"rawliteral(</li>
)rawliteral";

#if defined(ESP32)
  s += "<li>Reset Reason: " + Log_get_reset_reason() + "</li>\n";
#elif defined(ESP8266)
  s += "<li>Reset Reason: " + String(ESP.getResetReason()) + " / " + String(ESP.getResetInfo()) + "</li>\n";
#endif

  s += R"rawliteral(  <li>System time (UTC): )rawliteral";
  s += String(Time_getFormattedTime()) + " / " + String(Time_getEpochTime());
  s += R"rawliteral(</li>
  <li>Build Time: )rawliteral";
  s += String(BUILD_TIMESTAMP);
  s += R"rawliteral(</li>
  <li>Free Heap: )rawliteral";
  s += String(ESP.getFreeHeap());
  s += R"rawliteral(</li>
  <li>Log Buffer Length (max): )rawliteral";
  s += String(LOG_BUFFER_SIZE);
  s += R"rawliteral(</li>
  <li><a href='update'>FW Update</a></li>
  <li><a href='restart'>Restart</a></li>
</ul>

<h3>Log Buffer (last 10 / index )rawliteral";
  s += String(logIndex);
  s += R"rawliteral()</h3>
<ul>
  <li><a href='showLogBuffer'>Show full Log</a></li>
  <li><a href='resetLogBuffer'>Reset Log</a></li>
  <div class="log-section">)rawliteral";
  s += Log_BufferToString(10);
  s += R"rawliteral(</div>
</body></html>)rawliteral";

  server.send(200, "text/html", s);
}

void Webserver_ShowCert()
{
  server.send(200, "text/html", String(FullCert));
}

void Webserver_ShowTelegram_Raw()
{
  String s;
  s.reserve(TELEGRAM_LENGTH * 12 + 512); // 3 textarea blocks * ~4 chars/byte + headers
  s = "<div class='block'>Receive Buffer</div><textarea name='cert' rows='10' cols='80'>";
  for (int i = 0; i < TELEGRAM_LENGTH; i++) { if (i > 0) s += " "; s += String(telegram_receive_buffer[i]); }
  s += "</textarea><br><br><div class='block'>Receive Buffer Hex</div><textarea name='cert' rows='10' cols='80'>";
  for (int i = 0; i < TELEGRAM_LENGTH; i++) { if (i > 0) s += " "; s += String(telegram_receive_buffer[i], HEX); }
  s += "</textarea><br><br><div class='block'>Receive Buffer ASCII</div><textarea name='cert' rows='10' cols='80'>";
  for (int i = 0; i < TELEGRAM_LENGTH; i++)
  {
    char c = (char)telegram_receive_buffer[i];
    s += (isPrintable(c) || c == '\n' || c == '\r') ? String(c) : ".";
  }
  s += "</textarea>";
  server.send(200, "text/html", s);
}

void Webserver_ShowTelegram()
{
  if (iotWebConf.handleCaptivePortal()) return;

  String s;
  s.reserve(TELEGRAM_LENGTH * 45 + 1024); // ~45 chars per table row + header overhead
  s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
  s += "<title>SMGWLite - Show Telegram</title>" + String(HTML_STYLE);
  s += "<br>Last Byte received @ " + String(millis() - lastByteTime) + "ms ago<br>";
  s += "<br>Last Complete Telegram @ " + String(timestamp_telegram) + " = " + Time_formatTimestamp(timestamp_telegram) + ": " + String(Time_getEpochTime() - timestamp_telegram) + "s old<br>";
  s += "<table border=1><tr><th>Index</th><th>Receive Buffer</th></tr>";

  String color;
  int signature_7101 = 9999;
  int k = 0;
  for (int i = 0; i < TELEGRAM_LENGTH; i++)
  {
    if (i < TELEGRAM_LENGTH - 5 &&
        telegram_receive_buffer[i-k]   == 7 && telegram_receive_buffer[i+1-k] == 1 &&
        telegram_receive_buffer[i+2-k] == 0 && telegram_receive_buffer[i+3-k] == 1 &&
        telegram_receive_buffer[i+4-k] == 8)
    {
      color = "bgcolor=959018"; signature_7101 = i; if (k < 5) k++;
    }
    else if (i > signature_7101 && telegram_receive_buffer[i] == 0x77)
    {
      signature_7101 = 9999; color = "bgcolor=959018";
    }
    else color = "";
    s += "<tr><td>" + String(i) + "</td><td " + String(color) + ">" + String(telegram_receive_buffer[i], HEX) + "</td></tr>";
  }
  s += "</table></body></html>\n";
  server.send(200, "text/html", s);
}

void Webserver_ShowLastMeterValue()
{
  JsonDocument jsonDoc;
  jsonDoc["meter_value_180"] = LastMeterValue.meter_value_180;
  jsonDoc["timestamp"]       = LastMeterValue.timestamp;
  jsonDoc["temperature"]     = LastMeterValue.temperature;
  jsonDoc["solar"]           = LastMeterValue.solar;
  // OBIS 2.8.0 is always included in the JSON endpoint for completeness,
  // even if it is not stored in the packed ring-buffer
  jsonDoc["meter_value_280"] = LastMeterValue.meter_value_280;

  String jsonResponse;
  serializeJson(jsonDoc, jsonResponse);
  server.send(200, "application/json", jsonResponse);
}

void Param_configSaved()
{
  DLOGLN("Configuration was updated.");
  Led_update_Blink();
  Webclient_splitHostAndPath(String(backend_endpoint), backend_host, backend_path);
  Log_AddEntry(1003);

  cached_taf7_param          = max(1, atoi(taf7_param));
  cached_taf14_param         = max(1, atoi(taf14_param));
  cached_backend_call_minute = max(1, atoi(backend_call_minute));

  // Only re-initialise the ring-buffer (which clears all pending values!)
  // when a setting that affects the binary layout actually changed.
  // Saving unrelated settings must not silently discard buffered meter data.
  bool layout_changed =
    (config_temperature_object.isChecked() != last_init_temp)  ||
    (config_solar_object.isChecked()       != last_init_solar) ||
    (config_280_object.isChecked()         != last_init_280)   ||
    (atoi(Meter_Value_Buffer_Size_Char)    != last_init_buffer_kb);

  if (layout_changed)
  {
    
    // Flush pending values synchronously before the buffer is cleared.
    // Called directly (no task) so the send is guaranteed to complete
    // before MeterValue_init_Buffer() wipes the data.
    // Take the semaphore with a timeout so we don't block indefinitely
    // if a backend task happens to be running at the same moment.
    if (MeterValue_Num() > 0 && xSemaphoreTake(Sema_Backend, pdMS_TO_TICKS(15000)))
    {
      Webclient_send_meter_values_to_backend();
      xSemaphoreGive(Sema_Backend);
    }
    Log_AddEntry(1004);
    MeterValue_init_Buffer();
  }
}

void Led_update_Blink()
{
  if (led_blink_object.isChecked()) iotWebConf.enableBlink();
  else { iotWebConf.disableBlink(); digitalWrite(LED_BUILTIN, LOW); }
}
