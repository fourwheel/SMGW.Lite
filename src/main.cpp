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
#include <SPIFFS.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <HardwareSerial.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "Arduino.h"
#include "soc/uart_reg.h"  // UART_INT_RAW_REG / UART_INT_CLR_REG for parity-error detection
#include <Preferences.h>  // NVS persistence for serial config
#include "app_globals.h"
#include "time_utils.h"
#include "html_style.h"
#include "log_buffer.h"
#include "debug_log.h"
#include "serial_scan.h"
#include "webserver_optical.h"
#include "webserver_main.h"
#include "webserver_data.h"
#include "meter_value.h"
#include "ota_pull.h"
#include "telegram.h"

#include "build_info.h"
const String BUILD_TIMESTAMP = String(BUILD_TIMESTAMP_STR);
const String BUILD_BRANCH = String(BUILD_BRANCH_STR);

// -- Initial name of the Thing. Used e.g. as SSID of the own Access Point.
char thingName[20] = "SMGWLite"; // mutable — staticDelay suffix appended in setup()

// -- Initial password to connect to the Thing, when it creates an own Access Point.
const char wifiInitialApPassword[] = "password";

// ---------------------------------------------------------------------------
// Zero-touch deployment — pre-configured WiFi credentials.
// Credentials are defined in wifi_credentials.h (gitignored).
// Copy src/wifi_credentials.h.example to src/wifi_credentials.h and fill in your values.
// ---------------------------------------------------------------------------
#include "wifi_credentials.h"
#include "certs/isrg_root_x1.h"

#define STRING_LEN 128
#define ID_LEN 4
#define NUMBER_LEN 5

#include "version.h"

// Telegram vars
#define TELEGRAM_TIMEOUT_MS 30                    // timeout for telegram in ms
uint8_t telegram_receive_buffer[TELEGRAM_LENGTH]; // buffer for serial data
size_t telegram_receive_bufferIndex = 0;          // position in serial data buffer
unsigned long lastByteTime = 0;                   // timestamp of last received byte
unsigned long timestamp_telegram;                 // timestamp of telegram


int staticDelay = 0;

// Cached integer versions of config string params — updated in Param_configSaved() and setup().
// Avoids calling atoi() on every loop() tick.
int   cached_taf7_param             = 15;
int   cached_taf14_param            = 60;
int   cached_backend_call_minute    = 2;
// Backend vars
bool call_backend_successfull = true;
bool redirect_to_sysinfo = false;
bool          g_wifiSetupPending  = false;
bool          g_wifiSetupFailed   = false; // set by handle_wifi_setup_lifecycle(), read by /wifiStatus
unsigned long g_wifiSetupStartedAt = 0;    // millis() timestamp WiFi.begin() was issued from /wifiSetup
unsigned long g_apStopAt          = 0;    // millis() timestamp to stop AP, 0 = not scheduled
SemaphoreHandle_t Sema_Backend;       // Mutex / Semaphore for backend call
// Held by telegramTask while it reads mySerial and by the web handlers that
// restart it (end()/begin()), so the UART driver is never removed mid-read.
SemaphoreHandle_t Sema_Serial;
// Guards MeterValueBuffer + write pointers while a meter-value upload is in
// flight, so a store between "payload sent" and "acknowledged -> clear buffer"
// cannot be wiped without ever having been transmitted.
static SemaphoreHandle_t Sema_MeterBuffer;
volatile bool ota_active          = false; // set during OTA to block new backend calls
volatile bool g_ota_check_requested = false;          // backend offered another firmware version (OtaPull_setOffer)
volatile bool g_ota_manual_install_requested = false; // user confirmed "Installieren" on the Remote FW Update page
static TaskHandle_t h_meter_task = NULL;
static TaskHandle_t h_log_task   = NULL;
unsigned long last_call_backend = 0; // 0 = never called; set to millis() on first attempt
unsigned long last_backend_success = 0; // millis() of the last acknowledged upload, see handle_backend_recovery()

// Self-recovery, see supervisorTask(): loop() heartbeat and the time the
// running upload task got Sema_Backend (0 = no upload in progress).
volatile unsigned long g_loop_heartbeat     = 0;
volatile unsigned long g_meter_task_started = 0;
volatile unsigned long g_log_task_started   = 0;
#ifdef TEST_SELF_RECOVERY
volatile bool g_test_hang_upload = false; // set by /testTaskHang
#endif
// The cause of a supervisorTask() restart (log code 1120-1122) is kept in NVS
// ("recovery"/"cause"), not in RTC_NOINIT memory: the IDF keeps the system
// time across a software reset in RTC_NOINIT variables, and adding our own
// shifts their addresses, so after an OTA update between firmware versions
// with a different RTC_NOINIT layout the clock would read garbage.

// Temperature vars
#define ONE_WIRE_BUS 4
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature Temp_sensors(&oneWire);
unsigned long last_temperature  = 0;
bool read_temperature           = false;
int current_temperature         = 0;

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
void handle_backend_recovery();
void supervisorTask(void *pvParameters);
void handle_remote_ota();
void handle_check_wifi_connection();
void handle_wifi_setup_lifecycle();
void handle_MeterValue_trigger();
void handle_telegram_watchdog();
void handle_MeterValue_store();
void handle_temperature();
void Led_update_Blink();
bool MeterValue_store(bool override);
bool MeterValue_unchangedSincePrev(const MeterValue &v);
int32_t MeterValue_get_from_remote();
bool Telegram_parse_SML(uint8_t* buffer, size_t length);
bool Telegram_parse_IEC(uint8_t* buffer, size_t length);
void OTA_setup();
void Param_configSaved();

void Telegram_ResetReceiveBuffer();
void handle_Telegram_receive();
void Webclient_send_log_to_backend();
void Webclient_Send_Log_to_backend_Task(void *pvParameters);
void Webclient_Send_Log_to_backend_wrapper();
void Webclient_send_meter_values_to_backend();
void Webclient_Send_Meter_Values_to_backend_Task(void *pvParameters);
void Webclient_Send_Meter_Values_to_backend_wrapper();
void Webclient_splitHostAndPath(const String &url, String &host, String &path);
static int Webclient_read_response(WiFiClientSecure &client, String &body, unsigned long timeout_ms);
void Webserver_HandleCertUpload();
void Webserver_LocationHrefsysinfo(int delay);
void Webserver_SetCert();

void Webserver_TestBackendConnection();
void Webserver_TestBackendConnectionRun();

DNSServer dnsServer;
WebServer server(80);

HardwareSerial mySerial(1);

bool MeterValue_trigger_override     = false;
bool MeterValue_trigger_non_override = false;
// MeterValue_trigger_is_taf7 narrows MeterValue_trigger_override down to TAF7
// TAF7 mark stores. The override trigger is also set by the boot snapshot and
// by a manual TAF6 store (/StoreMeterValue); all three share the override store
// path, but only a TAF7 mark store may remove a TAF14 entry stored <10 s before
// it (see handle_MeterValue_store()). A manual TAF6 must keep every reading it
// is asked for, so it must never remove anything.
bool MeterValue_trigger_is_taf7      = false;
bool startup_print_done              = false; // one-time diagnostic after first telegram
bool boot_snapshot_done              = false; // boot snapshot: fired once after first telegram + reliable time
bool last_store_was_override         = false; // true when the last successful store was a TAF7 override
unsigned long last_meter_value_successful = 0;
unsigned long last_taf7_meter_value       = 0;
unsigned long last_taf14_meter_value      = 0;
unsigned long last_reconnect_attempt      = 0;
unsigned long last_telegram_parsed      = 0; // millis() of last successfully parsed telegram; initialised in setup()
unsigned long last_silence_log            = 0; // millis() 3005/3007 were last logged, see handle_telegram_watchdog()

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


IotWebConfParameterGroup groupTelegram      = IotWebConfParameterGroup("groupTelegram",      "Telegram Param");
IotWebConfParameterGroup groupBackend       = IotWebConfParameterGroup("groupBackend",       "Backend Config");
IotWebConfParameterGroup groupTaf           = IotWebConfParameterGroup("groupTaf",           "Taf config");
IotWebConfParameterGroup groupAdditionalMeter = IotWebConfParameterGroup("groupAdditionalMeter", "Additional Meters & Sensors");
IotWebConfParameterGroup groupSys           = IotWebConfParameterGroup("groupSys",           "Advanced Sys Config");
IotWebConfParameterGroup groupDebug         = IotWebConfParameterGroup("groupDebug",         "Debug Helpers");

IotWebConfCheckboxParameter activate_IEC_Parser_object = IotWebConfCheckboxParameter("- NOT USED -", "activate_IEC_Parser", activate_IEC_Parser, STRING_LEN, false);

IotWebConfTextParameter     backend_endpoint_object     = IotWebConfTextParameter("backend endpoint", "backend_endpoint", backend_endpoint, STRING_LEN);
IotWebConfCheckboxParameter led_blink_object            = IotWebConfCheckboxParameter("LED Blink", "led_blink", led_blink, STRING_LEN, DNS_FALLBACK_SERVER_INDEX);
IotWebConfTextParameter     backend_ID_object           = IotWebConfTextParameter("backend ID", "backend_ID", backend_ID, ID_LEN);
IotWebConfTextParameter     backend_token_object        = IotWebConfTextParameter("backend token", "backend_token", backend_token, STRING_LEN);

IotWebConfCheckboxParameter taf7_b_object               = IotWebConfCheckboxParameter("Taf 7 activated", "b_taf7", b_taf7, STRING_LEN, true);
IotWebConfNumberParameter   taf7_param_object           = IotWebConfNumberParameter("Taf 7 minute", "taf7_param", taf7_param, NUMBER_LEN, "15", "60...1", "min='1' max='60' step='1'");
IotWebConfCheckboxParameter taf14_b_object              = IotWebConfCheckboxParameter("Taf 14 activated", "b_taf14", b_taf14, STRING_LEN, true);
IotWebConfNumberParameter   taf14_param_object          = IotWebConfNumberParameter("Taf 14 Meter Interval (s)", "taf14_param", taf14_param, NUMBER_LEN, "60", "1..100 s", "min='1' max='100' step='1'");
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


volatile bool b_send_log_to_backend = false;
volatile bool g_log_upload_full = false; // next log upload sends the whole ring (manual upload pages)


void Webserver_LocationHrefsysinfo(int delay)
{
  String call = "<meta http-equiv='refresh' content='" + String(delay) + ";url=/sysinfo'>";
  server.send(200, "text/html", call);
}


String meter_model = "";

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
  // Read directly from SPIFFS so the textarea is empty when no custom cert is stored.
  // Empty textarea = bundled ISRG Root X1 fallback is active.
  String stored = "";
  File file = SPIFFS.open("/cert.pem", FILE_READ);
  if (file && file.size() > 0)
  {
    stored = file.readString();
    file.close();
  }

  String page;
  page.reserve(2000);
  page += R"rawliteral(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>SmartMeterLite &ndash; SSL Certificate</title>)rawliteral";
  page += HTML_STYLE_MODERN;
  page += R"rawliteral(</head>
<body>
<div class="logo">&#9889; SmartMeterLite</div>
<a class="back" href="/sysinfo">&#8592; Zur&uuml;ck</a>
<div class="card">
  <div class="card-title">SSL Certificate</div>
  <form action="/upload" method="POST">
    <textarea name="cert" rows="12">)rawliteral";
  page += stored;
  page += R"rawliteral(</textarea>
    <p class="hint">Leave empty to use the onboard ISRG Root X1 cert (valid until 2035).</p>
    <div class="btns" style="margin-top:.7rem;">
      <button class="btn" type="submit">Save</button>
      <a class="btn btn-s" href="/sysinfo">Cancel</a>
    </div>
  </form>
</div>
</body></html>)rawliteral";

  server.send(200, "text/html", page);
}

void Webserver_TestBackendConnection()
{
  size_t certLen = strlen(FullCert);
  String certStart = String(FullCert).substring(0, 30);
  certStart.replace("<", "&lt;");
  String certEnd = certLen > 30 ? String(FullCert).substring(certLen - 30) : "";
  certEnd.replace("<", "&lt;");

  time_t now = (time_t)Time_getEpochTime();
  char timeBuf[32];
  struct tm* ti = gmtime(&now);
  strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S UTC", ti);

  String page;
  page.reserve(2500);
  page += R"rawliteral(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>SmartMeterLite &ndash; Backend Test</title>)rawliteral";
  page += HTML_STYLE_MODERN;
  page += R"rawliteral(</head>
<body>
<div class="logo">&#9889; SmartMeterLite</div>
<a class="back" href="/sysinfo">&#8592; Zur&uuml;ck</a>
<div class="card">
  <div class="card-title">Backend Connection Test</div>)rawliteral";
  page += "<div class='kv'><span class='kl'>Cert length</span>" + String(certLen) + "</div>";
  page += "<div class='kv'><span class='kl'>Cert start</span><code>" + certStart + "</code></div>";
  page += "<div class='kv'><span class='kl'>Cert end</span><code>" + certEnd + "</code></div>";
  page += "<div class='kv'><span class='kl'>Device time</span><code>" + String(timeBuf) + "</code> (epoch " + String((unsigned long)now) + ")</div>";
  page += R"rawliteral(
  <div id="result">
    <div class="kv"><span class="kl">Host</span><span class="warn">&#9679; Teste&hellip;</span></div>
  </div>
</div>
<script>
fetch('/testBackendConnectionRun')
  .then(function(r){return r.json();})
  .then(function(d){
    var h='';
    if(!d.host_ok){
      h+="<div class='kv last'><span class='kl'>Host</span><span class='fail'>&#10060; Nicht erreichbar</span></div>";
    } else {
      h+="<div class='kv'><span class='kl'>Host</span><span class='ok'>&#9989; Erreichbar</span></div>";
      h+="<div class='kv'><span class='kl'>Zertifikat</span>"+(d.cert_ok?"<span class='ok'>&#9989; G&uuml;ltig</span>":"<span class='fail'>&#10060; Ung&uuml;ltig</span>")+"</div>";
      h+="<div class='kv last'><span class='kl'>ID &amp; Token</span>"+(d.auth_ok?"<span class='ok'>&#9989; G&uuml;ltig</span>":"<span class='fail'>&#10060; Ung&uuml;ltig oder Timeout</span>")+"</div>";
    }
    document.getElementById('result').innerHTML=h;
  })
  .catch(function(){
    document.getElementById('result').innerHTML="<div class='kv last'><span class='kl'>Fehler</span><span class='fail'>Keine Antwort vom Ger&auml;t</span></div>";
  });
</script>
</body></html>)rawliteral";

  server.send(200, "text/html", page);
}

void Webserver_TestBackendConnectionRun()
{
  bool host_ok = false;
  bool cert_ok = false;
  bool auth_ok = false;

  WiFiClientSecure client;
  client.setHandshakeTimeout(10); // seconds; the default of 120 s would block loop() for minutes
  client.setCACert(FullCert);

  if (client.connect(backend_host.c_str(), 443, 5000))
  {
    host_ok = true;
    cert_ok = true;
  }
  else
  {
    client.stop();
    client.setInsecure();
    if (client.connect(backend_host.c_str(), 443, 5000))
      host_ok = true;
  }

  if (host_ok)
  {
    String url = String(backend_path) + "?backend_test=true&ID=" + String(backend_ID);
    client.print(String("GET ") + url + " HTTP/1.1\r\n" +
                 "Host: " + String(backend_host) + "\r\n" +
                 "X-Auth-Token: " + String(backend_token) + "\r\n" +
                 "Connection: close\r\n\r\n");
    // ID & token only count as valid if the backend confirms them in the body
    // ({"ok":true,"id":<backend_ID>}) — a bare HTTP 200 may come from a
    // misconfigured server that never ran the backend script.
    String body;
    if (Webclient_read_response(client, body, 5000) == 200)
    {
      JsonDocument doc;
      auth_ok = deserializeJson(doc, body) == DeserializationError::Ok &&
                (doc["ok"] | false) && strcmp(doc["id"] | "", backend_ID) == 0;
    }
    client.stop();
  }

  String json = "{\"host_ok\":";
  json += host_ok ? "true" : "false";
  json += ",\"cert_ok\":";
  json += cert_ok ? "true" : "false";
  json += ",\"auth_ok\":";
  json += auth_ok ? "true" : "false";
  json += "}";
  server.send(200, "application/json", json);
}

void Webserver_HandleCertUpload()
{
  if (server.hasArg("cert"))
  {
    String cert = server.arg("cert");
    cert.trim();

    if (cert.length() == 0)
    {
      // Empty submission — remove SPIFFS file so the bundled fallback cert is used.
      SPIFFS.remove("/cert.pem");
      Log_Add(8002);
      Webserver_LocationHrefsysinfo();
      return;
    }

    File file = SPIFFS.open("/cert.pem", FILE_WRITE);
    if (file)
    {
      file.println(cert);
      file.close();
      Log_Add(8002);
      Webserver_LocationHrefsysinfo();
    }
    else
    {
      Log_Add(8003);
      Webserver_LocationHrefsysinfo();
      server.send(500, "text/plain", "Cannot Open File!");
    }
  }
  else
  {
    Log_Add(8004);
    Webserver_LocationHrefsysinfo();
  }
}

void Webclient_loadCertToChar()
{
  File file = SPIFFS.open("/cert.pem", FILE_READ);
  if (file && file.size() > 0)
  {
    size_t size = min((size_t)file.size(), sizeof(FullCert) - 1);
    file.readBytes(FullCert, size);
    FullCert[size] = '\0';
    file.close();
    return;
  }
  // No cert in SPIFFS — fall back to bundled ISRG Root X1
  strncpy(FullCert, ISRG_ROOT_X1, sizeof(FullCert) - 1);
  FullCert[sizeof(FullCert) - 1] = '\0';
  Log_AddWithoutTransmit(8001);
}

void Webclient_Send_Meter_Values_to_backend_Task(void *pvParameters)
{
  if (ota_active) { h_meter_task = NULL; vTaskDelete(NULL); return; }
  if (xSemaphoreTake(Sema_Backend, portMAX_DELAY))
  {
    // Timed from here, not from the spawn: waiting for another holder of
    // Sema_Backend is not this task's hang, and every holder is watched
    // itself (the other upload task, or loop() for OTA and web pages).
    g_meter_task_started = millis();
#ifdef TEST_SELF_RECOVERY
    if (g_test_hang_upload) for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
#endif
    if (!ota_active) Webclient_send_meter_values_to_backend();
    xSemaphoreGive(Sema_Backend);
  }
  watermark_meter_buffer = uxTaskGetStackHighWaterMark(NULL);
  g_meter_task_started = 0;
  h_meter_task = NULL;
  vTaskDelete(NULL);
}

void Webclient_Send_Log_to_backend_Task(void *pvParameters)
{
  if (ota_active) { h_log_task = NULL; vTaskDelete(NULL); return; }
  if (xSemaphoreTake(Sema_Backend, portMAX_DELAY))
  {
    g_log_task_started = millis(); // see Webclient_Send_Meter_Values_to_backend_Task()
    if (!ota_active)
    {
      // Cleared right before the buffer is copied: entries logged from now on
      // set it again (Log_Add) and go out with the next upload. Not
      // cleared when skipped for OTA, so the pending log isn't lost.
      b_send_log_to_backend = false;
      Webclient_send_log_to_backend();
    }
    xSemaphoreGive(Sema_Backend);
  }
  watermark_log_buffer = uxTaskGetStackHighWaterMark(NULL);
  g_log_task_started = 0;
  h_log_task = NULL;
  vTaskDelete(NULL);
}


void telegramTask(void * pvParameters) {
  for(;;) {
    xSemaphoreTake(Sema_Serial, portMAX_DELAY);
    if (SerialScan_consumePending()) {
      SerialScan_run();
    } else if (!SerialScan_isRunning()) {
      handle_Telegram_receive();
    }
    xSemaphoreGive(Sema_Serial);
    vTaskDelay(pdMS_TO_TICKS(10));
    watermark_telegram = uxTaskGetStackHighWaterMark(NULL);
  }
}

void setup()
{
  Sema_Backend = xSemaphoreCreateMutex();
  Sema_MeterBuffer = xSemaphoreCreateMutex();
  Sema_Serial = xSemaphoreCreateMutex();
  LogBuffer_reset();
  last_telegram_parsed = millis(); // start watchdog timer from boot
  Log_Add(1001);
  // Why did the device start? 1100 + esp_reset_reason() (power on, panic,
  // watchdog, brownout, software...), plus the cause if supervisorTask()
  // restarted it. Both reach the backend with the first log upload.
  Log_Add(1100 + (int)esp_reset_reason());
  {
    // Read-only first: the namespace is only opened for writing (and thereby
    // created) by Supervisor_restart(), not on every boot.
    Preferences prefs;
    int cause = 0;
    if (prefs.begin("recovery", true)) { cause = prefs.getInt("cause", 0); prefs.end(); }
    if (cause != 0)
    {
      Log_Add(cause);
      prefs.begin("recovery", false);
      prefs.remove("cause");
      prefs.end();
    }
  }
  // As early as possible: confirms the running image unless it is a pulled
  // update awaiting validation (until then a restart rolls it back).
  OtaPull_boot();
  Serial.begin(115200);
#ifdef SERIAL_DEBUG
  // USB-CDC needs time to enumerate before the host monitor connects.
  // Without this delay the first log lines are lost.
  unsigned long _t = millis();
  while (!Serial && millis() - _t < 3000) delay(10);
#endif
  mySerial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);
  SerialConfig_load();  // override with NVS-persisted config if available
  DLOGLN();
  DLOGLN("Starting up...Hello!");

  // Compute staticDelay before Param_setup() so the suffix is visible in the
  // AP SSID (IotWebConf reads thingName during init() inside Param_setup()).
  {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    uint16_t lastTwoBytes = (mac[4] << 8) | mac[5];
    staticDelay = lastTwoBytes % 60;
    snprintf(thingName, sizeof(thingName), "SMGWLite-%d", staticDelay);
  }

  // Zero-touch: seed WiFi credentials so the device connects on first boot
  // without requiring the AP config portal. Stored values (if any) win.
  Param_setup(); // calls iotWebConf.init() — loads stored config from NVS

  // Zero-touch: if no SSID is stored yet (first boot / after flash erase),
  // write the default credentials directly and persist them to NVS so the
  // device connects immediately without going through the AP config portal.
  {
    bool needsSave = false;

    // WiFi — IotWebConf's mustStayInApMode() requires BOTH the WiFi SSID AND
    // the AP password to be non-empty before it honours skipApStartup().
    if (strlen(iotWebConf.getWifiSsidParameter()->valueBuffer) == 0)
    {
      strncpy(iotWebConf.getWifiSsidParameter()->valueBuffer,
              WIFI_DEFAULT_SSID, IOTWEBCONF_WORD_LEN);
      strncpy(iotWebConf.getWifiPasswordParameter()->valueBuffer,
              WIFI_DEFAULT_PASSWORD, IOTWEBCONF_WORD_LEN);
      strncpy(iotWebConf.getApPasswordParameter()->valueBuffer,
              WIFI_DEFAULT_AP_PASSWORD, IOTWEBCONF_PASSWORD_LEN);
      needsSave = true;
      DLOGLN("Zero-touch: WiFi credentials written.");
    }

    // Backend — set defaults if not yet configured
    if (strlen(backend_endpoint) == 0)
    {
      strncpy(backend_endpoint, DEFAULT_BACKEND_ENDPOINT, STRING_LEN);
      needsSave = true;
    }
    if (strlen(backend_ID) == 0)
    {
      strncpy(backend_ID, DEFAULT_BACKEND_ID, ID_LEN);
      needsSave = true;
    }
    if (strlen(backend_token) == 0)
    {
      strncpy(backend_token, DEFAULT_BACKEND_TOKEN, STRING_LEN);
      needsSave = true;
    }

    if (needsSave)
    {
      iotWebConf.saveConfig();
      DLOGLN("Zero-touch: defaults written to NVS.");
    }
  }
  cached_taf7_param           = max(1, atoi(taf7_param));
  cached_taf14_param          = max(1, atoi(taf14_param));
  cached_backend_call_minute  = max(1, atoi(backend_call_minute));
  Led_update_Blink();
  Webserver_UrlConfig();
#ifdef TEST_SELF_RECOVERY
  // Test only: simulate the failures handled by supervisorTask() and
  // handle_backend_recovery(); the supervisor limit is 1 min in this build.
  //   PowerShell: $env:PLATFORMIO_BUILD_FLAGS="-DTEST_SELF_RECOVERY"; pio run -e esp32-nodemcu -t upload
  //               Remove-Item Env:PLATFORMIO_BUILD_FLAGS
  // /testLoopHang        loop() blocks forever            -> restart, log 1120
  // /testTaskHang        the next meter upload task hangs while holding
  //                      Sema_Backend                      -> restart, log 1121
  // /testBackendRecovery last acknowledged call backdated   -> WiFi reconnect, log 7000
  server.on("/testLoopHang", [] {
    server.send(200, "text/plain", "loop() blocks now");
    for (;;) delay(1000);
  });
  server.on("/testTaskHang", [] {
    g_test_hang_upload = true;
    server.send(200, "text/plain", "the next meter upload task hangs");
  });
  server.on("/testBackendRecovery", [] {
    last_backend_success = millis() - 3UL * 3600 * 1000;
    server.send(200, "text/plain", "last acknowledged backend call backdated by 3 h");
  });
#endif
  OTA_setup();

  if (!SPIFFS.begin(true)) Log_Add(8000);
  Webclient_loadCertToChar();
  Webclient_splitHostAndPath(String(backend_endpoint), backend_host, backend_path);

  // MeterValue_init_Buffer reads the feature flags from IotWebConf, so it must
  // be called after iotWebConf.init() (inside Param_setup) has restored them.
  MeterValue_init_Buffer();

  Time_begin();
  Temp_sensors.begin();
  DLOG("Temp sensors found: ");
  DLOGLN(Temp_sensors.getDeviceCount());
  // Blocking first conversion (~750 ms) so telegrams parsed right after boot
  // carry a real temperature instead of the initial 0. Without a prior
  // requestTemperatures() the sensor would also return its 85 degC power-on value.
  if (temperature_object.isChecked())
  {
    Temp_sensors.requestTemperatures();
    float raw_temp = Temp_sensors.getTempCByIndex(0) * 100;
    if (raw_temp > -10000) // filter out -127°C sensor error (-12700 in raw units)
      current_temperature = (int)raw_temp;
    // Seed LastMeterValue too: the parser carries the previous temperature over
    // into each new reading and only overwrites it after the first-telegram
    // diagnostics, while the boot snapshot (loop(), other core) may already
    // store that first reading.
    LastMeterValue.temperature = current_temperature;
    DLOG("Initial temperature: ");
    DLOGLN(current_temperature);
    // Next conversion is requested by handle_temperature() after the regular 20 s interval
    last_temperature = millis();
    read_temperature = true;
  }

  // staticDelay already set above (before Param_setup)

  vTaskPrioritySet(NULL, 3);
  xTaskCreate(telegramTask, "TelegramBot", 3072, NULL, 0, NULL); // ~700 B left with 2048 on the esp32dev (Xtensa) while parsing SML
  // Priority above loop() (3): on the single-core ESP32-C3 a busy-looping
  // loop() must not be able to starve the supervisor.
  g_loop_heartbeat = millis();
  xTaskCreate(supervisorTask, "Supervisor", 4096, NULL, 4, NULL); // 4 KB: the NVS write before a restart needs more than 2 KB
}

void OTA_setup()
{
  ArduinoOTA
    .onStart([]() {
      ota_active = true;
      String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
      Serial.println("Start updating " + type);
    })
    .onEnd([]() {
      ota_active = false;
      Serial.println("\nEnd");
      if (MeterValue_Num() > 0 && WiFi.isConnected()) {
        Serial.println("OTA: sending pending meter values before restart...");
        if (xSemaphoreTake(Sema_Backend, pdMS_TO_TICKS(15000))) {
          Webclient_send_meter_values_to_backend();
          xSemaphoreGive(Sema_Backend);
        }
      }
    })
    .onProgress([](unsigned int progress, unsigned int total) { Serial.printf("Progress: %u%%\r", (progress / (total / 100))); })
    .onError([](ota_error_t error) {
      ota_active = false;
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
  if (h_meter_task != NULL) { DLOGLN("Meter task still running, skipping spawn"); return; }
  xTaskCreate(Webclient_Send_Meter_Values_to_backend_Task, "Send_Meter task", 8192, NULL, 2, &h_meter_task);
}

void Webclient_Send_Log_to_backend_wrapper()
{
  if (h_log_task != NULL) { DLOGLN("Log task still running, skipping spawn"); return; }
  xTaskCreate(Webclient_Send_Log_to_backend_Task, "send log task", 8192, NULL, 2, &h_log_task);
}

// Finds an OBIS code in an SML buffer and returns the raw integer value
// and its SML scaler byte. The raw value is sign-extended for signed types (0x5x).
// Returns false if the OBIS code is not found or the value type is unsupported.
bool obisExtractRaw(uint8_t* buffer, int px, int sx, uint8_t* code,
                    uint64_t* raw_out, int8_t* scaler_out) {
  for (int i = px; i < sx - 12; i++) {
    if (memcmp(&buffer[i], code, 6) == 0) {
      for (int j = i + 6; j < i + 40 && j < sx; j++) {
        if (buffer[j] == 0x52) {
          int8_t  scaler    = (int8_t)buffer[j + 1];
          uint8_t typeByte  = buffer[j + 2];
          uint8_t typeGroup = typeByte & 0xF0;
          if (typeGroup == 0x50 || typeGroup == 0x60) {
            int vLen   = (typeByte & 0x0F) - 1;
            int vStart = j + 3;
            if (vStart + vLen > sx || vLen <= 0 || vLen > 8) continue;
            uint64_t raw64 = 0;
            for (int k = 0; k < vLen; k++) raw64 = (raw64 << 8) | buffer[vStart + k];
            if (typeGroup == 0x50) {
              uint64_t signBit = 1ULL << (vLen * 8 - 1);
              if (raw64 & signBit)
                raw64 |= ~((signBit << 1) - 1);
            }
            *raw_out    = raw64;
            *scaler_out = scaler;
            return true;
          }
        }
      }
    }
  }
  return false;
}

// Converts a raw SML value to 0.1 Wh (the DB energy storage unit).
// SML scaler s: raw is in 10^s Wh → multiply by 10^(s+1) to reach 10^-1 Wh.
static uint32_t smlToDeciWh(uint64_t raw, int8_t scaler) {
  int8_t adj = scaler + 1;
  if (adj > 0) for (int8_t i = 0; i < adj;  i++) raw *= 10;
  if (adj < 0) for (int8_t i = 0; i > adj; i--) raw /= 10;
  return (uint32_t)raw;
}

// Converts a raw SML value to integer watts.
// SML scaler s: raw is in 10^s W → apply 10^s to reach W.
static int32_t smlToWatt(uint64_t raw_u, int8_t scaler) {
  int64_t raw = (int64_t)raw_u;
  if (scaler > 0) for (int8_t i = 0; i < scaler;  i++) raw *= 10;
  if (scaler < 0) for (int8_t i = 0; i > scaler; i--) raw /= 10;
  return (int32_t)raw;
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

  // 3a. Extract meter serial (OBIS 0-0:96.1.0) for meter_model if not yet known
  if (meter_model.isEmpty()) {
    String model; // built locally, assigned once - other tasks read meter_model
    uint8_t obis960[] = {0x01, 0x00, 0x60, 0x01, 0x00, 0xff};
    for (int i = px; i < sx - 12; i++) {
      if (memcmp(&buffer[i], obis960, 6) == 0) {
        for (int j = i + 6; j < i + 40 && j < sx; j++) {
          uint8_t tl = buffer[j];
          if ((tl & 0xF0) == 0x00 && (tl & 0x0F) > 1) {
            int slen = (tl & 0x0F) - 1;
            if (j + slen >= sx) break;
            // Look for 3 consecutive uppercase letters (manufacturer code)
            for (int k = 0; k < slen - 2; k++) {
              uint8_t a = buffer[j+1+k], b = buffer[j+2+k], c = buffer[j+3+k];
              if (a >= 'A' && a <= 'Z' && b >= 'A' && b <= 'Z' && c >= 'A' && c <= 'Z') {
                model = String((char)a) + String((char)b) + String((char)c);
                break;
              }
            }
            if (model.isEmpty()) {
              char hex[3];
              for (int k = 0; k < slen; k++) {
                sprintf(hex, "%02X", buffer[j+1+k]);
                if (k) model += ' ';
                model += hex;
              }
            }
            break;
          }
        }
        break;
      }
    }
    meter_model = model;
  }

  // 3b. Extract OBIS Data
  uint8_t obis180[] = {0x01, 0x00, 0x01, 0x08, 0x00, 0xff};
  uint8_t obis280[] = {0x01, 0x00, 0x02, 0x08, 0x00, 0xff};
  uint8_t obis170[] = {0x01, 0x00, 0x01, 0x07, 0x00, 0xff};
  uint8_t obis270[] = {0x01, 0x00, 0x02, 0x07, 0x00, 0xff};
  uint8_t obis167[] = {0x01, 0x00, 0x10, 0x07, 0x00, 0xff};
  uint32_t temp180 = 0, temp280 = 0, temp170 = 0, temp270 = 0, temp167 = 0;
  uint64_t raw; int8_t sc;
  bool found180 = obisExtractRaw(buffer, px, sx, obis180, &raw, &sc);
  if (found180) temp180 = smlToDeciWh(raw, sc);
  bool found280 = obisExtractRaw(buffer, px, sx, obis280, &raw, &sc);
  if (found280) temp280 = smlToDeciWh(raw, sc);
  if (obisExtractRaw(buffer, px, sx, obis170, &raw, &sc)) temp170 = (uint32_t)smlToWatt(raw, sc);
  if (obisExtractRaw(buffer, px, sx, obis270, &raw, &sc)) temp270 = (uint32_t)smlToWatt(raw, sc);
  if (obisExtractRaw(buffer, px, sx, obis167, &raw, &sc)) temp167 = (uint32_t)smlToWatt(raw, sc);

  // Only update globals if the main consumption register was found.
  // All other fields are optional — some meters don't transmit them.
  //
  // Build the update in a local struct first, then assign to LastMeterValue in
  // one shot. This avoids a SMP race condition: telegramTask runs on Core 0
  // while the webserver / MeterValue_store run on Core 1. The old approach
  // called resetMeterValue() (which zeroed meter_value_180) and then assigned
  // temp180 in a separate step, leaving a brief window where meter_value_180 == 0
  // was visible to the other core — causing the REST API to occasionally return 0.
  if (found180 && temp180 > 0) {
    if (PrevMeterValue.meter_value_180 > 0 && temp180 < PrevMeterValue.meter_value_180)
      Log_Add(1208);
    if (found280 && PrevMeterValue.meter_value_280 > 0 && temp280 < PrevMeterValue.meter_value_280)
      Log_Add(1208);
    MeterValue newVal = {};
    // Preserve solar if MyStrom is active (mirrors resetMeterValue logic)
    if (mystrom_PV_object.isChecked()) newVal.solar = LastMeterValue.solar;
    // Always preserve the externally-sourced temperature reading
    newVal.temperature     = LastMeterValue.temperature;
    newVal.meter_value_180 = temp180;
    newVal.meter_value_280 = found280 ? temp280 : 0;
    newVal.power_import    = temp170;
    newVal.power_export    = temp270;
    newVal.net_power       = (int32_t)temp167;
    newVal.timestamp       = Time_getEpochTime();
    // Single locked assignment: readers never see meter_value_180 at 0 or a
    // mix of the old and the new telegram.
    MeterValue_setLast(newVal);
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

  // Extract meter model from identification line (/<MFR><baud><ident>) if not yet known
  if (meter_model.isEmpty() && telegram_str[0] == '/') {
    String model; // built locally, assigned once - other tasks read meter_model
    const char *eol = strstr(telegram_str, "\r\n");
    size_t lineLen = eol ? (size_t)(eol - telegram_str) : 0;
    if (lineLen >= 4) {
      char mfr[4]; strncpy(mfr, telegram_str + 1, 3); mfr[3] = '\0';
      model = mfr;
      if (lineLen > 5) {
        char ident[64];
        size_t identLen = lineLen - 5 < sizeof(ident) - 1 ? lineLen - 5 : sizeof(ident) - 1;
        strncpy(ident, telegram_str + 5, identLen); ident[identLen] = '\0';
        model += ' '; model += ident;
      }
    }
    meter_model = model;
  }

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

  // Helper lambda: find OBIS label in IEC text, parse the float value after '('
  auto parseIecObis = [&](const char* label, float* out) -> bool {
    const char *p = strstr(telegram_str, label);
    if (!p) return false;
    const char *op = strchr(p, '(');
    const char *st = op ? strchr(op, '*') : nullptr;
    if (!op || !st || op >= st) return false;
    char buf[20];
    size_t l = st - op - 1;
    if (l >= sizeof(buf)) return false;
    strncpy(buf, op + 1, l);
    buf[l] = '\0';
    for (int i = 0; buf[i]; i++) if (buf[i] == ',') buf[i] = '.';
    *out = atof(buf);
    return true;
  };

  uint32_t new180 = (uint32_t)(kWh180 * 10000.0f);
  if (PrevMeterValue.meter_value_180 > 0 && new180 < PrevMeterValue.meter_value_180)
    Log_Add(1208);

  float v280_raw = 0.0f;
  bool has280 = parseIecObis("1-0:2.8.0", &v280_raw);
  uint32_t new280 = has280 ? (uint32_t)(v280_raw * 10000.0f) : 0;
  if (has280 && PrevMeterValue.meter_value_280 > 0 && new280 < PrevMeterValue.meter_value_280)
    Log_Add(1208);

  float v170 = 0.0f, v270 = 0.0f, v167 = 0.0f;
  parseIecObis("1-0:1.7.0",  &v170);
  parseIecObis("1-0:2.7.0",  &v270);
  parseIecObis("1-0:16.7.0", &v167);

  // Build update in a local struct first, then assign to LastMeterValue in one shot.
  // Avoids the SMP race window where resetMeterValue() zeroes meter_value_180 and the
  // other core sees 0 before the new value is written (same fix as in Telegram_parse_SML).
  MeterValue newVal = {};
  if (mystrom_PV_object.isChecked()) newVal.solar = LastMeterValue.solar;
  newVal.temperature     = LastMeterValue.temperature;
  newVal.meter_value_180 = new180;
  newVal.meter_value_280 = has280 ? new280 : 0;
  newVal.timestamp       = Time_getEpochTime();
  newVal.power_import    = (uint32_t)(v170 * 1000.0f);
  newVal.power_export    = (uint32_t)(v270 * 1000.0f);
  newVal.net_power       = (int32_t)(v167 * 1000.0f);
  MeterValue_setLast(newVal);

  return true;
}


void myStrom_get_Meter_value()
{
  if (!mystrom_PV_object.isChecked()) { LastMeterValue.solar = 0; return; }

  DLOGLN(F("myStrom_get_Meter_value Connecting..."));
  WiFiClient client;
  // WiFiClient::setTimeout() takes seconds (connect and every read). This runs
  // in loop() on every store, so an unreachable myStrom must not block long.
  client.setTimeout(3);
  if (!client.connect(mystrom_PV_IP, 80)) { Log_Add(5000); DLOGLN(F("myStrom_get_Meter_value Connection failed")); LastMeterValue.solar = 0; return; }

  DLOGLN(F("myStrom_get_Meter_value Connected!"));
  client.println(F("GET /report HTTP/1.0"));
  client.print(F("Host: ")); client.println(mystrom_PV_IP);
  client.println(F("Connection: close"));
  if (client.println() == 0) { Log_Add(5001); client.stop(); LastMeterValue.solar = 0; return; }

  char status[32] = {0};
  client.readBytesUntil('\r', status, sizeof(status));
  if (strcmp(status + 9, "200 OK") != 0) { client.stop(); LastMeterValue.solar = 0; return; }

  char endOfHeaders[] = "\r\n\r\n";
  if (!client.find(endOfHeaders)) { client.stop(); LastMeterValue.solar = 0; return; }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, client);
  if (error) { Log_Add(5002); client.stop(); LastMeterValue.solar = 0; return; }

  LastMeterValue.temperature = doc["temperature"].as<float>() * 100;
  LastMeterValue.solar       = doc["energy_since_boot"].as<int>();
  client.stop();
}

int32_t MeterValue_get_from_remote()
{
  DLOGLN("MeterValue_get_from_remote Connecting...");
  WiFiClient client;
  client.setTimeout(2); // seconds, see myStrom_get_Meter_value()
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

  MeterValue newVal = MeterValue_getLast();
  resetMeterValue(newVal);
  newVal.meter_value_180 = doc["meter_value_180"];
  newVal.timestamp       = doc["timestamp"];
  newVal.temperature     = doc["temperature"];
  newVal.solar           = doc["solar"];
  newVal.meter_value_280 = doc["meter_value_280"];
  MeterValue_setLast(newVal);
  client.stop();
  timestamp_telegram = timestamp;
  return meter_value_180_i32;
}

void Telegram_ResetReceiveBuffer()
{
  telegram_receive_bufferIndex = 0;
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
      Log_Add(3001);
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
    if (!parsed)
    {
      Log_Add(3006);
    }
    if (parsed)
    {
      last_telegram_parsed = millis(); // reset watchdog
      last_silence_log     = 0;         // restart alert cycle if meter comes back online
      if (!startup_print_done) {
        startup_print_done = true;
        Serial.printf("\n--- Startup Meter Diagnostic ---\n");
        Serial.printf("Protocol : %s\n", last_detected_protocol == TelegramProtocol::SML ? "SML" : "IEC 62056-21");
        Serial.printf("Meter    : %s\n", meter_model.isEmpty() ? "(unknown)" : meter_model.c_str());
        Serial.printf("1.8.0    : %lu (0.1 Wh)\n", (unsigned long)LastMeterValue.meter_value_180);
        Serial.printf("2.8.0    : %lu (0.1 Wh)\n", (unsigned long)LastMeterValue.meter_value_280);
        Serial.printf("P Import : %lu W\n", (unsigned long)LastMeterValue.power_import);
        Serial.printf("P Export : %lu W\n", (unsigned long)LastMeterValue.power_export);
        Serial.printf("P Net    : %ld W\n", (long)LastMeterValue.net_power);
        Serial.printf("--- End Diagnostic ---\n\n");

      }
    }
    if(last_detected_protocol != prev_detected_protocol){
      if(last_detected_protocol == TelegramProtocol::SML) Log_AddWithoutTransmit(3003);
      else if(last_detected_protocol == TelegramProtocol::IEC) Log_AddWithoutTransmit(3004);

      prev_detected_protocol = last_detected_protocol;
    }
    if (parsed && temperature_object.isChecked() && !mystrom_PV_object.isChecked())
      LastMeterValue.temperature = current_temperature;

    Telegram_ResetReceiveBuffer();
  }
}

// ---------------------------------------------------------------------------
// Webclient_crc32
// CRC-32 (IEEE 802.3), identical to PHP hash('crc32b') and zlib crc32().
// Chainable: start with crc = 0 and pass the previous result for the next block.
// ---------------------------------------------------------------------------
static uint32_t Webclient_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
  crc = ~crc;
  while (len--)
  {
    crc ^= *data++;
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1)));
  }
  return ~crc;
}

// ---------------------------------------------------------------------------
// Webclient_read_response
// Reads a backend response: returns the HTTP status code (0 if none arrived
// in time) and the body. Header lines are offered to the HTTP Date time
// fallback. The body is read byte-wise up to Content-Length (or until the
// server closes), so no read waits for a trailing newline. Chunked transfer
// encoding is not decoded — the backend always sends Content-Length.
// ---------------------------------------------------------------------------
static int Webclient_read_response(WiFiClientSecure &client, String &body, unsigned long timeout_ms)
{
  const unsigned int MAX_BODY = 1024; // backend responses are < 300 bytes
  int  status         = 0;
  long content_length = -1;
  bool headers_done   = false;
  body = "";
  unsigned long deadline = millis() + timeout_ms;

  while (millis() < deadline)
  {
    if (headers_done && (body.length() >= MAX_BODY ||
                         (content_length >= 0 && (long)body.length() >= content_length))) break;
    if (!client.available())
    {
      if (!client.connected()) break;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (headers_done) { body += (char)client.read(); continue; }

    String line = client.readStringUntil('\n'); // header lines always end with \r\n
    DLOGLN(line);
    line.trim();
    if (status == 0)
    {
      if (line.startsWith("HTTP/")) status = line.substring(line.indexOf(' ') + 1).toInt();
      continue;
    }
    if (line.isEmpty())
    {
      headers_done = true;
      if (status != 200) break; // error bodies are not evaluated
      continue;
    }
    if (line.substring(0, 15).equalsIgnoreCase("Content-Length:")) content_length = line.substring(15).toInt();
    Time_setFromHttpDate(line); // no-op once NTP is synced or for non-Date lines
  }
  return status;
}

// ---------------------------------------------------------------------------
// Webclient_ack_valid
// True if a backend response acknowledges exactly the payload that was sent:
// "bytes" and "crc32" (see payload_ack() in the backend) must match. A server
// that answers HTTP 200 without running the backend script cannot produce
// this, so the device never discards data on a bare 200.
// ---------------------------------------------------------------------------
static bool Webclient_ack_valid(JsonDocument &doc, size_t bytes, uint32_t crc)
{
  char crc_hex[9];
  snprintf(crc_hex, sizeof(crc_hex), "%08lx", (unsigned long)crc);
  return (doc["bytes"] | -1L) == (long)bytes && strcmp(doc["crc32"] | "", crc_hex) == 0;
}

void Webclient_send_log_to_backend()
{
  if (backend_host.isEmpty()) { DLOGLN("No backend host configured, skipping"); Log_AddWithoutTransmit(1023); return; }
  DLOGLN("Send Log to Backend");
  Log_AddWithoutTransmit(1019);
  WiFiClientSecure client;
  client.setHandshakeTimeout(10); // 10 s SSL handshake timeout
  if (UseSslCert_object.isChecked()) client.setCACert(FullCert);
  else client.setInsecure();

  if (!client.connect(backend_host.c_str(), 443, 10000)) { DLOGLN("Connection to server failed"); Log_Add(4000); call_backend_successfull = false; return; }

  uint8_t *logDataBuffer = (uint8_t *)malloc(LOG_BUFFER_SIZE * sizeof(LogEntry));
  if (!logDataBuffer) { DLOGLN("Log buffer allocation failed"); Log_Add(1002); call_backend_successfull = false; return; }
  // Regular uploads carry only the entries since the last acknowledged upload;
  // /sendLog_Task and /sendboth_Task request the whole ring (g_log_upload_full).
  bool full = g_log_upload_full;
  g_log_upload_full = false;
  uint32_t logUpto;
  size_t logBufferSize = Log_CopyForUpload(logDataBuffer, full, logUpto);

  String logHeader  = "POST " + String(backend_path) + "log.php";
  logHeader += "?ID=" + String(backend_ID) + "&IP=" + String(IPlastOctet);
  logHeader += "&serial=" + SerialScan_activeLabel();
  logHeader += "&fw=" + String(FIRMWARE_VERSION);
  logHeader += "&cfg=" + String(CONFIG_VERSION);
  if (!meter_model.isEmpty()) {
    String encoded = meter_model;
    encoded.replace(" ", "%20");
    logHeader += "&model=" + encoded;
  }
  logHeader += " HTTP/1.1\r\nHost: " + backend_host + "\r\n";
  logHeader += "X-Auth-Token: " + String(backend_token) + "\r\n";
  logHeader += "Content-Type: application/octet-stream\r\n";
  logHeader += "Content-Length: " + String(logBufferSize) + "\r\n";
  logHeader += "Connection: close\r\n\r\n";
  DLOGLN(logHeader);

  client.print(logHeader);
  client.write(logDataBuffer, logBufferSize);
  uint32_t logCrc = Webclient_crc32(0, logDataBuffer, logBufferSize);
  free(logDataBuffer);

  // The log only counts as sent if the backend acknowledges exactly this payload.
  String body;
  int  status = Webclient_read_response(client, body, 15000);
  bool logOk  = false;
  if (status == 200)
  {
    JsonDocument doc;
    logOk = deserializeJson(doc, body) == DeserializationError::Ok &&
            Webclient_ack_valid(doc, logBufferSize, logCrc);
    if (!logOk) Log_Add(4005);
  }
  else Log_Add(4003);

  // No b_send_log_to_backend = false here: the task cleared it before the
  // buffer was copied; an entry logged since then must keep it set.
  if (logOk) { DLOGLN("Log successfully sent"); Log_MarkSent(logUpto); Log_AddWithoutTransmit(1020); last_backend_success = millis(); }
  else b_send_log_to_backend = true;
  call_backend_successfull = logOk;
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
//
// The buffer is only cleared if the backend acknowledges exactly this payload
// (bytes + CRC32 in the JSON response). The URL also carries fw/hw, so a
// confirmed response may contain an fw_update offer for OtaPull.
// ---------------------------------------------------------------------------
void Webclient_send_meter_values_to_backend()
{
  Log_AddWithoutTransmit(1005);
  Log_AddWithoutTransmit(LOG_VALUE_COUNT_BASE + MeterValue_Num());
  DLOGLN("call_backend_V2");
  last_call_backend = millis();

  if (backend_host.isEmpty()) { DLOGLN("No backend host configured, skipping"); Log_AddWithoutTransmit(1023); call_backend_successfull = true; last_backend_success = millis(); return; }
  // No early return without values: the call (with an empty payload) is also
  // how the device learns about an assigned firmware update and shows up as
  // alive in the backend, even if it receives no telegrams.

  WiFiClientSecure client;
  // connect(..., 10000) below sets the 10 s socket timeout (each TLS write);
  // the TLS handshake has its own limit, 120 s by default.
  client.setHandshakeTimeout(30); // seconds
  if (UseSslCert_object.isChecked()) client.setCACert(FullCert);
  else client.setInsecure();

  if (!client.connect(backend_host.c_str(), 443, 10000)) { DLOGLN("Connection to server failed"); Log_Add(4000); call_backend_successfull = false; return; }

  // Hold the buffer lock from computing the ranges until the buffer is cleared
  // (or the send failed). handle_MeterValue_store() defers new stores meanwhile.
  xSemaphoreTake(Sema_MeterBuffer, portMAX_DELAY);

  // Determine which byte ranges of the buffer actually contain data.
  //
  // Buffer layout:
  //   [0 .. override_i-1]              TAF7  entries (ascending from front)
  //   [override_i .. NON_override_i]   empty gap
  //   [NON_override_i+1 .. Size-1]     TAF14 entries (descending from back)
  //
  // When the buffer is full/overflowed the two pointers have crossed and
  // there is no gap — send the entire buffer as one contiguous block.
  const size_t entrySize = MeterValue_EntrySize();
  size_t taf7_bytes, taf14_offset, taf14_bytes;

  if (meter_value_buffer_full || meter_value_buffer_overflow)
  {
    taf7_bytes   = (size_t)Meter_Value_Buffer_Size * entrySize;
    taf14_offset = 0;
    taf14_bytes  = 0;
  }
  else
  {
    taf7_bytes   = (size_t)meter_value_override_i * entrySize;
    taf14_offset = (size_t)(meter_value_NON_override_i + 1) * entrySize;
    taf14_bytes  = (size_t)(Meter_Value_Buffer_Size - 1 - meter_value_NON_override_i) * entrySize;
  }
  size_t totalPayload = taf7_bytes + taf14_bytes;

  String header  = "POST " + String(backend_path);
  header += "?ID=" + String(backend_ID);
  header += "&chipTemp=" + String(temperatureRead(), 1);
  header += "&uptime=" + String(millis() / 60000);
  header += "&time=" + String(Time_getFormattedTime());
  // Self-describing field manifest — the backend uses this to parse the binary payload.
  // Format: fields=ts,m180[,temp][,solar][,m280]
  header += "&" + MeterValue_BuildFieldsParam();
  header += "&heap=" + String(ESP.getFreeHeap());
  header += "&transmittedValues=" + String(MeterValue_Num());
  header += "&IP=" + String(IPlastOctet);
  header += "&fw=" FIRMWARE_VERSION "&hw=" FIRMWARE_TARGET; // backend answers with fw_update if another version is assigned
  header += " HTTP/1.1\r\n";
  header += "Host: " + backend_host + "\r\n";
  header += "X-Auth-Token: " + String(backend_token) + "\r\n";
  header += "Content-Type: application/octet-stream\r\n";
  header += "Content-Length: " + String(totalPayload) + "\r\n";
  header += "Connection: close\r\n\r\n";

  client.print(header);

  // Send filled ranges in 1 KB chunks so partial TLS writes don't silently
  // drop the tail. WiFiClientSecure::write() may return less than requested.
  // sendRange() logs 4001 and returns false if write() returns 0 (connection
  // lost mid-transfer). Abort without clearing the buffer so data is retried.
  // The CRC32 of everything sent is compared with the backend acknowledgement.
  uint32_t crc = 0;
  auto sendRange = [&](const uint8_t* data, size_t len) -> bool {
    const size_t CHUNK = 1024;
    size_t offset = 0;
    while (offset < len)
    {
      size_t toSend = min(CHUNK, len - offset);
      size_t sent   = client.write(data + offset, toSend);
      if (sent == 0) { DLOGLN("write() returned 0 — aborting"); Log_Add(4001); return false; }
      crc = Webclient_crc32(crc, data + offset, sent);
      offset += sent;
    }
    return true;
  };

  // TAF7 range (front of buffer), then TAF14 range (back of buffer).
  // The backend sorts all entries by timestamp, so the order here doesn't matter.
  if ((taf7_bytes  > 0 && !sendRange(MeterValueBuffer,                taf7_bytes)) ||
      (taf14_bytes > 0 && !sendRange(MeterValueBuffer + taf14_offset, taf14_bytes)))
  {
    xSemaphoreGive(Sema_MeterBuffer);
    client.stop();
    call_backend_successfull = false;
    return;
  }

  // Clear the buffer only if the backend acknowledges exactly this payload —
  // HTTP 200 alone is not enough (a misconfigured server may answer 200
  // without storing anything).
  String body;
  int  status = Webclient_read_response(client, body, 15000);
  bool ok     = false;
  JsonDocument doc;
  if (status == 200)
  {
    ok = deserializeJson(doc, body) == DeserializationError::Ok &&
         Webclient_ack_valid(doc, totalPayload, crc);
    if (!ok) Log_Add(4004);
  }
  else Log_Add(4002);

  if (ok)
  {
    DLOGLN("MeterValues successfully sent");
    MeterValues_clear_Buffer();
    last_call_backend    = millis();
    last_backend_success = millis();
    Log_AddWithoutTransmit(1021);
  }
  xSemaphoreGive(Sema_MeterBuffer);

  // Only a confirmed response may offer a firmware update. Every caller of
  // this function holds Sema_Backend, as OtaPull_setOffer() requires.
  if (ok) OtaPull_setOffer(doc["fw_update"]);

  call_backend_successfull = ok;
  client.stop();
}

// True if the counters in v equal the last successfully stored value
// (PrevMeterValue), i.e. storing v would only duplicate that entry.
bool MeterValue_unchangedSincePrev(const MeterValue &v)
{
  return PrevMeterValue.timestamp > 0
      && v.meter_value_180 == PrevMeterValue.meter_value_180
      && v.meter_value_280 == PrevMeterValue.meter_value_280
      && v.solar           == PrevMeterValue.solar;
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
  if (ESP.getFreeHeap() < 1000) { Log_Add(1015); DLOGLN("Not enough free heap to store another value"); return false; }

  if (mystrom_PV_object.isChecked()) myStrom_get_Meter_value();

  // Snapshot LastMeterValue once, after the blocking myStrom call, and use only
  // the snapshot below. Copied under the lock, so it never mixes two telegrams
  // (telegramTask may replace LastMeterValue on the other core meanwhile).
  MeterValue snap = MeterValue_getLast();

  // Without a 1.8.0 value the trigger stays set and the store is retried on
  // every pass, so 1200 was logged several times a minute (repeats are only
  // suppressed while no other entry comes in between). Log it at most every
  // 30 min (like 3005/3007), so it still reaches the backend without filling
  // the log ring and uploading the log with every backend call. A valid value
  // restarts the cycle.
  static unsigned long last_zero_log = 0;
  if (snap.meter_value_180 <= 0)
  {
    if (last_zero_log == 0 || millis() - last_zero_log >= 1800000UL)
    {
      Log_Add(1200);
      last_zero_log = millis();
    }
    return false;
  }
  last_zero_log = 0;

  // The telegram may have been parsed before the time sync even if the store
  // runs after it. Refuse it; the trigger stays set and the next telegram
  // (seconds later) carries a valid timestamp.
  if (!Time_isPlausible(snap.timestamp)) { Log_Add(1027); return false; }

  // Skip storing if the value has not changed since the last successful store.
  if (MeterValue_unchangedSincePrev(snap))
  {
    // Timestamp unchanged = no new telegram received (e.g. meter reader slipped off).
    // Never re-store a frozen reading regardless of elapsed time — this prevents
    // an infinite loop where TAF7 keeps re-queuing the same stale entry after the
    // backend acknowledges it and the buffer is cleared.
    if (snap.timestamp == PrevMeterValue.timestamp)
    {
      Log_AddWithoutTransmit(1201);
      return false;
    }
    // New telegram received but counter value unchanged. Override stores (TAF7,
    // boot snapshot, manual TAF6) store it anyway, right away, so every TAF7
    // mark gets its own entry. TAF14 stores it again only after 15 min as an
    // "alive" heartbeat.
    if (override == false && millis() - last_meter_value_successful < 900000)
    {
      Log_AddWithoutTransmit(1201);
      return false;
    }
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
    // Always the real telegram time, also for TAF7: flooring it to the TAF7
    // mark would trade real precision for a cosmetically clean timestamp.
    uint32_t storeTimestamp = snap.timestamp;

    // Write the current reading into the packed buffer at the selected slot.
    // Fields that are disabled (temperature, solar, obis280) are silently
    // skipped inside MeterValue_write() — they consume no bytes.
    MeterValue_write(write_i,
      storeTimestamp,
      snap.meter_value_180,
      snap.temperature,
      snap.solar,
      snap.meter_value_280
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
    Log_Add(1016);
    MeterValue_trigger_non_override = false; // prevent immediate re-trigger
    DLOGLN("Buffer full, no space to write new value!");
    return false;
  }

  PrevMeterValue = snap; // remember last stored value for change detection
  return true;
}

// ---------------------------------------------------------------------------
// handle_wifi_setup_lifecycle — owns both ends of an in-progress /wifiSetup
// connection attempt: dropping the AP a couple of seconds after a confirmed
// success, and aborting a failed attempt from the main loop rather than only
// when the browser happens to poll /wifiStatus.
//
// ESP32 has a single radio shared between AP and STA, which must run on the
// same channel. While the STA is actively negotiating (and, with a wrong
// password, repeatedly retrying the auth handshake), that radio time is
// taken away from the softAP, which is exactly the AP the user's browser is
// connected to — so the very request needed to detect and abort the failed
// attempt can itself be delayed or dropped. Running the check unconditionally
// in the main loop means the retrying STA connection gets aborted as soon as
// possible, regardless of whether the config portal is currently reachable.
// ---------------------------------------------------------------------------
void handle_wifi_setup_lifecycle()
{
  if (g_apStopAt > 0 && millis() >= g_apStopAt)
  {
    g_apStopAt = 0;
    DLOGLN("WiFi-Setup: AP hold time elapsed, handing over to IotWebConf.");
    iotWebConf.forceApMode(false); // _forceApMode was true -> triggers changeState(Connecting)
  }

  static unsigned long failSince = 0;

  if (!g_wifiSetupPending) { failSince = 0; return; }

  wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED)
  {
    failSince = 0;
    Webserver_CheckWifiSetupFallback(); // handled by /wifiStatus normally; this is the fallback if the client never polls again
    return;
  }

  bool definitiveFailure = status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL;
  if (definitiveFailure) {
    if (failSince == 0) failSince = millis();
  } else {
    failSince = 0; // status recovered (e.g. back to idle/disconnected mid-handshake) - don't act on a blip
  }

  // ESP32 can report WL_CONNECT_FAILED transiently for a moment during a
  // handshake that still goes on to succeed - require the failure to persist
  // for a bit before treating it as final, so a real connection isn't killed
  // by this loop-driven watchdog on its very first bad reading.
  bool failureConfirmed = definitiveFailure && (millis() - failSince > 3000);
  bool timedOut         = millis() - g_wifiSetupStartedAt > 20000;
  if (!failureConfirmed && !timedOut) return;

  failSince           = 0;
  g_wifiSetupPending = false;
  g_wifiSetupFailed  = true;
  WiFi.disconnect();
  Log_Add(7002);
  DLOGLN("WiFi-Setup: connection attempt failed, freeing radio for AP.");
  Webserver_ClearPendingWifiCredentials();
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
      Log_Add(1008);
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
      Log_Add(1009);
      wifi_connected = false;
    }
    else
    {
      // Still offline — periodically trigger reconnect attempt
      if (millis() - last_reconnect_attempt > 60000)
      {
        last_reconnect_attempt = millis();
        // Skip while a /wifiSetup attempt is in flight: it manages
        // forceApMode() itself, and IotWebConf::forceApMode(false) clears
        // the internal _forceApMode flag even when it can't leave ApMode
        // right away (it just logs and stays put) - if that flag is cleared
        // out from under us, IotWebConf's own AP timeout can tear the AP
        // down entirely (WiFi.mode(WIFI_STA)) once _apTimeoutMs elapses,
        // right in the middle of our own connection attempt.
        if (!g_wifiSetupPending)
        {
          iotwebconf::NetworkState state = iotWebConf.getState();
          if (state == iotwebconf::NetworkState::ApMode)
          {
            DLOGLN("AP mode: triggering reconnect via IotWebConf");
            iotWebConf.forceApMode(false);
          }
          // In Connecting state: let IotWebConf manage its own reconnect cycle.
          // Calling WiFi.begin() here would reset an in-progress association
          // attempt, making reconnection harder (especially with hidden SSIDs).
        }
      }
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

// ---------------------------------------------------------------------------
// handle_telegram_watchdog
// Logs 3007 (no serial data) or 3005 (no valid telegram) if nothing has been
// received for 5 minutes (e.g. optical reader slipped off the meter). Repeats
// every 30 minutes while the meter remains silent. Like any non-routine code,
// the entry marks the log for upload with the next backend call.
// Resets automatically when a new telegram arrives (see handle_Telegram_receive).
// ---------------------------------------------------------------------------
void handle_telegram_watchdog()
{
  if (DebugFromOtherClient_object.isChecked()) return; // no serial telegram expected in remote mode

  bool alertDue = (last_silence_log == 0 || millis() - last_silence_log >= 1800000UL);

  if (millis() - lastByteTime >= 300000UL)
  {
    // No bytes at all on the serial interface — optical reader likely disconnected.
    if (alertDue)
    {
      Log_Add(3007);
      last_silence_log = millis();
    }
    return; // 3005 (parse failure) would be redundant — suppress it
  }

  if (millis() - last_telegram_parsed >= 300000UL)
  {
    // Bytes arriving but no valid telegram parsed for 5 min (baud/parity mismatch?).
    if (alertDue)
    {
      Log_Add(3005);
      last_silence_log = millis();
    }
  }
}

// ---------------------------------------------------------------------------
// handle_backend_recovery
// Reconnects WiFi if no backend call has been acknowledged for 30 min while
// WiFi reports connected — covers a link that is up but passes no traffic.
// IotWebConf notices the disconnect and connects again. Unlike a restart this
// keeps the RAM buffer, so a backend outage costs no data: the reconnect just
// repeats until the backend answers again (log 7000).
// With a long backend interval the limit grows to two intervals + 5 min, so a
// regular gap between two calls never counts as "no success". The calls run
// at minutes divisible by the interval, so the longest gap is 60 min.
// ---------------------------------------------------------------------------
void handle_backend_recovery()
{
  static unsigned long last_reconnect = 0;

  if (!wifi_connected || g_wifiSetupPending || backend_host.isEmpty()) return;

  unsigned long interval_min = (unsigned long)min(cached_backend_call_minute, 60);
  unsigned long limit_ms     = max(30UL, 2 * interval_min + 5) * 60UL * 1000UL;
  if (millis() - last_backend_success < limit_ms) return;
  if (last_reconnect != 0 && millis() - last_reconnect < limit_ms) return;

  last_reconnect = millis();
  Log_Add(7000);
  DLOGLN("No acknowledged backend call within the limit - reconnecting WiFi");
  WiFi.disconnect();
}

// ---------------------------------------------------------------------------
// supervisorTask
// Restarts the device if it is stuck without rebooting by itself:
//  - loop() has not started a new pass for 10 min (hung main loop), or
//  - a meter value or log upload task has held Sema_Backend for 10 min; while
//    it hangs, no backend call gets through anymore.
// Longest regular durations (all limits set in code), well below 10 min:
//  - meter upload: DNS 15 s + connect 10 s + TLS handshake 30 s + payload in
//    1 KB writes of max. 10 s each (16 KB: 160 s, only on a nearly dead link)
//    + response 15 s = ~4 min worst case, normally a few seconds
//  - log upload: ~1 min (handshake 10 s, one write)
//  - loop(): OTA pull up to ~7 min (Sema_Backend 30 s, DNS 15 s, connect
//    15 s, handshake 30 s, request/headers 75 s, download capped at 4 min);
//    meter upload after a config change or espota update (Sema_Backend 15 s
//    + upload as above) ~4.5 min; Check Remote FW Update page and each
//    post-update validation attempt ~3.5 min; connection test page ~1 min;
//    myStrom fetch a few seconds; optical flash test 20 s
// The cause is kept in NVS across the restart and logged on boot (1120-1122).
// During a post-update validation (OtaPull_validate()) any restart makes the
// bootloader boot the previous firmware (6028).
// Values still in the RAM buffer are lost, but a stuck device would not
// deliver them either.
// ---------------------------------------------------------------------------
static void Supervisor_restart(int cause)
{
  // Fallback: restart after 5 s even if the NVS write below never returns
  // (e.g. the hung code holds the NVS lock). esp_timer callbacks run in their
  // own high-priority task, independent of that lock.
  esp_timer_create_args_t args = {};
  args.callback = [](void *) { esp_restart(); };
  args.name     = "recovery_restart";
  esp_timer_handle_t fallback = nullptr;
  if (esp_timer_create(&args, &fallback) == ESP_OK) esp_timer_start_once(fallback, 5 * 1000000ULL);

  Preferences prefs;
  prefs.begin("recovery", false);
  prefs.putInt("cause", cause);
  prefs.end();
  esp_restart();
}

void supervisorTask(void *pvParameters)
{
#ifdef TEST_SELF_RECOVERY
  static const unsigned long LIMIT_MS = 60UL * 1000; // test build: 1 min instead of 10
#else
  static const unsigned long LIMIT_MS = 10UL * 60 * 1000;
#endif
  for (;;)
  {
    vTaskDelay(pdMS_TO_TICKS(10000));
    // Read the timestamps before millis(): read the other way round, a value
    // updated in between would be newer than "now" and underflow the difference.
    unsigned long loop_hb       = g_loop_heartbeat;
    unsigned long meter_started = g_meter_task_started;
    unsigned long log_started   = g_log_task_started;
    unsigned long now           = millis();

    if (now - loop_hb > LIMIT_MS)                             Supervisor_restart(1120);
    if (meter_started != 0 && now - meter_started > LIMIT_MS) Supervisor_restart(1121);
    if (log_started   != 0 && now - log_started   > LIMIT_MS) Supervisor_restart(1122);
  }
}

void handle_call_backend()
{
  if (wifi_connected)// && millis() - wifi_reconnection_time > 60000)
  {
    if ((last_call_backend == 0 && Time_isSynced()) || // first boot: fire as soon as the time is valid
        (!call_backend_successfull && millis() - last_call_backend > 180000) ||
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

void handle_remote_ota()
{
  // hint   = the backend offered another firmware version (fw_update in a
  //          confirmed response, see OtaPull_setOffer())
  // manual = the user confirmed "Installieren" on the Remote FW Update page
  // There is no time-based fallback: the backend is called in every interval,
  // even without stored values, so an assigned update always arrives.
  bool manual = g_ota_manual_install_requested;
  bool hint   = g_ota_check_requested;

  if (!manual && !hint) return;

  g_ota_manual_install_requested = false;
  g_ota_check_requested          = false;
  Log_AddWithoutTransmit(manual ? 6022 : 6014);
  OtaPull_check(manual);
}

unsigned long last_meter_value_store   = 0;
unsigned long last_meter_value_trigger = 0;

void handle_MeterValue_store()
{
  if (!MeterValue_trigger_override && !MeterValue_trigger_non_override) return; // nothing to do
  if (millis() - last_meter_value_store < 1000) return;
  last_meter_value_store = millis();

  // Upload in flight: defer instead of writing into a buffer that is about to
  // be cleared. The trigger stays set, so the store is retried in ~1 s.
  if (xSemaphoreTake(Sema_MeterBuffer, 0) != pdTRUE) { Log_AddWithoutTransmit(1026); return; }

  bool retVal = false;
  if (MeterValue_trigger_override == true)
  {
    // Remember the entry stored before this one: if it is a TAF14 reading taken
    // less than TAF7_REPLACE_WINDOW_S before the TAF7 value at the mark, it is removed
    // below so the TAF7 mark isn't preceded by a near-duplicate.
    static const uint32_t TAF7_REPLACE_WINDOW_S = 10;
    uint32_t prevTs       = PrevMeterValue.timestamp;
    bool     prevWasTaf14 = !last_store_was_override && prevTs > 0;

    retVal = MeterValue_store(true);
    if (retVal == true)
    {
      last_taf7_meter_value   = millis();
      last_store_was_override = true;

      // Runs only after a successful store, i.e. at most once per TAF7 mark —
      // a refused store that is retried can never remove further entries.
      // The slot is checked against the remembered timestamp, so nothing else
      // is removed if the buffer was uploaded/cleared or wrapped in between.
      uint32_t newTs = PrevMeterValue.timestamp;
      int      lastTaf14_i = meter_value_NON_override_i + 1;
      if (MeterValue_trigger_is_taf7 && prevWasTaf14 &&
          newTs >= prevTs && newTs - prevTs < TAF7_REPLACE_WINDOW_S &&
          lastTaf14_i < Meter_Value_Buffer_Size)
      {
        uint32_t ts, m180, solar, m280;
        int32_t  temp;
        MeterValue_read(lastTaf14_i, ts, m180, temp, solar, m280);
        if (ts == prevTs)
        {
          MeterValue_ClearSlot(lastTaf14_i);
          meter_value_NON_override_i = lastTaf14_i;
          Log_AddWithoutTransmit(1025);
        }
      }
    }
    // Refused although nothing changed since the last stored value — only
    // possible when no new telegram arrived since then (frozen reading). That
    // value already is the reading at this moment, so drop the trigger instead
    // of retrying and storing it late once a telegram comes in.
    else if (MeterValue_unchangedSincePrev(LastMeterValue)) MeterValue_trigger_override = false;
    if (!MeterValue_trigger_override || retVal) MeterValue_trigger_is_taf7 = false;
  }
  else if (MeterValue_trigger_non_override == true)
  {
    retVal = MeterValue_store(false);
    if (retVal == true) { last_taf14_meter_value = millis(); last_store_was_override = false; }
  }
  xSemaphoreGive(Sema_MeterBuffer);

  if (retVal == true)
  {
    Log_AddWithoutTransmit(1017);
    last_taf14_meter_value      = millis();
    last_meter_value_successful = millis();
    MeterValue_trigger_override     = false;
    MeterValue_trigger_non_override = false;
  }
}

void handle_MeterValue_trigger()
{
  // No TAF triggers before the system time is valid (NTP or backend Date
  // header, see time_utils): readings would carry
  // 1970 timestamps (rejected by the backend anyway), the TAF7 marks would be
  // meaningless, and a trigger left pending would later store a "TAF7 mark" value
  // at an arbitrary time. The boot snapshot below takes the first value once
  // the time is valid.
  if (!Time_isSynced()) return;

  // Boot snapshot: fire once as soon as the first telegram has been received.
  if (!boot_snapshot_done && startup_print_done)
  {
    boot_snapshot_done              = true;
    Log_AddWithoutTransmit(1024);
    MeterValue_trigger_override     = true;
    MeterValue_trigger_non_override = false;
    MeterValue_trigger_is_taf7      = false;
    return;
  }

  // True during the first 15s after a TAF7 mark (e.g. HH:00/:15/:30/:45).
  // TAF14 is held back for the whole window (see below) so it cannot land a few
  // seconds after the TAF7 snapshot; a TAF14 reading right before the mark is
  // removed once the TAF7 value is stored (see handle_MeterValue_store()).
  bool taf7WindowOpen = taf7_b_object.isChecked() &&
                         ((Time_getEpochTime() - 1) % ((unsigned long)cached_taf7_param * 60) < 15);

  if (MeterValue_trigger_override == false &&
      taf7WindowOpen &&
      (millis() - last_taf7_meter_value > 45000))
  {
    // TAF7 always stores its own value at the mark. A TAF14 reading
    // stored less than 10 s before it is removed after the TAF7 store succeeded
    // (see handle_MeterValue_store()).
    Log_AddWithoutTransmit(1010);
    last_taf7_meter_value = millis(); // one TAF7 trigger per TAF7 mark
    MeterValue_trigger_override     = true;
    MeterValue_trigger_non_override = false;
    MeterValue_trigger_is_taf7      = true;
  }
  else if (MeterValue_trigger_override == false &&
           MeterValue_trigger_non_override == false &&
           taf14_b_object.isChecked() &&
           !taf7WindowOpen &&
           millis() - last_meter_value_successful >= 1000UL * (unsigned long)cached_taf14_param &&
           millis() - last_taf14_meter_value      >= 1000UL * (unsigned long)cached_taf14_param)
  {
    if (meter_value_buffer_full == true) { last_taf14_meter_value = millis(); Log_Add(1206); }
    else { Log_AddWithoutTransmit(1011); MeterValue_trigger_non_override = true; }
  }
}

void loop()
{
  g_loop_heartbeat = millis(); // watched by supervisorTask()
  iotWebConf.doLoop();

  ArduinoOTA.handle();
  handle_temperature();
  // handle_Telegram_receive();  // handled by dedicated FreeRTOS task
  handle_wifi_setup_lifecycle();
  handle_check_wifi_connection();
  handle_MeterValue_trigger();
  handle_MeterValue_store();
  handle_telegram_watchdog();
  handle_call_backend();
  handle_backend_recovery();
  OtaPull_validate();
  handle_remote_ota();
}

void Param_configSaved()
{
  DLOGLN("Configuration was updated.");
  redirect_to_sysinfo = true;
  Led_update_Blink();
  Webclient_splitHostAndPath(String(backend_endpoint), backend_host, backend_path);
  Log_Add(1003);

  cached_taf7_param           = max(1, atoi(taf7_param));
  cached_taf14_param          = max(1, atoi(taf14_param));
  cached_backend_call_minute  = max(1, atoi(backend_call_minute));

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
    Log_Add(1004);
    MeterValue_init_Buffer();
  }
}

void Led_update_Blink()
{
  if (led_blink_object.isChecked()) iotWebConf.enableBlink();
  else { iotWebConf.disableBlink(); digitalWrite(LED_BUILTIN, LOW); }
}
