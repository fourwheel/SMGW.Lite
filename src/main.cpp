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
#include "backend_client.h"

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
SemaphoreHandle_t Sema_MeterBuffer;
volatile bool ota_active          = false; // set during OTA to block new backend calls
volatile bool g_ota_check_requested = false;          // backend offered another firmware version (OtaPull_setOffer)
volatile bool g_ota_manual_install_requested = false; // user confirmed "Installieren" on the Remote FW Update page
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


// -- Forward declarations.
void handle_call_backend();
void handle_backend_recovery();
void supervisorTask(void *pvParameters);
void handle_remote_ota();
void handle_check_wifi_connection();
void handle_wifi_setup_lifecycle();
void handle_MeterValue_trigger();
void handle_MeterValue_store();
void handle_temperature();
void Led_update_Blink();
bool MeterValue_store(bool override);
bool MeterValue_unchangedSincePrev(const MeterValue &v);
void OTA_setup();
void Param_configSaved();

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
bool boot_snapshot_done              = false; // boot snapshot: fired once after first telegram + reliable time
bool last_store_was_override         = false; // true when the last successful store was a TAF7 override
unsigned long last_meter_value_successful = 0;
unsigned long last_taf7_meter_value       = 0;
unsigned long last_taf14_meter_value      = 0;
unsigned long last_reconnect_attempt      = 0;

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
