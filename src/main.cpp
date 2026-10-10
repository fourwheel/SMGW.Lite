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
#include "meter_store.h"
#include "sensors.h"
#include "webserver_backend.h"
#include "supervisor.h"

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


// Task watermark vars
int watermark_meter_buffer = 0;
int watermark_log_buffer   = 0;
int watermark_telegram     = 0;

// WiFi vars
bool wifi_connected;
int IPlastOctet = -1;


// -- Forward declarations.
void handle_call_backend();
void handle_remote_ota();
void Led_update_Blink();
void OTA_setup();
void Param_configSaved();

void Webserver_LocationHrefsysinfo(int delay);


DNSServer dnsServer;
WebServer server(80);

HardwareSerial mySerial(1);


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
