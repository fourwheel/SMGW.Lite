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
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include "NTPClient.h"
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

const String BUILD_TIMESTAMP = String(__DATE__) + " " + String(__TIME__);

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

// -- Configuration specific key. The value should be modified if config structure was changed.
#define CONFIG_VERSION "2906"

#define FIRMWARE_VERSION "1.2.4"

// -- When CONFIG_PIN is pulled to ground on startup, the Thing will use the initial
//      password to build an AP. (E.g. in case of lost password)
#define CONFIG_PIN 5

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

#define STATUS_PIN LED_BUILTIN

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
// int   cached_tafdyn_absolute        = 100;    // used by handle_dynTaf (#if 0)
// float cached_tafdyn_multiplicator   = 3.0f;
// Backend vars
bool call_backend_successfull = true;
bool redirect_to_sysinfo = false;
bool          g_wifiSetupPending  = false;
unsigned long g_apStopAt          = 0;    // millis() timestamp to stop AP, 0 = not scheduled
SemaphoreHandle_t Sema_Backend;       // Mutex / Semaphore for backend call
volatile bool ota_active = false;     // set during OTA to block new backend calls
static TaskHandle_t h_meter_task = NULL;
static TaskHandle_t h_log_task   = NULL;
unsigned long last_call_backend = 0; // 0 = never called; set to millis() on first attempt

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
void handle_check_wifi_connection();
void handle_MeterValue_trigger();
void handle_telegram_watchdog();
void handle_MeterValue_store();
void handle_temperature();
void Led_update_Blink();
bool MeterValue_store(bool override);
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
void Webserver_HandleCertUpload();
void Webserver_HandleRoot();
void Webserver_HandleSysInfo();
void Webserver_LocationHrefsysinfo(int delay);
void Webserver_SetCert();

void Webserver_TestBackendConnection();
void Webserver_TestBackendConnectionRun();

DNSServer dnsServer;
WebServer server(80);

HardwareSerial mySerial(1);

bool MeterValue_trigger_override     = false;
bool MeterValue_trigger_non_override = false;
bool startup_print_done              = false;
bool boot_snapshot_done              = false;
unsigned long last_meter_value_successful = 0;
unsigned long last_taf7_meter_value       = 0;
unsigned long last_taf14_meter_value      = 0;
unsigned long last_reconnect_attempt      = 0;
unsigned long last_telegram_received      = 0;
unsigned long last_urgent_log_call        = 0;

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
// char dynTaf_enabled[STRING_LEN]; // used by dynTaf_enabled_object (#if 0)
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
// IotWebConfCheckboxParameter dynTaf_enabled_object = IotWebConfCheckboxParameter("Dyn. Tarif (experimental)", "activate_IEC_Parser", dynTaf_enabled, STRING_LEN, false);

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



bool b_send_log_to_backend = false;
bool b_send_log_urgent     = false; // triggers immediate log.php call, independent of meter value cycle


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
    unsigned long t = millis();
    while (client.available() == 0 && millis() - t < 5000) {}
    String response = "";
    while (client.available()) response += client.readString();
    auth_ok = response.indexOf("200") != -1;
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
      Log_AddEntry(8002);
      Webserver_LocationHrefsysinfo();
      return;
    }

    File file = SPIFFS.open("/cert.pem", FILE_WRITE);
    if (file)
    {
      file.println(cert);
      file.close();
      Log_AddEntry(8002);
      Webserver_LocationHrefsysinfo();
    }
    else
    {
      Log_AddEntry(8003);
      Webserver_LocationHrefsysinfo();
      server.send(500, "text/plain", "Cannot Open File!");
    }
  }
  else
  {
    Log_AddEntry(8004);
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
  Log_AddEntry(8001);
}

void Webclient_Send_Meter_Values_to_backend_Task(void *pvParameters)
{
  if (ota_active) { h_meter_task = NULL; vTaskDelete(NULL); return; }
  if (xSemaphoreTake(Sema_Backend, portMAX_DELAY))
  {
    if (!ota_active) Webclient_send_meter_values_to_backend();
    xSemaphoreGive(Sema_Backend);
  }
  watermark_meter_buffer = uxTaskGetStackHighWaterMark(NULL);
  h_meter_task = NULL;
  vTaskDelete(NULL);
}

void Webclient_Send_Log_to_backend_Task(void *pvParameters)
{
  b_send_log_to_backend = false;
  if (ota_active) { h_log_task = NULL; vTaskDelete(NULL); return; }
  if (xSemaphoreTake(Sema_Backend, portMAX_DELAY))
  {
    if (!ota_active) Webclient_send_log_to_backend();
    xSemaphoreGive(Sema_Backend);
  }
  watermark_log_buffer = uxTaskGetStackHighWaterMark(NULL);
  h_log_task = NULL;
  vTaskDelete(NULL);
}


void telegramTask(void * pvParameters) {
  for(;;) {
    if (SerialScan_consumePending()) {
      SerialScan_run();
    } else if (!SerialScan_isRunning()) {
      handle_Telegram_receive();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    watermark_telegram = uxTaskGetStackHighWaterMark(NULL);
  }
}

void setup()
{
  Sema_Backend = xSemaphoreCreateMutex();
  LogBuffer_reset();
  last_telegram_received = millis(); // start watchdog timer from boot
  Log_AddEntry(1001);
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

  // staticDelay already set above (before Param_setup)

  vTaskPrioritySet(NULL, 3);
  xTaskCreate(telegramTask, "TelegramBot", 2048, NULL, 0, NULL);
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
static int32_t smlToWatt(uint64_t raw, int8_t scaler) {
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

  // 3a. Extract meter serial (OBIS 0-0:96.1.0) for meter_model if not yet known
  if (meter_model.isEmpty()) {
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
                meter_model = String((char)a) + String((char)b) + String((char)c);
                break;
              }
            }
            if (meter_model.isEmpty()) {
              char hex[3];
              for (int k = 0; k < slen; k++) {
                sprintf(hex, "%02X", buffer[j+1+k]);
                if (k) meter_model += ' ';
                meter_model += hex;
              }
            }
            break;
          }
        }
        break;
      }
    }
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
    if (PrevMeterValue.meter_value_180 > 0 && temp180 < PrevMeterValue.meter_value_180) {
      Log_AddEntry(1207);
      return false;
    }
    if (found280 && PrevMeterValue.meter_value_280 > 0 && temp280 < PrevMeterValue.meter_value_280) {
      Log_AddEntry(1207);
      return false;
    }
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
    // Single struct assignment: meter_value_180 goes from old value to temp180,
    // never through 0, eliminating the race window.
    LastMeterValue = newVal;
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
    const char *eol = strstr(telegram_str, "\r\n");
    size_t lineLen = eol ? (size_t)(eol - telegram_str) : 0;
    if (lineLen >= 4) {
      char mfr[4]; strncpy(mfr, telegram_str + 1, 3); mfr[3] = '\0';
      meter_model = mfr;
      if (lineLen > 5) {
        char ident[64];
        size_t identLen = lineLen - 5 < sizeof(ident) - 1 ? lineLen - 5 : sizeof(ident) - 1;
        strncpy(ident, telegram_str + 5, identLen); ident[identLen] = '\0';
        meter_model += ' '; meter_model += ident;
      }
    }
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
  if (PrevMeterValue.meter_value_180 > 0 && new180 < PrevMeterValue.meter_value_180) {
    Log_AddEntry(1207);
    return false;
  }

  float v280_raw = 0.0f;
  bool has280 = parseIecObis("1-0:2.8.0", &v280_raw);
  uint32_t new280 = has280 ? (uint32_t)(v280_raw * 10000.0f) : 0;
  if (has280 && PrevMeterValue.meter_value_280 > 0 && new280 < PrevMeterValue.meter_value_280) {
    Log_AddEntry(1207);
    return false;
  }

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
  LastMeterValue = newVal;

  return true;
}


void myStrom_get_Meter_value()
{
  if (!mystrom_PV_object.isChecked()) { LastMeterValue.solar = 0; return; }

  DLOGLN(F("myStrom_get_Meter_value Connecting..."));
  WiFiClient client;
  client.setTimeout(1000);
  if (!client.connect(mystrom_PV_IP, 80)) { Log_AddEntry(5000); DLOGLN(F("myStrom_get_Meter_value Connection failed")); LastMeterValue.solar = 0; return; }

  DLOGLN(F("myStrom_get_Meter_value Connected!"));
  client.println(F("GET /report HTTP/1.0"));
  client.print(F("Host: ")); client.println(mystrom_PV_IP);
  client.println(F("Connection: close"));
  if (client.println() == 0) { Log_AddEntry(5001); client.stop(); LastMeterValue.solar = 0; return; }

  char status[32] = {0};
  client.readBytesUntil('\r', status, sizeof(status));
  if (strcmp(status + 9, "200 OK") != 0) { client.stop(); LastMeterValue.solar = 0; return; }

  char endOfHeaders[] = "\r\n\r\n";
  if (!client.find(endOfHeaders)) { client.stop(); LastMeterValue.solar = 0; return; }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, client);
  if (error) { Log_AddEntry(5002); client.stop(); LastMeterValue.solar = 0; return; }

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
    if (!parsed)
    {
      Log_AddEntry(3006);
    }
    if (parsed)
    {
      last_telegram_received = millis(); // reset watchdog
      last_urgent_log_call   = 0;       // restart alert cycle if meter comes back online
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
      if(last_detected_protocol == TelegramProtocol::SML) Log_AddEntry(3003);
      else if(last_detected_protocol == TelegramProtocol::IEC) Log_AddEntry(3004);

      prev_detected_protocol = last_detected_protocol;
    }
    if (parsed && temperature_object.isChecked() && !mystrom_PV_object.isChecked())
      LastMeterValue.temperature = current_temperature;

    Telegram_ResetReceiveBuffer();
  }
}

void Webclient_send_log_to_backend()
{
  if (backend_host.isEmpty()) { DLOGLN("No backend host configured, skipping"); Log_AddEntry(1023); return; }
  DLOGLN("Send Log to Backend");
  Log_AddEntry(1019);
  WiFiClientSecure client;
  client.setHandshakeTimeout(10); // 10 s SSL handshake timeout
  if (UseSslCert_object.isChecked()) client.setCACert(FullCert);
  else client.setInsecure();

  if (!client.connect(backend_host.c_str(), 443, 10000)) { DLOGLN("Connection to server failed"); Log_AddEntry(4000); call_backend_successfull = false; return; }

  size_t logBufferSize = LOG_BUFFER_SIZE * sizeof(LogEntry);
  uint8_t *logDataBuffer = (uint8_t *)malloc(logBufferSize);
  if (!logDataBuffer) { DLOGLN("Log buffer allocation failed"); call_backend_successfull = false; return; }
  memcpy(logDataBuffer, Log_getRawBuffer(), logBufferSize);

  String logHeader  = "POST " + String(backend_path) + "log.php";
  logHeader += "?ID=" + String(backend_ID) + "&token=header&IP=" + String(IPlastOctet);
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
  free(logDataBuffer);

  bool logOk = false;
  unsigned long log_deadline = millis() + 15000;
  while ((client.connected() || client.available()) && millis() < log_deadline)
  {
    if (client.available())
    {
      String line = client.readStringUntil('\n');
      DLOGLN(line);
      if (line.startsWith("HTTP/1.1 200")) { DLOGLN("Log successfully sent"); Log_AddEntry(1020); b_send_log_to_backend = false; logOk = true; break; }
      else b_send_log_to_backend = true;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  call_backend_successfull = logOk;
  if (!logOk) Log_AddEntry(4003);
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

  if (backend_host.isEmpty()) { DLOGLN("No backend host configured, skipping"); Log_AddEntry(1023); call_backend_successfull = true; return; }
  if (MeterValue_Num() == 0) { DLOGLN("Zero Values to transmit"); call_backend_successfull = true; return; }

  WiFiClientSecure client;
  client.setTimeout(10000); // 10 s read timeout
  if (UseSslCert_object.isChecked()) client.setCACert(FullCert);
  else client.setInsecure();

  if (!client.connect(backend_host.c_str(), 443, 10000)) { DLOGLN("Connection to server failed"); Log_AddEntry(4000); call_backend_successfull = false; return; }

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
  auto sendRange = [&](const uint8_t* data, size_t len) -> bool {
    const size_t CHUNK = 1024;
    size_t offset = 0;
    while (offset < len)
    {
      size_t toSend = min(CHUNK, len - offset);
      size_t sent   = client.write(data + offset, toSend);
      if (sent == 0) { DLOGLN("write() returned 0 — aborting"); Log_AddEntry(4001); return false; }
      offset += sent;
    }
    return true;
  };

  // TAF7 range (front of buffer), then TAF14 range (back of buffer).
  // The backend sorts all entries by timestamp, so the order here doesn't matter.
  if (taf7_bytes  > 0 && !sendRange(MeterValueBuffer,                taf7_bytes))  { client.stop(); call_backend_successfull = false; return; }
  if (taf14_bytes > 0 && !sendRange(MeterValueBuffer + taf14_offset, taf14_bytes)) { client.stop(); call_backend_successfull = false; return; }

  bool ok = false;
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
        ok = true;
        MeterValues_clear_Buffer();
        last_call_backend = millis();
        Log_AddEntry(1021);
        break; // don't wait for the rest of the response — we have what we need
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  call_backend_successfull = ok;
  if (!ok) Log_AddEntry(4002);
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

  // Snapshot LastMeterValue once. myStrom_get_Meter_value() above is a blocking
  // network call; while it is blocked, telegramTask (Core 0) may update
  // LastMeterValue concurrently. Taking a snapshot after the blocking call and
  // using it exclusively below prevents reading an inconsistent (partially-written)
  // struct from the other core.
  MeterValue snap = LastMeterValue;

  if (snap.meter_value_180 <= 0) { Log_AddEntry(1200); return false; }

  // Skip storing if the value has not changed since the last successful store.
  if (snap.meter_value_180 == PrevMeterValue.meter_value_180
    && snap.meter_value_280 == PrevMeterValue.meter_value_280
    && snap.solar           == PrevMeterValue.solar)
  {
    // Timestamp unchanged = no new telegram received (e.g. meter reader slipped off).
    // Never re-store a frozen reading regardless of elapsed time — this prevents
    // an infinite loop where TAF7 keeps re-queuing the same stale entry after the
    // backend accepts it with HTTP 200 and clears the buffer.
    if (snap.timestamp == PrevMeterValue.timestamp)
    {
      Log_AddEntry(1201);
      return false;
    }
    // New telegram received but counter value unchanged — apply time-based cooldown.
    // TAF14 (non-override) waits 15 min, TAF7 (override) only 1 min.
    if ((override == false && millis() - last_meter_value_successful < 900000) ||
        (override == true  && millis() - last_meter_value_successful < 60000))
    {
      Log_AddEntry(1201);
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
    // Write the current reading into the packed buffer at the selected slot.
    // Fields that are disabled (temperature, solar, obis280) are silently
    // skipped inside MeterValue_write() — they consume no bytes.
    MeterValue_write(write_i,
      snap.timestamp,
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
    Log_AddEntry(1016);
    MeterValue_trigger_non_override = false; // prevent immediate re-trigger
    DLOGLN("Buffer full, no space to write new value!");
    return false;
  }

  PrevMeterValue = snap; // remember last stored value for change detection
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
      // Still offline — periodically trigger reconnect attempt
      if (millis() - last_reconnect_attempt > 60000)
      {
        last_reconnect_attempt = millis();
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
// Fires status 1024 and an urgent log.php call if no telegram has been
// received for 5 minutes (e.g. optical reader slipped off the meter).
// Repeats every 30 minutes while the meter remains silent.
// Resets automatically when a new telegram arrives (see handle_Telegram_receive).
// ---------------------------------------------------------------------------
void handle_telegram_watchdog()
{
  if (millis() - last_telegram_received < 300000UL) return; // within 5-min grace period

  // Fire on first alert (last_urgent_log_call == 0) or every 30 min thereafter.
  if (last_urgent_log_call == 0 || millis() - last_urgent_log_call >= 1800000UL)
  {
    Log_AddEntry(3005);
    b_send_log_urgent    = true;
    last_urgent_log_call = millis();
  }
}

void handle_call_backend()
{
  if (wifi_connected)// && millis() - wifi_reconnection_time > 60000)
  {
    // Urgent log call — triggered by alerts like "no telegram for 5 min".
    // Runs independently of the meter value backend cycle.
    if (b_send_log_urgent)
    {
      b_send_log_urgent = false;
      Webclient_Send_Log_to_backend_wrapper();
    }

    if ((last_call_backend == 0 && Time_getEpochTime() > 0) || // first boot: fire as soon as NTP is ready
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

unsigned long last_meter_value_store   = 0;
unsigned long last_meter_value_trigger = 0;

// ---------------------------------------------------------------------------
// handle_dynTaf — commented out, feature temporarily disabled
// Triggers a non-override buffer store whenever instantaneous net power
// changes significantly since the last stored reading.
//
// Signal:  net power = power_import - power_export (from OBIS 1.7.0 / 2.7.0),
//          falling back to net_power (OBIS 16.7.0).
//          Returns without triggering if the meter does not transmit any
//          instantaneous power value.
//
// Reference: PrevMeterValue — the reading that was last successfully stored.
//            Updated automatically by MeterValue_store() on every successful
//            store (TAF7, TAF14, or DynTaf), so the baseline always reflects
//            what is already in the backend.
//
// Trigger conditions (OR-linked, both configurable):
//   Absolute: |currentPower - lastPower| >= effectiveAbsolute (W)
//   Ratio:    magnitude changed by factor >= effectiveMultiplicator
//
// Both thresholds scale with time since the last successful store (any kind).
// At DYNTAF_FULL_THRESHOLD_MS the configured values apply unchanged.
// Below that, thresholds scale up linearly so recent TAF7/TAF14 stores raise
// the bar for an immediate DynTaf re-trigger.
// ---------------------------------------------------------------------------
#if 0
static const uint32_t DYNTAF_FULL_THRESHOLD_MS = 5000; // ms after which base thresholds apply

void handle_dynTaf()
{
  if (LastMeterValue.timestamp == 0) return;
  if (meter_value_buffer_full) { Log_AddEntry(1022); return; }

  // Scale thresholds based on time since any successful store.
  uint32_t elapsed = millis() - last_meter_value_successful;
  float scale = (elapsed >= DYNTAF_FULL_THRESHOLD_MS)
                  ? 1.0f
                  : (float)DYNTAF_FULL_THRESHOLD_MS / (float)(elapsed + 1);

  int32_t effectiveAbsolute      = (int32_t)((float)cached_tafdyn_absolute * scale);
  float   effectiveMultiplicator = 1.0f + (cached_tafdyn_multiplicator - 1.0f) * scale;

  // Derive current net power from instantaneous telegram values.
  // Prefer explicit import/export (1.7.0 / 2.7.0) over signed net (16.7.0).
  int32_t currentPower;
  if (LastMeterValue.power_import > 0 || LastMeterValue.power_export > 0)
    currentPower = (int32_t)LastMeterValue.power_import - (int32_t)LastMeterValue.power_export;
  else if (LastMeterValue.net_power != 0)
    currentPower = LastMeterValue.net_power;
  else
    return; // meter does not transmit instantaneous power — DynTaf not available

  // Derive reference power from the last successfully stored reading.
  int32_t lastPower;
  if (PrevMeterValue.power_import > 0 || PrevMeterValue.power_export > 0)
    lastPower = (int32_t)PrevMeterValue.power_import - (int32_t)PrevMeterValue.power_export;
  else
    lastPower = PrevMeterValue.net_power;

  // Absolute delta trigger
  int32_t delta = currentPower - lastPower;
  if (delta < 0) delta = -delta;
  bool triggerAbs = (delta >= effectiveAbsolute);

  // Multiplicator trigger: fire when power magnitude changed by factor N
  bool triggerMulti = false;
  if (effectiveMultiplicator > 1.0f)
  {
    int32_t absLast = lastPower    < 0 ? -lastPower    : lastPower;
    int32_t absCurr = currentPower < 0 ? -currentPower : currentPower;
    if (absLast > 0)
    {
      float ratio = (float)absCurr / (float)absLast;
      triggerMulti = (ratio >= effectiveMultiplicator || ratio <= 1.0f / effectiveMultiplicator);
    }
  }

  if (triggerAbs || triggerMulti)
  {
    Log_AddEntry(1018);
    MeterValue_trigger_non_override = true;
    last_dyntaf_store = millis();
  }
}
#endif

void handle_MeterValue_store()
{
  if (!MeterValue_trigger_override && !MeterValue_trigger_non_override) return; // nothing to do
  if (millis() - last_meter_value_store < 1000) return;
  last_meter_value_store = millis();

  bool retVal = false;
  if (MeterValue_trigger_override == true)
  {
#if 0
    // Remove the most recent non-override entry if it was stored shortly before
    // this TAF7 trigger — keeps the grid mark uncluttered.
    static const unsigned long TAF7_REPLACE_WINDOW_MS = 5000UL;
    if (!last_store_was_override &&
        last_meter_value_successful > 0 &&
        millis() - last_meter_value_successful < TAF7_REPLACE_WINDOW_MS &&
        meter_value_NON_override_i < Meter_Value_Buffer_Size - 1)
    {
      meter_value_NON_override_i++;
      memset(MeterValueBuffer + MeterValue_Offset(meter_value_NON_override_i), 0, MeterValue_EntrySize());
      Log_AddEntry(1025);
    }
#endif
    retVal = MeterValue_store(true);
    if (retVal == true) last_taf7_meter_value = millis();
    // if (retVal == true) { last_taf7_meter_value = millis(); last_store_was_override = true; }
  }
  else if (MeterValue_trigger_non_override == true)
  {
    retVal = MeterValue_store(false);
    if (retVal == true) last_taf14_meter_value = millis();
    // if (retVal == true) { last_taf14_meter_value = millis(); last_store_was_override = false; }
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

// Minimum plausible epoch for a reliable NTP sync (2020-01-01 00:00:00 UTC).
static const unsigned long EPOCH_MIN_PLAUSIBLE = 1577836800UL;

void handle_MeterValue_trigger()
{
  // Boot snapshot: fire once as soon as the first telegram has been received
  // AND the system time is reliably NTP-synced (not the 1970 default).
  if (!boot_snapshot_done && startup_print_done && Time_getEpochTime() > EPOCH_MIN_PLAUSIBLE)
  {
    boot_snapshot_done              = true;
    Log_AddEntry(1024);
    MeterValue_trigger_override     = true;
    MeterValue_trigger_non_override = false;
    return;
  }

  if (MeterValue_trigger_override == false &&
      taf7_b_object.isChecked() &&
      ((Time_getEpochTime() - 1) % ((unsigned long)cached_taf7_param * 60) < 15) &&
      (millis() - last_taf7_meter_value > 45000))
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
  // else if (MeterValue_trigger_override == false && MeterValue_trigger_non_override == false)
  // {
  //   if (dynTaf_enabled_object.isChecked()) handle_dynTaf();
  // }
}

void loop()
{
  iotWebConf.doLoop();

  if (g_apStopAt > 0 && millis() >= g_apStopAt)
  {
    g_apStopAt = 0;
    DLOGLN("WiFi-Setup: AP hold time elapsed, handing over to IotWebConf.");
    iotWebConf.forceApMode(false); // _forceApMode was true -> triggers changeState(Connecting)
  }

  ArduinoOTA.handle();
  handle_temperature();
  // handle_Telegram_receive();  // handled by dedicated FreeRTOS task
  handle_check_wifi_connection();
  handle_MeterValue_trigger();
  handle_MeterValue_store();
  handle_telegram_watchdog();
  handle_call_backend();
}

void Webserver_HandleSysInfo()
{
  String s;
  s.reserve(9000);
  s += R"rawliteral(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>SmartMeterLite – Details</title>)rawliteral";
  s += HTML_STYLE_MODERN;
  s += R"rawliteral(</head>
<body>
<div class="logo">&#9889; SmartMeterLite</div>
<a class="back" href="/">&#8592; Home</a>

<a class="cfg-link" href="config">
<span class="cfg-icon">&#9881;</span>
<span class="cfg-text">
<strong>Konfigurationsseite</strong>
<small>Werte mit &#9999; k&ouml;nnen dort ge&auml;ndert werden</small>
</span>
</a>

<div class="card">
<div class="card-title">Last Meter Value <small style="font-weight:400;color:#888;">&mdash; green = in wire format</small></div>
<div class="tbl"><table>)rawliteral";
  {
    const char* W = " style='background:#d4edda'";
    const char* N = "";
    const char* w280  = config_obis280_enabled     ? W : N;
    const char* wTemp = config_temperature_enabled ? W : N;
    const char* wSol  = config_solar_enabled        ? W : N;
    s += "<tr><th>Time</th><th>1.8.0</th><th>2.8.0</th><th>Temp</th><th>MyStrom</th></tr>";
    s += String("<tr><td") + W    + ">" + String(Time_getEpochTime() - LastMeterValue.timestamp) + "s ago</td>"
       + "<td" + W    + ">" + String(LastMeterValue.meter_value_180) + "</td>"
       + "<td" + w280  + ">" + String(LastMeterValue.meter_value_280) + "</td>"
       + "<td" + wTemp + ">" + String(LastMeterValue.temperature / 100.0) + "\xc2\xb0""C</td>"
       + "<td" + wSol  + ">" + String(LastMeterValue.solar) + "</td></tr>";
  }
  s += R"rawliteral(</table></div>
<div class="btns" style="margin-top:.6rem;">
<a class="btn btn-s" href="StoreMeterValue">Store Meter Value Now (TAF6)</a>
<a class="btn btn-s" href="showLastMeterValue">Last Value (JSON)</a>
</div>
</div>

<div class="card">
<div class="card-title">Meter Value Buffer</div>
<div class="kv"><span class="kl">Used / Size</span>)rawliteral";
  s += String(MeterValue_Num()) + " / " + String(Meter_Value_Buffer_Size);
  {
    int cfgKB = atoi(Meter_Value_Buffer_Size_Char);
    s += meter_value_buffer_is_auto
         ? " <small>(auto &mdash; " + String(BUFFER_REFERENCE_BYTES / 1024) + " KB reference budget)</small>"
         : " <small>(manual &mdash; " + String(cfgKB) + " KB budget)</small>";
  }
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Budget</span>)rawliteral";
  {
    int cfgKB = atoi(Meter_Value_Buffer_Size_Char);
    size_t budget = (cfgKB <= 0) ? BUFFER_REFERENCE_BYTES : (size_t)cfgKB * 1024;
    s += String(budget) + " bytes &nbsp;|&nbsp; " + String(MeterValue_EntrySize()) + " bytes/slot &nbsp;&rarr;&nbsp; " + String(MeterValue_slots_from_budget(budget)) + " slots";
  }
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Capacity (free heap)</span>)rawliteral";
  {
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
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Wire format</span>)rawliteral";
  s += MeterValue_BuildFieldsParam();
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Entry size</span>)rawliteral";
  s += String(MeterValue_EntrySize()) + " bytes";
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Free heap</span>)rawliteral";
  s += String(ESP.getFreeHeap() / 1024) + " KB <small>(buffer uses " + String((size_t)Meter_Value_Buffer_Size * MeterValue_EntrySize() / 1024) + " KB)</small>";
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Temperature</span>)rawliteral";
  s += String(config_temperature_enabled ? "yes" : "no");
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">MyStrom / Solar</span>)rawliteral";
  s += String(config_solar_enabled ? "yes" : "no");
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Infeed (2.8.0)</span>)rawliteral";
  s += String(config_obis280_enabled ? "yes" : "no");
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">i override / non-override</span>)rawliteral";
  s += String(meter_value_override_i) + " / " + String(meter_value_NON_override_i);
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Buffer Overflow</span>)rawliteral";
  s += String(meter_value_buffer_overflow);
  s += R"rawliteral(</div>
<div class="kv last"><span class="kl">Buffer Full</span>)rawliteral";
  s += String(meter_value_buffer_full);
  s += R"rawliteral(</div>
<div class="btns" style="margin-top:.6rem;">
<a class="btn btn-s" href="showMeterValues">Show Meter Values</a>
<a class="btn btn-s" href="MeterValue_Num2">Count (alternative)</a>
</div>
</div>

<div class="card">
<div class="card-title">Telegram Parse Config</div>
<div class="kv"><span class="kl">Protocol (auto-detected)</span>)rawliteral";
  s += Telegram_protocol_to_string(last_detected_protocol);
  s += R"rawliteral(</div>
<div class="kv last"><span class="kl">Serial config (active)</span>)rawliteral";
  s += SerialScan_activeLabel();
  s += R"rawliteral(</div>
<div class="btns" style="margin-top:.6rem;">
<a class="btn btn-s" href="showTelegram">Show Telegram</a>
<a class="btn btn-s" href="showTelegramRaw">Raw</a>
<a class="btn btn-s" href="showTelegramAnalysis">Analysis</a>
<a class="btn btn-s" href="/serialScan">Baud/Parity Scan</a>
</div>
</div>

<div class="card">
<div class="card-title">Backend Config</div>
<div class="kv"><span class="kl e">Backend Endpoint</span>)rawliteral";
  s += backend_endpoint;
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Host</span>)rawliteral";
  s += backend_host;
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Path</span>)rawliteral";
  s += backend_path;
  s += R"rawliteral(</div>
<div class="kv"><span class="kl e">Call Minute</span>)rawliteral";
  s += String(atoi(backend_call_minute));
  s += R"rawliteral(</div>
<div class="kv"><span class="kl e">Backend ID</span>)rawliteral";
  s += backend_ID;
  s += R"rawliteral(</div>
<div class="kv"><span class="kl e">Use SSL Cert</span>)rawliteral";
  s += (UseSslCert_object.isChecked() ? "true" : "false");
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Last Call Ago</span>)rawliteral";
  s += String((millis() - last_call_backend) / 60000) + " min";
  s += R"rawliteral(</div>
<div class="kv last"><span class="kl">Static Delay</span>)rawliteral";
  s += String(staticDelay) + " s";
  s += R"rawliteral(</div>
<div class="btns" style="margin-top:.6rem;">
<a class="btn btn-s" href="setCert">Set Cert</a>
<a class="btn btn-s" href="testBackendConnection">Test Connection</a>
<a class="btn btn-s" href="sendLog_Task">Send Log</a>
<a class="btn btn-s" href="sendMeterValues_Task">Send Meter Values</a>
<a class="btn btn-s" href="sendboth_Task">Send Both</a>
</div>
</div>

<div class="card">
<div class="card-title">TAF Config</div>
<div class="kv"><span class="kl e">TAF 7</span>)rawliteral";
  s += (taf7_b_object.isChecked() ? "activated" : "not activated");
  s += R"rawliteral(</div>
<div class="kv"><span class="kl e">TAF 7 Minute</span>)rawliteral";
  s += String(atoi(taf7_param));
  s += R"rawliteral(</div>
<div class="kv"><span class="kl e">TAF 14</span>)rawliteral";
  s += (taf14_b_object.isChecked() ? "activated" : "not activated");
  s += R"rawliteral(</div>
<div class="kv"><span class="kl e">TAF 14 Interval</span>)rawliteral";
  s += String(atoi(taf14_param)) + " s";
  s += R"rawliteral(</div>)rawliteral";
  // Dyn TAF rows — commented out (feature disabled)
  // s += R"rawliteral(<div class="kv"><span class="kl">Dyn TAF</span>)rawliteral";
  // s += "activated (hardcoded)";
  // s += R"rawliteral(</div><div class="kv"><span class="kl">Dyn TAF Absolute Delta</span>)rawliteral";
  // s += String(cached_tafdyn_absolute) + " W";
  // s += R"rawliteral(</div><div class="kv last"><span class="kl">Dyn TAF Multiplicator</span>)rawliteral";
  // s += String(cached_tafdyn_multiplicator, 1) + " x";
  // s += R"rawliteral(</div>)rawliteral";
  s += R"rawliteral(
</div>
<div class="card">
<div class="card-title">Additional Meters &amp; Sensors</div>
<div class="kv"><span class="kl e">Temperature Sensor</span>)rawliteral";
  s += (temperature_object.isChecked() ? "activated" : "deactivated");
  s += R"rawliteral(</div>
<div class="kv"><span class="kl e">MyStrom (solar)</span>)rawliteral";
  s += (mystrom_PV_object.isChecked() ? "activated" : "deactivated");
  s += R"rawliteral(</div>
<div class="kv last"><span class="kl e">MyStrom IP</span>)rawliteral";
  s += mystrom_PV_IP;
  s += R"rawliteral(</div>
</div>

<div class="card">
<div class="card-title">Helpers</div>
<div class="kv"><span class="kl e">Set Device Offline</span>)rawliteral";
  s += (DebugSetOffline_object.isChecked() ? "activated" : "deactivated");
  s += R"rawliteral(</div>
<div class="kv"><span class="kl e">Values from other SMGWLite</span>)rawliteral";
  s += (DebugFromOtherClient_object.isChecked() ? "activated" : "deactivated");
  s += R"rawliteral(</div>
<div class="kv last"><span class="kl e">Remote Client IP</span>)rawliteral";
  s += String(DebugMeterValueFromOtherClientIP);
  s += R"rawliteral(</div>
<div class="btns" style="margin-top:.6rem;">
<a class="btn" href="PinAssistant">PIN Assistant</a>
<a class="btn" href="PinAssistantDeluxe">PIN Assistant Deluxe</a>
</div>
</div>

<div class="card">
<div class="card-title">System Info</div>
<div class="kv"><span class="kl">Meter Model</span>)rawliteral";
  s += meter_model.isEmpty() ? "unknown" : meter_model;
  s += R"rawliteral(</div>
<div class="kv"><span class="kl e">LED Blink</span>)rawliteral";
  s += (led_blink_object.isChecked() ? "activated" : "deactivated");
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Watermark Main</span>)rawliteral";
  s += String(uxTaskGetStackHighWaterMark(NULL));
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Watermark Meter Values</span>)rawliteral";
  s += String(watermark_meter_buffer);
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Watermark Logs</span>)rawliteral";
  s += String(watermark_log_buffer);
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Watermark Telegram</span>)rawliteral";
  s += String(watermark_telegram);
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Uptime</span>)rawliteral";
  s += Time_formatUptime();
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Reset Reason</span>)rawliteral";
  s += Log_get_reset_reason();
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">System Time (UTC)</span>)rawliteral";
  s += String(Time_getFormattedTime()) + " / " + String(Time_getEpochTime());
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Firmware Version</span>)rawliteral";
  s += String(FIRMWARE_VERSION);
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Config Version</span>)rawliteral";
  s += String(CONFIG_VERSION);
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Build Time</span>)rawliteral";
  s += String(BUILD_TIMESTAMP);
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Free Heap</span>)rawliteral";
  s += String(ESP.getFreeHeap());
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Chip Temperature</span>)rawliteral";
  s += String(temperatureRead(), 1) + " &deg;C";
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">WiFi RSSI</span>)rawliteral";
  {
    int rssi = WiFi.RSSI();
    s += String(rssi) + " dBm";
    if      (rssi >= -60) s += " (good)";
    else if (rssi >= -75) s += " (ok)";
    else                  s += " (weak)";
  }
  s += R"rawliteral(</div>
<div class="kv last"><span class="kl">Log Buffer (max)</span>)rawliteral";
  s += String(LOG_BUFFER_SIZE);
  s += R"rawliteral(</div>
<div class="btns" style="margin-top:.6rem;">
<a class="btn btn-s" href="update">FW Update</a>
<a class="btn btn-d" href="restart">Restart</a>
</div>
</div>

<div class="card">
<div class="card-title">Log Buffer <small style="font-weight:400;color:#888;">(last 10 / index )rawliteral";
  s += String(Log_getIndex());
  s += R"rawliteral()</small></div>
<div class="tbl">)rawliteral";
  s += Log_BufferToString(10);
  s += R"rawliteral(</div>)rawliteral";
  s += R"rawliteral(
<div class="btns" style="margin-top:.6rem;">
<a class="btn btn-s" href="showLogBuffer">Show Full Log</a>
<a class="btn btn-s" href="resetLogBuffer">Reset Log</a>
</div>
</div>

</body></html>)rawliteral";

  server.send(200, "text/html", s);
}

// ---------------------------------------------------------------------------
// Webserver_HandleRoot – dashboard (/)
// ---------------------------------------------------------------------------
void Webserver_HandleRoot()
{
  if (iotWebConf.handleCaptivePortal()) return;

  if (redirect_to_sysinfo) {
    redirect_to_sysinfo = false;
    if (wifi_connected) {
      server.sendHeader("Location", "/sysinfo");
      server.send(302, "text/plain", "");
      return;
    }
  }

  bool isApMode        = !wifi_connected;
  bool hasReading      = LastMeterValue.timestamp > 0 && LastMeterValue.meter_value_180 > 0;
  bool hasPinPrecision = hasReading && (LastMeterValue.meter_value_180 % 10000) != 0;
  bool backendCalled   = last_call_backend > 0;
  bool backendOk       = call_backend_successfull;
  uint32_t ageS        = hasReading ? (uint32_t)(Time_getEpochTime() - LastMeterValue.timestamp) : 0;
  const uint32_t kNoTelegramThresholdS = 30;
  uint32_t backAgoMin  = backendCalled ? (millis() - last_call_backend) / 60000UL : 0;

  String s;
  s.reserve(4000);
  s += R"rawliteral(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<meta name="format-detection" content="telephone=no">
<title>SmartMeterLite</title>
<style>
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0;}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;background:#f0f2f7;min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:1.5rem 1rem 3rem;gap:1rem;color:#1a1a1a;}
.logo{font-size:1.5rem;font-weight:800;color:#1a3799;letter-spacing:-.02em;padding-top:.4rem;}
.meter-card{background:#1a3799;color:#fff;border-radius:16px;padding:2rem 2rem 1.6rem;text-align:center;width:100%;max-width:400px;box-shadow:0 4px 20px rgba(26,55,153,.22);}
.m-lbl{font-size:.76rem;opacity:.72;letter-spacing:.08em;text-transform:uppercase;margin-bottom:.6rem;}
.m-row{display:flex;align-items:center;justify-content:space-between;margin-bottom:.3rem;}
.m-row-l{display:flex;align-items:baseline;gap:.35rem;}
.m-arr{font-size:1.2rem;opacity:.8;flex-shrink:0;}
.m-val{font-size:2.3rem;font-weight:800;letter-spacing:-.03em;line-height:1;-webkit-text-fill-color:#fff;}
.m-val2{font-size:1.5rem;font-weight:700;letter-spacing:-.02em;line-height:1;-webkit-text-fill-color:#fff;opacity:.85;}
.m-unit{font-size:.78rem;opacity:.6;align-self:flex-end;padding-bottom:.1rem;}
.m-pwr{display:flex;align-items:baseline;gap:.2rem;opacity:.8;}
.m-pwr-val{font-size:1.05rem;font-weight:700;-webkit-text-fill-color:#fff;}
.m-pwr-unit{font-size:.72rem;opacity:.75;}
.m-div{border:none;border-top:1px solid rgba(255,255,255,.15);margin:.45rem 0;}
.m-age{font-size:.73rem;opacity:.48;margin-top:.35rem;}
.m-age-warn{color:#ffb300;opacity:1;font-weight:600;}
.m-net-lbl{font-size:.78rem;opacity:.55;}
.sc{background:#fff;border-radius:14px;border:1px solid #d0d8f0;padding:.9rem 1.1rem;width:100%;max-width:400px;display:flex;flex-direction:column;gap:.75rem;}
.sc-top{display:flex;align-items:center;gap:.75rem;}
.dot{width:13px;height:13px;border-radius:50%;flex-shrink:0;}
.dg{background:#4caf50;box-shadow:0 0 6px #4caf5066;}
.do{background:#ff9800;box-shadow:0 0 6px #ff980066;}
.dr{background:#f44336;box-shadow:0 0 6px #f4433666;}
.st strong{display:block;font-size:.92rem;margin-bottom:.1rem;}
.st small{font-size:.77rem;color:#666;}
.br{display:flex;gap:.7rem;width:100%;}
.btn{flex:1;display:flex;align-items:center;justify-content:center;gap:.35rem;padding:.75rem .4rem;border-radius:10px;font-size:.85rem;font-weight:700;text-decoration:none;border:2px solid #1a3799;color:#1a3799;background:#f0f2f7;text-align:center;line-height:1.3;}
.btn:hover{background:#1a3799;color:#fff;}
.lc{color:#666;font-size:.85rem;text-decoration:none;border:1px solid #d0d8f0;border-radius:8px;padding:.4rem 1rem;background:#fff;}
.lc:hover{color:#1a3799;}
.grafana-btn{display:flex;align-items:center;justify-content:center;gap:.5rem;width:100%;max-width:400px;padding:.85rem 1rem;border-radius:14px;background:#374151;color:#fff;font-size:.88rem;font-weight:700;text-decoration:none;white-space:nowrap;box-shadow:0 2px 8px rgba(0,0,0,.15);}
.grafana-btn:hover{background:#1e293b;}
footer{display:flex;flex-direction:column;align-items:center;gap:.4rem;margin-top:.5rem;padding-top:.5rem;}
footer a{color:#999;font-size:.78rem;text-decoration:none;}
footer a:hover{color:#1a3799;}
.footer-love{font-size:.78rem;color:#aaa;}
.wifi-card{background:#fff3cd;border:1px solid #ffc107;border-radius:14px;padding:1rem 1.1rem;width:100%;max-width:400px;display:flex;flex-direction:column;gap:.6rem;}
.wifi-card h3{font-size:.92rem;font-weight:700;color:#856404;margin:0;}
.wifi-card small{font-size:.77rem;color:#856404;opacity:.85;}
.wifi-form{display:flex;flex-direction:column;gap:.5rem;}
.wifi-form input[type=text],.wifi-form input[type=password]{width:100%;padding:.55rem .75rem;border-radius:8px;border:1px solid #ccc;font-size:.88rem;background:#fff;}
.wifi-form button{padding:.65rem 1rem;border-radius:8px;background:#1a3799;color:#fff;font-size:.88rem;font-weight:700;border:none;cursor:pointer;}
.wifi-form button:hover{background:#142b7a;}
</style>
</head>
<body>
<div class="logo">&#9889; SmartMeterLite</div>
)rawliteral";

  // Leistung bestimmen
  // Beide Richtungen (1.7.0 + 2.7.0) bekannt → an kWh-Zeilen hängen.
  // Sonst (nur eine Richtung oder nur 16.7.0) → standalone net-Zeile.
  bool hasPower   = false;
  bool netOnly    = false;
  int32_t importW = 0, exportW = 0, netW = 0;
  if (hasReading) {
    if (LastMeterValue.power_import > 0 && LastMeterValue.power_export > 0) {
      importW  = (int32_t)LastMeterValue.power_import;
      exportW  = (int32_t)LastMeterValue.power_export;
      hasPower = true;
    } else if (LastMeterValue.net_power != 0) {
      netW     = LastMeterValue.net_power;
      netOnly  = true;
      hasPower = true;
    } else if (LastMeterValue.power_import > 0) {
      netW     = (int32_t)LastMeterValue.power_import;
      netOnly  = true;
      hasPower = true;
    } else if (LastMeterValue.power_export > 0) {
      netW     = -(int32_t)LastMeterValue.power_export;
      netOnly  = true;
      hasPower = true;
    }
  }

  auto pwrStr = [](int32_t w) -> String {
    char buf[24];
    if (w >= 1000) snprintf(buf, sizeof(buf), "%.2f&thinsp;kW", w / 1000.0f);
    else           snprintf(buf, sizeof(buf), "%d&thinsp;W", (int)w);
    return String(buf);
  };

  // Meter reading card
  s += "<div class='meter-card'>"
       "<div class='m-lbl'>Z&#228;hlerstand</div>";

  // 1.8.0 — Bezug (↓) + Import-Leistung rechts
  if (hasReading) {
    char valBuf[32];
    uint32_t v = LastMeterValue.meter_value_180;
    snprintf(valBuf, sizeof(valBuf), "%lu,%04lu", (unsigned long)(v / 10000), (unsigned long)(v % 10000));
    s += "<div class='m-row'><div class='m-row-l'>"
         "<span class='m-arr'>&#8595;</span><span class='m-val' id='val180'>" + String(valBuf) + "</span>"
         "<span class='m-unit'>kWh</span></div>";
    if (!netOnly && importW > 0)
      s += "<div class='m-pwr'><span class='m-pwr-val' id='pwr-import'>" + pwrStr(importW) + "</span></div>";
    s += "</div>";
  } else {
    s += "<div class='m-age' style='margin:.6rem 0;'>Warte auf Telegramm&#8239;&#8230;</div>";
    if (lastByteTime > 0)
      s += "<div class='m-age-warn' style='margin-top:.4rem;font-size:.75rem;'>"
           "&#9888; Bytes empfangen, aber kein g&uuml;ltiges Telegramm &mdash; "
           "<a href='/serialScan' style='color:#ffb300;'>Baud/Parity pr&uuml;fen</a>"
           "</div>";
  }

  // Nettoleistung zwischen 1.8.0 und 2.8.0
  if (hasPower) {
    int32_t calcNet = netOnly ? netW : (importW - exportW);
    int32_t absP    = calcNet < 0 ? -calcNet : calcNet;
    const char* arr = calcNet >= 0 ? "&#8595;" : "&#8593;";
    const char* lbl = calcNet >= 0 ? "Netzbezug" : "Netzeinspeisung";
    s += "<hr class='m-div'><div class='m-row'><div class='m-row-l'>"
         "<span class='m-arr' id='net-arr'>" + String(arr) + "</span>"
         "<span class='m-pwr-val' id='net-val'>" + pwrStr(absP) + "</span>"
         "</div>"
         "<span class='m-net-lbl' id='net-lbl'>" + String(lbl) + "</span></div>";
  }

  // 2.8.0 — Einspeisung (↑) + Export-Leistung rechts, nur wenn > 0
  if (hasReading && LastMeterValue.meter_value_280 > 0) {
    char val2Buf[32];
    uint32_t v2 = LastMeterValue.meter_value_280;
    snprintf(val2Buf, sizeof(val2Buf), "%lu,%04lu", (unsigned long)(v2 / 10000), (unsigned long)(v2 % 10000));
    s += "<hr class='m-div'><div class='m-row'><div class='m-row-l'>"
         "<span class='m-arr'>&#8593;</span>"
         "<span class='m-val2' id='val280'>" + String(val2Buf) + "</span>"
         "<span class='m-unit'>kWh</span></div>";
    if (!netOnly && exportW > 0)
      s += "<div class='m-pwr'><span class='m-pwr-val' id='pwr-export'>" + pwrStr(exportW) + "</span></div>";
    s += "</div>";
  }

  if (hasReading) {
    bool recentBytes = lastByteTime > 0 && (millis() - lastByteTime) < 30000;
    if (ageS >= kNoTelegramThresholdS && recentBytes)
      s += "<div class='m-age m-age-warn' id='m-age'>&#9888; Empfange Bytes, kann Telegramm nicht lesen &mdash; "
           "<a href='/serialScan' style='color:#ffb300;'>Baud/Parity pr&uuml;fen</a></div>";
    else if (ageS >= kNoTelegramThresholdS)
      s += "<div class='m-age m-age-warn' id='m-age'>&#9888; Kein Telegramm seit " + String(ageS) + "&thinsp;s</div>";
    else
      s += "<div class='m-age' id='m-age'>Letzter Wert vor " + String(ageS) + "&thinsp;s</div>";
  }
  s += "</div>";

  // WiFi setup card — only shown in AP mode
  if (isApMode) {
    s += R"rawliteral(<div class='wifi-card' id='wifi-card'>
<h3>&#128246; Kein WLAN verbunden &ndash; Netzwerk einrichten</h3>
<small>Das Ger&auml;t ist im Access-Point-Modus. Gib dein WLAN-Passwort ein, um eine Verbindung herzustellen.</small>
<form class='wifi-form' action='/wifiSetup' method='POST'>
<input type='text'     name='ssid'     placeholder='WLAN-Name (SSID)'    autocomplete='off' autocorrect='off' autocapitalize='none' spellcheck='false'>
<input type='password' name='password' placeholder='WLAN-Passwort'       autocomplete='current-password'>
<button type='submit'>Verbinden</button>
</form>
</div>)rawliteral";
  }

  // PIN / INF status
  s += "<div class='sc' id='sc-pin'>";
  if (!hasReading) {
    s += "<div class='sc-top'><div class='dot do'></div><div class='st'><strong>Kein Messwert</strong>"
         "<small>Noch kein Telegramm empfangen.</small></div></div>"
         "<div class='br'>"
         "<a class='btn' href='/PinAssistant'>&#128274; PIN Assistant</a>"
         "<a class='btn' href='/PinAssistantDeluxe'>&#128274; PIN Assistant Deluxe</a>"
         "</div>";
  } else if (ageS >= kNoTelegramThresholdS) {
    s += "<div class='sc-top'><div class='dot do'></div><div class='st'><strong>Kein Telegramm empfangen</strong>"
         "<small>Lesekopf getrennt oder Verbindungsproblem?</small></div></div>";
  } else if (hasPinPrecision) {
    s += "<div class='sc-top'><div class='dot dg'></div><div class='st'><strong>PIN eingegeben &#8211; INF aktiv</strong>"
         "<small>Messwerte mit Nachkommastellen.</small></div></div>";
  } else {
    s += "<div class='sc-top'><div class='dot do'></div><div class='st'><strong>PIN nicht eingegeben oder INF aus</strong>"
         "<small>Bitte PIN eingeben und INF auf ON setzen.</small></div></div>"
         "<div class='br'>"
         "<a class='btn' href='/PinAssistant'>&#128274; PIN Assistant</a>"
         "<a class='btn' href='/PinAssistantDeluxe'>&#128274; PIN Assistant Deluxe</a>"
         "</div>";
  }
  s += "</div>";

  // Backend status
  s += "<div class='sc' id='sc-backend'>";
  if (!wifi_connected)
    s += "<div class='sc-top'><div class='dot dr'></div><div class='st'><strong>Kein Backend-Kontakt</strong>"
         "<small>Kein WLAN &ndash; Backend nicht erreichbar.</small></div></div>";
  else if (!backendCalled)
    s += "<div class='sc-top'><div class='dot do'></div><div class='st'><strong>Backend noch nicht kontaktiert</strong>"
         "<small>Noch kein Backend-Call durchgef&#252;hrt.</small></div></div>";
  else if (backendOk)
    s += "<div class='sc-top'><div class='dot dg'></div><div class='st'><strong>Mit Backend verbunden</strong>"
         "<small>Letzter Call vor " + String(backAgoMin) + " min.</small></div></div>";
  else
    s += "<div class='sc-top'><div class='dot dr'></div><div class='st'><strong>Backend-Fehler</strong>"
         "<small>Letzter Backend-Call fehlgeschlagen.</small></div></div>"
         "<div class='br'><a class='btn' href='/testBackendConnection'>Verbindung testen</a></div>";
  s += "</div>";

  s += R"rawliteral(<a class='grafana-btn' href='https://portal.smartmeterlite.de' target='_blank' rel='noopener'>&#128200; portal.smartmeterlite.de &rarr;</a>
<a class='lc' href='/sysinfo'>&#9881;&#65039; Konfiguration &amp; Details</a>
<footer>
<a href='https://smartmeterlite.de' target='_blank' rel='noopener'>smartmeterlite.de</a>
<a href='https://www.linkedin.com/in/laurin-vierrath/' target='_blank' rel='noopener'>&#128039; From Laurin with Love</a>
</footer>
</body></html>)rawliteral";
  // Inject live-update script — needsReload/had280 are set from current server state
  s += "<script>var needsReload=";
  s += hasReading ? "false" : "true";
  s += ";var had280=";
  s += (hasReading && LastMeterValue.meter_value_280 > 0) ? "true" : "false";
  s += R"rawliteral(;
function _set(id,txt){var e=document.getElementById(id);if(e)e.textContent=txt;}
function _pwr(w){var a=Math.abs(w);return a>=1000?(a/1000).toFixed(2)+' kW':a+' W';}
function _meter(v){return Math.floor(v/10000)+','+String(v%10000).padStart(4,'0');}
function _live(d){
  if(needsReload&&d.meter_value_180>0){location.reload();return;}
  if(!had280&&d.meter_value_280>0){location.reload();return;}
  _set('val180',_meter(d.meter_value_180));
  if(d.meter_value_280>0)_set('val280',_meter(d.meter_value_280));
  var imp=d.power_import,exp=d.power_export,net=d.net_power;
  if(imp>0&&exp>0){
    _set('pwr-import',_pwr(imp));_set('pwr-export',_pwr(exp));
    var calc=imp-exp;
    _set('net-arr',calc>=0?'↓':'↑');
    _set('net-val',_pwr(Math.abs(calc)));
    _set('net-lbl',calc>=0?'Netzbezug':'Netzeinspeisung');
  }else if(net!==0){
    _set('net-arr',net>=0?'↓':'↑');
    _set('net-val',_pwr(net));
    _set('net-lbl',net>=0?'Netzbezug':'Netzeinspeisung');
  }else if(imp>0){
    _set('net-arr','↓');_set('net-val',_pwr(imp));_set('net-lbl','Netzbezug');
  }else if(exp>0){
    _set('net-arr','↑');_set('net-val',_pwr(exp));_set('net-lbl','Netzeinspeisung');
  }
  var ageS=d.timestamp>0?Math.round(Date.now()/1000-d.timestamp):0;
  var el=document.getElementById('m-age');
  if(el){
    if(ageS>=30&&d.last_byte_age_s<30){el.className='m-age m-age-warn';el.innerHTML='&#9888; Empfange Bytes, kann Telegramm nicht lesen &mdash; <a href=\'\/serialScan\' style=\'color:#ffb300;\'>Baud\/Parity prüfen<\/a>';}
    else if(ageS>=30){el.className='m-age m-age-warn';el.textContent='⚠ Kein Telegramm seit '+ageS+' s';}
    else{el.className='m-age';el.textContent='Letzter Wert vor '+ageS+' s';}
  }
  // PIN / meter status
  var hasR=d.meter_value_180>0,hasPP=hasR&&(d.meter_value_180%10000)!==0;
  var pe=document.getElementById('sc-pin');
  if(pe){var btns="<div class='br'><a class='btn' href='/PinAssistant'>&#128274; PIN Assistant</a><a class='btn' href='/PinAssistantDeluxe'>&#128274; PIN Assistant Deluxe</a></div>";var ph;
    if(!hasR)ph="<div class='sc-top'><div class='dot do'></div><div class='st'><strong>Kein Messwert</strong><small>Noch kein Telegramm empfangen.</small></div></div>"+btns;
    else if(ageS>=30)ph="<div class='sc-top'><div class='dot do'></div><div class='st'><strong>Kein Telegramm empfangen</strong><small>Lesekopf getrennt oder Verbindungsproblem?</small></div></div>";
    else if(hasPP)ph="<div class='sc-top'><div class='dot dg'></div><div class='st'><strong>PIN eingegeben &#8211; INF aktiv</strong><small>Messwerte mit Nachkommastellen.</small></div></div>";
    else ph="<div class='sc-top'><div class='dot do'></div><div class='st'><strong>PIN nicht eingegeben oder INF aus</strong><small>Bitte PIN eingeben und INF auf ON setzen.</small></div></div>"+btns;
    pe.innerHTML=ph;}
  // Backend / WiFi status
  var be=document.getElementById('sc-backend');
  if(be){var bh;
    if(!d.wifi_connected)bh="<div class='sc-top'><div class='dot dr'></div><div class='st'><strong>Kein Backend-Kontakt</strong><small>Kein WLAN &#8211; Backend nicht erreichbar.</small></div></div>";
    else if(!d.backend_called)bh="<div class='sc-top'><div class='dot do'></div><div class='st'><strong>Backend noch nicht kontaktiert</strong><small>Noch kein Backend-Call durchgef&#252;hrt.</small></div></div>";
    else if(d.backend_ok)bh="<div class='sc-top'><div class='dot dg'></div><div class='st'><strong>Mit Backend verbunden</strong><small>Letzter Call vor "+d.backend_ago_min+" min.</small></div></div>";
    else bh="<div class='sc-top'><div class='dot dr'></div><div class='st'><strong>Backend-Fehler</strong><small>Letzter Backend-Call fehlgeschlagen.</small></div></div><div class='br'><a class='btn' href='/testBackendConnection'>Verbindung testen</a></div>";
    be.innerHTML=bh;}
  if(d.wifi_connected&&document.getElementById('wifi-card'))location.reload();
}
setInterval(function(){fetch('/showLastMeterValue').then(function(r){return r.json();}).then(_live).catch(function(){});},2000);
</script>)rawliteral";

  server.send(200, "text/html", s);
}

void Param_configSaved()
{
  DLOGLN("Configuration was updated.");
  redirect_to_sysinfo = true;
  Led_update_Blink();
  Webclient_splitHostAndPath(String(backend_endpoint), backend_host, backend_path);
  Log_AddEntry(1003);

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
    Log_AddEntry(1004);
    MeterValue_init_Buffer();
  }
}

void Led_update_Blink()
{
  if (led_blink_object.isChecked()) iotWebConf.enableBlink();
  else { iotWebConf.disableBlink(); digitalWrite(LED_BUILTIN, LOW); }
}
