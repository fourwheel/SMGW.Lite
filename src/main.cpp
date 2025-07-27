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

const String BUILD_TIMESTAMP = String(__DATE__) + " " + String(__TIME__);

// -- Initial name of the Thing. Used e.g. as SSID of the own Access Point.
const char thingName[] = "SMGWLite";

// -- Initial password to connect to the Thing, when it creates an own Access Point.
const char wifiInitialApPassword[] = "password";

#define STRING_LEN 128
#define ID_LEN 4
#define NUMBER_LEN 5

// -- Configuration specific key. The value should be modified if config structure was changed.
#define CONFIG_VERSION "2905"

// -- When CONFIG_PIN is pulled to ground on startup, the Thing will use the initial
//      password to buld an AP. (E.g. in case of lost password)
#ifndef D2
#define D2 3
#endif

#define CONFIG_PIN D2

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

#define STATUS_PIN LED_BUILTIN

// Telegramm Vars
const uint8_t SML_SIGNATURE_START[] = {0x1b, 0x1b, 0x1b, 0x1b, 0x01, 0x01, 0x01, 0x01};
const uint8_t SML_SIGNATURE_END[] = {0x1b, 0x1b, 0x1b, 0x1b, 0x1a};
#define TELEGRAM_LENGTH 512
#define TELEGRAM_TIMEOUT_MS 30                    // timeout for telegramm in ms
size_t TelegramSizeUsed = 0;                      // actual size of stored telegram
uint8_t telegram_receive_buffer[TELEGRAM_LENGTH]; // buffer for serial data
size_t telegram_receive_bufferIndex = 0;          // positoin in serial data butter
bool readingExtraBytes = false;                   // reading additional bytes?
uint8_t extraBytes[3];                            // additional bytes after end signature
size_t extraIndex = 0;                            // index of additional bytes
unsigned long lastByteTime = 0;                   // timestamp of last received byte
unsigned long timestamp_telegram;                 // timestamp of telegram
uint8_t TELEGRAM[TELEGRAM_LENGTH];                // buffer for entire telegram

// Meter Value Vsrs

struct MeterValue
{
  uint32_t timestamp;   // 4 Bytes
  uint32_t meter_value; // 4 Bytes
  uint32_t temperature; // 4 Bytes
  uint32_t solar;       // 4 Bytes
};
MeterValue *MeterValues = nullptr;        // initiaize with nullptr
MeterValue LastMeterValue = {0, 0, 0, 0}; // initialize last meter value
MeterValue PrevMeterValue = {0, 0, 0, 0}; // initialize last meter value

bool MeterValue_trigger_override = false;
bool MeterValue_trigger_non_override = false;


unsigned long last_meter_value = 0;
unsigned long last_taf7_meter_value = 0;
unsigned long last_taf14_meter_value = 0;

int Meter_Value_Buffer_Size = 234;
bool meter_value_buffer_overflow = false;
bool meter_value_buffer_full = false;
int meter_value_override_i = 0;
int meter_value_NON_override_i = Meter_Value_Buffer_Size - 1;

float currentPower, LastPower = 0.0;

// Backend Vars
bool call_backend_successfull = true;
SemaphoreHandle_t Sema_Backend; // Mutex / Sempahore for backend call
unsigned long last_call_backend = 0;

// Temperature Vars
#define ONE_WIRE_BUS 2
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature Temp_sensors(&oneWire);
unsigned long last_temperature = 0;
bool read_temperature = false;
int current_temperature = 123;

// Log Vars
const int LOG_BUFFER_SIZE = 100;

// Tasks vars
int watermark_meter_buffer = 0;
int watermark_log_buffer = 0;

// Wifi Vars
unsigned long wifi_reconnection_time = 0;
unsigned long last_wifi_retry = 0;
unsigned long last_wifi_check;
bool wifi_connected;

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
int32_t MeterValue_get_from_telegram();
void OTA_setup();
void Param_configSaved();
void Param_setup();
String Time_formatTimestamp(unsigned long timestamp);
String Time_formatUptime();
String Time_getFormattedTime();
unsigned long Time_getEpochTime();
int Time_getMinutes();
bool Telegram_prefix_suffix_correct();
void Telegram_saveCompleteTelegram();
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
char telegram_offset[NUMBER_LEN];
char telegram_length[NUMBER_LEN];
char telegram_prefix[NUMBER_LEN];
char telegram_suffix[NUMBER_LEN];
char Meter_Value_Buffer_Size_Char[NUMBER_LEN] = "123";

IotWebConf iotWebConf(thingName, &dnsServer, &server, wifiInitialApPassword, CONFIG_VERSION);
// -- You can also use namespace formats e.g.: iotwebconf::TextParameter

IotWebConfParameterGroup groupTelegram = IotWebConfParameterGroup("groupTelegram", "Telegram Param");
IotWebConfParameterGroup groupBackend = IotWebConfParameterGroup("groupBackend", "Backend Config");
IotWebConfParameterGroup groupTaf = IotWebConfParameterGroup("groupTaf", "Taf config");
IotWebConfParameterGroup groupAdditionalMeter = IotWebConfParameterGroup("groupAdditionalMeter", "Additional Meters & Sensors");
IotWebConfParameterGroup groupSys = IotWebConfParameterGroup("groupSys", "Advanced Sys Config");
IotWebConfParameterGroup groupDebug = IotWebConfParameterGroup("groupDebug", "Debug Helpers");

IotWebConfNumberParameter telegram_offset_object = IotWebConfNumberParameter("Offset", "telegram_offset_object", telegram_offset, NUMBER_LEN, "20", "1..TELEGRAM_LENGTH", "min='1' max='TELEGRAM_LENGTH' step='1'");
IotWebConfNumberParameter telegram_length_object = IotWebConfNumberParameter("Length", "telegram_length_object", telegram_length, NUMBER_LEN, "8", "1..TELEGRAM_LENGTH", "min='1' max='TELEGRAM_LENGTH' step='1'");
IotWebConfNumberParameter telegram_prefix_object = IotWebConfNumberParameter("Prefix Begin", "telegram_prefix", telegram_prefix, NUMBER_LEN, "0", "1..TELEGRAM_LENGTH", "min='0' max='TELEGRAM_LENGTH' step='1'");
IotWebConfNumberParameter telegram_suffix_object = IotWebConfNumberParameter("Suffix Begin", "telegram_suffix", telegram_suffix, NUMBER_LEN, "100", "1..TELEGRAM_LENGTH", "min='100' max='TELEGRAM_LENGTH' step='1'");

IotWebConfTextParameter backend_endpoint_object = IotWebConfTextParameter("backend endpoint", "backend_endpoint", backend_endpoint, STRING_LEN);
IotWebConfCheckboxParameter led_blink_object = IotWebConfCheckboxParameter("LED Blink", "led_blink", led_blink, STRING_LEN, true);
IotWebConfTextParameter backend_ID_object = IotWebConfTextParameter("backend ID", "backend_ID", backend_ID, ID_LEN);
IotWebConfTextParameter backend_token_object = IotWebConfTextParameter("backend token", "backend_token", backend_token, STRING_LEN);

IotWebConfCheckboxParameter taf7_b_object = IotWebConfCheckboxParameter("Taf 7 activated", "b_taf7", b_taf7, STRING_LEN, true);
IotWebConfNumberParameter taf7_param_object = IotWebConfNumberParameter("Taf 7 minute", "taf7_param", taf7_param, NUMBER_LEN, "15", "15...1", "min='1' max='15' step='1'");
IotWebConfCheckboxParameter taf14_b_object = IotWebConfCheckboxParameter("Taf 14 activated", "b_taf14", b_taf14, STRING_LEN, true);
IotWebConfNumberParameter taf14_param_object = IotWebConfNumberParameter("Taf 14 Meter Intervall (s)", "taf14_param", taf14_param, NUMBER_LEN, "20", "1..100 s", "min='1' max='100' step='1'");
IotWebConfCheckboxParameter tafdyn_b_object = IotWebConfCheckboxParameter("Dyn Taf activated", "b_tafdyn", b_tafdyn, STRING_LEN, true);
IotWebConfNumberParameter tafdyn_absolute_object = IotWebConfNumberParameter("Dyn Taf absolute Delta", "tafdyn_absolute", tafdyn_absolute, NUMBER_LEN, "100", "Power Delta in Watts", "min='10' max='10000' step='1'");
IotWebConfNumberParameter tafdyn_multiplicator_object = IotWebConfNumberParameter("Dyn Taf multiplicator ", "tafdyn_multiplicator", tafdyn_multiplicator, NUMBER_LEN, "2", "Power n bigger or 1/n smaller", "min='1' max='10' step='0.1'");
IotWebConfNumberParameter backend_call_minute_object = IotWebConfNumberParameter("backend Call Minute", "backend_call_minute", backend_call_minute, NUMBER_LEN, "5", "", "");

IotWebConfCheckboxParameter mystrom_PV_object = IotWebConfCheckboxParameter("MyStrom PV", "mystrom_PV", mystrom_PV, STRING_LEN, false);
IotWebConfTextParameter mystrom_PV_IP_object = IotWebConfTextParameter("MyStrom PV IP", "mystrom_PV_IP", mystrom_PV_IP, STRING_LEN);
IotWebConfCheckboxParameter temperature_object = IotWebConfCheckboxParameter("Temperatur Sensor", "temperature_checkbock", temperature_checkbock, STRING_LEN, true);
IotWebConfCheckboxParameter UseSslCert_object = IotWebConfCheckboxParameter("Wirk-PKI (Use SSL Cert)", "UseSslCertValue", UseSslCertValue, STRING_LEN, false);

IotWebConfCheckboxParameter DebugSetOffline_object = IotWebConfCheckboxParameter("Set Device offline (Pretend no Wifi)", "DebugWifi", DebugSetOfflineValue, STRING_LEN, false);
IotWebConfCheckboxParameter DebugFromOtherClient_object = IotWebConfCheckboxParameter("Get Meter Value from other SMGWLite Client", "DebugFromOtherClient", DebugMeterValueFromOtherClient, STRING_LEN, false);
IotWebConfTextParameter DebugMeterValueFromOtherClientIP_object = IotWebConfTextParameter("IP to get Meter Values From", "DebugMeterValueFromOtherClientIP", DebugMeterValueFromOtherClientIP, STRING_LEN);

IotWebConfNumberParameter Meter_Value_Buffer_Size_object = IotWebConfNumberParameter("Meter_Value_Buffer_Size", "Meter_Value_Buffer_Size", Meter_Value_Buffer_Size_Char, NUMBER_LEN, "200", "1...1000", "min='1' max='1000' step='1'");

const char HTML_STYLE[] PROGMEM = R"rawliteral(
  <style>
    html, body { overflow-x: auto; max-width: 100%; }
    body { font-family: sans-serif; margin: 1em;   column-width: 600px; column-gap: 40px;}
    table { display: block; overflow-x: auto; white-space: nowrap; border-collapse: collapse; max-width: 100%;   }
    th, td { border: 1px solid #ccc; padding: 6px 12px; text-align: left; }
    ul { list-style-type: square; padding-left: 20px; }
    li { margin-bottom: 0.3em; }
    a { color: #0066cc; text-decoration: none; }
    a:hover { text-decoration: underline; }
    font[color="red"] { color: red; }
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

  return timeinfo.tm_min; // Extract minutes (0–59)
}

struct LogEntry
{
  unsigned long timestamp; // unix timestamp
  unsigned long uptime;
  int statusCode;
};
LogEntry logBuffer[LOG_BUFFER_SIZE];
int logIndex = -1;
void LogBuffer_reset()
{
  for (int i = 0; i < LOG_BUFFER_SIZE; ++i)
  {
    logBuffer[i].timestamp = 0;
    logBuffer[i].uptime = 0;
    logBuffer[i].statusCode = 0;
  }
  logIndex = -1;
}
void Log_AddEntry(int statusCode)
{
  // Increase Log Index
  // Remember, it is a ringbuffer and overwrites oldest entries
  logIndex = (logIndex + 1) % LOG_BUFFER_SIZE;

  unsigned long uptimeSeconds = millis() / 60000; // Uptime in seconds

  logBuffer[logIndex].timestamp = Time_getEpochTime();
  logBuffer[logIndex].uptime = uptimeSeconds;
  logBuffer[logIndex].statusCode = statusCode;
}
void MeterValue_init_Buffer()
{
  Meter_Value_Buffer_Size = atoi(Meter_Value_Buffer_Size_Char);
  // free memory if already allocated
  if (MeterValues)
  {
    delete[] MeterValues;
    MeterValues = nullptr; // initialize as nullpointer to prevent lost pointer
  }

  // allocate memory
  MeterValues = new /*(std::nothrow)*/ MeterValue[Meter_Value_Buffer_Size];
  if (!MeterValues)
  {
    Serial.println("Speicherzuweisung fehlgeschlagen!");
    Log_AddEntry(1002);
  }

  MeterValues_clear_Buffer();
}

bool b_send_log_to_backend = false;

bool firstTime = true;
String Log_StatusCodeToString(int statusCode)
{
  switch (statusCode)
  {
  case 1001:
    return "setup()";
  case 1002:
    return "Memory Allocation failed";
  case 1003:
    return "Config saved";
  case 1005:
    return "call_backend()";
  case 1006:
    return "Taf 6 meter reading trigger";
  case 1008:
    return "WiFi returned";
  case 1009:
    return "WiFi lost";
  case 1010:
    return "Taf 7 meter reading trigger";
  case 1011:
    return "Taf 14 meter reading trigger";
  case 1012:
    return "call backend trigger";
  case 1013:
    return "MeterValues_clear_Buffer()";
  case 1014:
    return "Taf 7-900s meter reading trigger";
  case 1015:
    return "not enough heap to store value";
  case 1016:
    return "Buffer full, cannot store non-override value";
  case 1017:
    return "Meter Value stored";
  case 1018:
    return "dynamic Taf trigger";
  case 1019:
    return "Sending Log";
  case 1020:
    return "Sending Log successful";
  case 1021:
    return "call_backend successful";
  case 1022:
    return "taf14 trigger not possible, buffer full";
  case 1200:
    return "meter value <= 0";
  case 1201:
    return "current Meter value = previous meter value";
  case 1203:
    return "Suffix Must not be 0";
  case 1204:
    return "prefix suffix not correct";
  case 1205:
    return "Error Buffer Size Exceeded";
  case 3000:
    return "Complete Telegram received";
  case 3001:
    return "Telegram Buffer overflow";
  case 3002:
    return "Telegram timeout";
  case 3003:
    return "Telegram too big for buffer";
  case 4000:
    return "Connection to server failed (Cert!?)";
  case 5000:
    return "myStrom_get_Meter_value Connection failed";
  case 5001:
    return "Failed to connect to myStrom";
  case 5002:
    return "myStrom_get_Meter_value deserializeJson() failed";
  case 7000:
    return "Stopping Wifi, Backendcall unsuccessfull";
  case 7001:
    return "Restarting Wifi";
  case 8000:
    return "Spiffs not mounted";
  case 8001:
    return "Error reading cert file";
  case 8002:
    return "Cert saved";
  case 8003:
    return "Error reading cert file";
  case 8004:
    return "No Cert received";
  }
  if (statusCode < 200)
  {
    return "# values transmitted";
  }

  return "Unknown status code";
}

String Time_getFormattedTime()
{
  time_t now = time(nullptr);
  char timeStr[64];
  struct tm timeinfo;
  localtime_r(&now, &timeinfo); // Lokale Zeit (Zeitzone angewendet)

  strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo); // Zeitformat HH:MM:SS
  return String(timeStr);                                    // Gibt Zeit als lesbaren String aus
}

String Time_formatTimestamp(unsigned long timestamp)
{
  // Konvertiere Unix-Timestamp in lokale Zeit
  time_t rawTime = static_cast<time_t>(timestamp);
  struct tm timeinfo;
  localtime_r(&rawTime, &timeinfo);

  char buffer[20];
  strftime(buffer, sizeof(buffer), "%D %H:%M:%S", &timeinfo);
  return String(buffer);
}
String Log_EntryToString(int i)
{
  if (logBuffer[i].statusCode == 0 && logBuffer[i].timestamp == 0 && logBuffer[i].uptime == 0)
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
  String logString = "<html><head><title>SMGWLite - Meter Values</title>";
  //logString += String(HTML_STYLE);
  logString += "</head><body><table border=1><tr><th>Index</th><th>Timestamp</th><th>Timestamp</th><th>Uptime</th><th>Statuscode</th><th>Status</th></tr>";

  // First Loop: more recent; from logIndex down to 0)
  for (int i = logIndex; i >= 0; i--)
  {
    logString += Log_EntryToString(i);
    showed_number++;
    if (showed_number >= showNumber)
    {
      return logString + "</table>";
    }
  }

  // Second Loop: less recent; from buffer end to logindex
  if (logIndex < LOG_BUFFER_SIZE - 1)
  {
    // logString += "<tr><td>-----</td></tr>";
    for (int i = LOG_BUFFER_SIZE - 1; i > logIndex; i--)
    {
      logString += Log_EntryToString(i);
      showed_number++;
      if (showed_number >= showNumber)
        break;
    }
  }

  return logString + "</table>";
}
void resetMeterValue(MeterValue &val)
{
  val = MeterValue{}; // unset all fields to zero
}
void MeterValues_clear_Buffer()
{
  for (int m = 0; m < Meter_Value_Buffer_Size; m++)
  {
    MeterValues[m].timestamp = 0;
    MeterValues[m].meter_value = 0;
    MeterValues[m].temperature = 0;
    MeterValues[m].solar = 0;
  }
  meter_value_override_i = 0;
  meter_value_NON_override_i = Meter_Value_Buffer_Size - 1;
  meter_value_buffer_overflow = false;
  meter_value_buffer_full = false;
}
int MeterValue_Num()
{
  if (meter_value_buffer_full == true || meter_value_buffer_overflow == true)
  {
    return Meter_Value_Buffer_Size;
  }
  return (meter_value_override_i + ((Meter_Value_Buffer_Size - 1) - meter_value_NON_override_i));
}
int MeterValue_Num2()
{
  int count = 0;
  for (int i = 0; i < Meter_Value_Buffer_Size; i++)
  {
    if (MeterValues[i].timestamp != 0)
    {
      count++;
    }
  }
  return count;
}
void Webserver_LocationHrefHome(int delay)
{
  String call = "<meta http-equiv='refresh' content = '" + String(delay) + ";url=/'>";
  server.send(200, "text/html", call);
}
void Webserver_MeterValue_Num2()
{
  String MeterValueNum = "<html><head><title>SMGWLite - Alternative Amount Meter Value</title> " + String(HTML_STYLE) + "</head><body>"+String(MeterValue_Num2())+"</body></html>";
  server.send(200, "text/html", MeterValueNum);
}
void Webserver_ShowMeterValues()
{
  String MeterValues_string = "<html><head>";
  MeterValues_string += "<title>SMGWLite - Meter Values</title>";
  MeterValues_string += String(HTML_STYLE);
  MeterValues_string += "</head><body><table border='1'><tr><th>Index</th><th>Count</th><th>Timestamp</th><th>Timestamp</th><th>Meter Value</th><th>Termperature </th><th>Solar </th></tr>";
  int count = 1;
  bool first = true;
  for (int m = 0; m < Meter_Value_Buffer_Size; m++)
  {
    if (MeterValues[m].timestamp == 0 && MeterValues[m].meter_value == 0) // don't show empty entries
    {
      if (first)
      {
        first = false;
        MeterValues_string += "<tr><td>-----</td></tr>";
      }
      continue; // skip empty entries 
    }
    MeterValues_string += "<tr><td>" + String(m) + "</td><td>" + String(count++) + "</td><td>" + String(Time_formatTimestamp(MeterValues[m].timestamp)) + "</td><td>" + String(MeterValues[m].timestamp) + "</td><td>" + String(MeterValues[m].meter_value) + "</td><td>" + String(MeterValues[m].temperature) + "</td><td>" + String(MeterValues[m].solar) + "</td></tr>";
  }
  MeterValues_string += "</table>";
  server.send(200, "text/html", MeterValues_string);
}
void Webserver_ShowLogBuffer()
{
  server.send(200, "text/html", Log_BufferToString());
}
void Webclient_splitHostAndPath(const String &url, String &host, String &path)
{
  // Suche nach dem ersten "/"
  int slashIndex = url.indexOf('/');

  if (slashIndex == -1)
  {
    // Kein "/" gefunden -> Alles ist der Host
    host = url;
    path = "/";
  }
  else
  {
    // Host ist der Teil vor dem ersten "/"
    host = url.substring(0, slashIndex);
    // Pfad ist der Teil ab dem ersten "/"
    path = url.substring(slashIndex);
  }
}
String backend_host;
String backend_path;

char *FullCert = new char[2000];
void Webserver_SetCert()
{
  server.send(200, "text/html", "<form action='/upload' method='POST'><textarea name='cert' rows='10' cols='80'>" + String(FullCert) + "</textarea><br><input type='submit'></form>");
}

void Webserver_TestBackendConnection()
{
  WiFiClientSecure client;
  client.setCACert(FullCert);

  String res = "<html><head><title>SMGWLite - Backend Test</title>" + String(HTML_STYLE) + "</head><body>";

  if (client.connect(backend_host.c_str(), 443))
  {
    res += "Host reachable,<br>Cert correct";
  }
  else
  {
    client.setInsecure(); // If Cert not accepted, try without
    if (client.connect(backend_host.c_str(), 443))
    {
      res += "Host reachable<br>Cert not working.";
    }
    else
    {
      res += "Host not reachable.";
      server.send(200, "text/html", res);
      return;
    }
  }

  // reach backend with token and ID
  String url = backend_path + "?backend_test=true&ID=" + String(backend_ID) + "&token=" + String(backend_token);
  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + backend_host + "\r\n" +
               "Connection: close\r\n\r\n");

  unsigned long timeout = millis();
  while (client.available() == 0)
  {
    if (millis() - timeout > 5000) // Timeout after 5 Seconds
    {
      res += "<br>No response from server.";
      server.send(200, "text/html", res);
      return;
    }
  }

  // read answer from server
  String response = "";
  while (client.available())
  {
    response += client.readString();
  }

  // check response code
  if (response.indexOf("200") != -1) //
  {
    res += "<br>ID & Token valid.";
  }
  else
  {
    res += "<br>ID & Token invalid!";
  }
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
  if (!file)
  {
    Log_AddEntry(8001);
    return;
  }

  size_t size = file.size();

  // load file content to char array
  file.readBytes(FullCert, size);

  // Add terminating mark
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
  // -- Set up required URL handlers on the web server.
  server.on("/", Webserver_HandleRoot);
  server.on("/showTelegram", Webserver_ShowTelegram);
  server.on("/showTelegramRaw", Webserver_ShowTelegram_Raw);
  server.on("/showLastMeterValue", Webserver_ShowLastMeterValue);
  server.on("/showCert", Webserver_ShowCert);
  server.on("/setCert", Webserver_SetCert);
  server.on("/testBackendConnection", Webserver_TestBackendConnection);
  server.on("/showMeterValues", Webserver_ShowMeterValues);
  server.on("/showLogBuffer", Webserver_ShowLogBuffer);
  server.on("/MeterValue_Num2", Webserver_MeterValue_Num2);

  server.on("/upload", []
            {
                Webserver_HandleCertUpload();
                Webclient_loadCertToChar(); });

  server.on("/config", []
            { iotWebConf.handleConfig(); });
  server.on("/restart", []
            { 
                Webserver_LocationHrefHome(5);
                delay(100);
                ESP.restart(); });
  server.on("/resetLogBuffer", []
            { 
                Webserver_LocationHrefHome();
                LogBuffer_reset(); });
  server.on("/StoreMeterValue", []
            { Webserver_LocationHrefHome();
              Log_AddEntry(1006);
              MeterValue_trigger_override = true;});
  server.on("/MeterValue_init_Buffer", []
            { MeterValue_init_Buffer();
              Webserver_LocationHrefHome(); });
  server.on("/sendboth_Task", []
            { 
              Webserver_LocationHrefHome(2);
              Webclient_Send_Meter_Values_to_backend_wrapper();
              Webclient_Send_Log_to_backend_wrapper(); });
  server.on("/sendStatus_Task", []
            { 
              Webserver_LocationHrefHome(2);
              Webclient_Send_Log_to_backend_wrapper(); });

  server.on("/sendMeterValues_Task", []
            { 
              Webserver_LocationHrefHome(2);
              Webclient_Send_Meter_Values_to_backend_wrapper(); });

  server.on("/setOffline", []
            { wifi_connected = false;
              Webserver_LocationHrefHome(); });

  server.onNotFound([]()
                    { iotWebConf.handleNotFound(); });

  // OTA Update Handler
  server.on("/update", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", 
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='update'>"
      "<input type='submit' value='Update'>"
      "</form>");
  });

  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "Update Failed" : "Update Successful. Rebooting...");
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Update Start: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });
}

void setup()
{
  Sema_Backend = xSemaphoreCreateMutex();
  Log_AddEntry(1001);
  Serial.begin(115200);

  mySerial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);

  Serial.println();
  Serial.println("Starting up...Hello!");

  Param_setup();
  Led_update_Blink();
  Webserver_UrlConfig();
  OTA_setup();

  if (!SPIFFS.begin(true))
  {
    Log_AddEntry(8000);
  }
  Webclient_loadCertToChar();
  Webclient_splitHostAndPath(String(backend_endpoint), backend_host, backend_path);

  MeterValue_init_Buffer();

  configTime(0, 0, "ptbnts1.ptb.de", "ptbtime1.ptb.de", "ptbtime2.ptb.de");
  Temp_sensors.begin();
  Serial.print("Temp sensors found: ");
  Serial.println(Temp_sensors.getDeviceCount());
}
void Param_setup()
{
  groupTelegram.addItem(&telegram_offset_object);
  groupTelegram.addItem(&telegram_length_object);
  groupTelegram.addItem(&telegram_prefix_object);
  groupTelegram.addItem(&telegram_suffix_object);
  groupBackend.addItem(&backend_endpoint_object);
  groupBackend.addItem(&backend_ID_object);
  groupBackend.addItem(&backend_token_object);
  groupTaf.addItem(&taf7_b_object);
  groupTaf.addItem(&taf7_param_object);
  groupTaf.addItem(&taf14_b_object);
  groupTaf.addItem(&taf14_param_object);
  // groupTaf.addItem(&tafdyn_b_object);
  // groupTaf.addItem(&tafdyn_absolute_object);
  // groupTaf.addItem(&tafdyn_multiplicator_object);
  groupBackend.addItem(&backend_call_minute_object);
  groupTelegram.addItem(&Meter_Value_Buffer_Size_object);

  groupSys.addItem(&led_blink_object);
  groupDebug.addItem(&DebugSetOffline_object);
  groupDebug.addItem(&DebugFromOtherClient_object);
  groupDebug.addItem(&DebugMeterValueFromOtherClientIP_object);
  groupAdditionalMeter.addItem(&mystrom_PV_object);
  groupAdditionalMeter.addItem(&mystrom_PV_IP_object);
  groupAdditionalMeter.addItem(&temperature_object);
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
  // iotWebConf.setFormValidator(&formValidator);
  iotWebConf.getApTimeoutParameter()->visible = true;

  // -- Initializing the configuration.
  iotWebConf.skipApStartup();
  iotWebConf.init();
}
void OTA_setup()
{
  ArduinoOTA
      .onStart([]()
               {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
      else // U_SPIFFS
        type = "filesystem";

      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      Serial.println("Start updating " + type); })
      .onEnd([]()
             { Serial.println("\nEnd"); })
      .onProgress([](unsigned int progress, unsigned int total)
                  { Serial.printf("Progress: %u%%\r", (progress / (total / 100))); })
      .onError([](ota_error_t error)
               {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed"); });
}
void Webclient_Send_Meter_Values_to_backend_wrapper()
{
  xTaskCreate(Webclient_Send_Meter_Values_to_backend_Task, "Send_Meter task", 8192, NULL, 2, NULL);
}

void Webclient_Send_Log_to_backend_wrapper()
{
  xTaskCreate(Webclient_Send_Log_to_backend_Task, "send log task", 8192, NULL, 2, NULL);
}

bool Telegram_prefix_suffix_correct()
{
  int prefix = atoi(telegram_prefix);
  int suffix = atoi(telegram_suffix);

  if (suffix == 0)
  {
    Log_AddEntry(1203);
    Serial.println("Suffix Must not be 0");
    return false;
  }

  if (TELEGRAM[suffix] == 0x1B && TELEGRAM[suffix + 1] == 0x1B && TELEGRAM[suffix + 2] == 0x1B && TELEGRAM[suffix + 3] == 0x1B && TELEGRAM[prefix] == 0x1B && TELEGRAM[prefix + 1] == 0x1B && TELEGRAM[prefix + 2] == 0x1B && TELEGRAM[prefix + 3] == 0x1B)
    return true;
  else
  {
    Log_AddEntry(1204);
    return false;
  }
}

int32_t MeterValue_get_from_telegram()
{
  int offset = atoi(telegram_offset);
  int length = atoi(telegram_length);
  int32_t meter_value = -1;

  if (!Telegram_prefix_suffix_correct())
  {
    return -2;
  }

  for (int i = 0; i < length; i++)
  {
    int shift = length - 1 - i;
    meter_value += TELEGRAM[offset + i] << 8 * shift;
  }
  return meter_value;
}

void myStrom_get_Meter_value()
{

  if (!mystrom_PV_object.isChecked())
  {
    LastMeterValue.solar = 0;
    return;
  }

  Serial.println(F("myStrom_get_Meter_value Connecting..."));

  // Connect to HTTP server
  WiFiClient client;
  client.setTimeout(1000);
  if (!client.connect(mystrom_PV_IP, 80))
  {
    Log_AddEntry(5000);
    Serial.println(F("myStrom_get_Meter_value Connection failed"));
    LastMeterValue.solar = -1;
    return;
  }

  Serial.println(F("myStrom_get_Meter_value Connected!"));

  // Send HTTP request
  client.println(F("GET /report HTTP/1.0"));
  client.print(F("Host: "));
  client.println(mystrom_PV_IP);
  client.println(F("Connection: close"));
  if (client.println() == 0)
  {
    Log_AddEntry(5001);
    Serial.println(F("Failed to connect to mystrom"));
    client.stop();
    LastMeterValue.solar = -2;
    return;
  }

  // Check HTTP status
  char status[32] = {0};
  client.readBytesUntil('\r', status, sizeof(status));
  // It should be "HTTP/1.0 200 OK" or "HTTP/1.1 200 OK"
  if (strcmp(status + 9, "200 OK") != 0)
  {
    Serial.print(F("myStrom_get_Meter_value Unexpected response: "));
    Serial.println(status);
    client.stop();
    LastMeterValue.solar = -3;
    return;
  }

  // Skip HTTP headers
  char endOfHeaders[] = "\r\n\r\n";
  if (!client.find(endOfHeaders))
  {
    Serial.println(F("myStrom_get_Meter_value Invalid response"));
    client.stop();
    LastMeterValue.solar = -4;
    return;
  }

  // Allocate the JSON document
  JsonDocument doc;

  // Parse JSON object
  DeserializationError error = deserializeJson(doc, client);
  if (error)
  {
    Log_AddEntry(5002);
    Serial.print(F("myStrom_get_Meter_value deserializeJson() failed: "));
    Serial.println(error.f_str());
    client.stop();
    LastMeterValue.solar = -5;
    return;
  }

  // Extract values
  LastMeterValue.temperature = doc["temperature"].as<float>() * 100;
  LastMeterValue.solar = doc["energy_since_boot"].as<int>();

  // Disconnect
  client.stop();
}

int32_t MeterValue_get_from_remote()
{
  Serial.println("MeterValue_get_from_remote Connecting...");

  WiFiClient client;
  client.setTimeout(2000);

  if (!client.connect(DebugMeterValueFromOtherClientIP, 80))
  {
    Serial.println(F("Connection failed"));
    return -1;
  }

  Serial.println(F("Connected!"));

  client.println(F("GET /showLastMeterValue HTTP/1.0"));
  client.print(F("Host: "));
  client.println(F(DebugMeterValueFromOtherClientIP));
  client.println(F("Connection: close"));
  client.println(); // empty line at end of headers

  Serial.println(F("Request sent"));

  // Gesamte Antwort lesen
  String fullResponse = "";
  unsigned long startTime = millis();

  while (client.connected() || client.available())
  {
    if (client.available())
    {
      char c = client.read();
      fullResponse += c;
    }
    else
    {
      if (millis() - startTime > 5000)
      {
        Serial.println(F("Timeout while reading response"));
        break;
      }
      delay(10);
    }
  }

  Serial.println(F("Full Response:"));
  Serial.println(fullResponse);

  // Header vom Body trennen
  int bodyIndex = fullResponse.indexOf("\r\n\r\n");
  if (bodyIndex == -1)
  {
    Serial.println(F("No HTTP body found"));
    return -2;
  }

  String body = fullResponse.substring(bodyIndex + 4); // +"4"because of  "\r\n\r\n"
  body.trim();                                         // remove leading/trailing whitespace

  Serial.println(F("Extracted Body:"));
  Serial.println(body);

  // JSON parsen
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, body);

  if (error)
  {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
    return -3;
  }

  // Werte extrahieren
  int32_t meter_value_i32 = doc["meter_value"] | -4;
  int32_t timestamp = doc["timestamp"] | 0;

  Serial.print(F("Meter Value: "));
  Serial.println(meter_value_i32);
  Serial.print(F("Timestamp: "));
  Serial.println(timestamp);
  // if (timestamp >= PrevMeterValue.timestamp + 10 && meter_value_i32 != PrevMeterValue.meter_value)
  // {
  //   PrevMeterValue = LastMeterValue;
  // }
  resetMeterValue(LastMeterValue);                 // reset LastMeterValue
  LastMeterValue.meter_value = doc["meter_value"]; // save meter value
  LastMeterValue.timestamp = doc["timestamp"];     // save timestamp
  LastMeterValue.temperature = doc["temperature"]; // save temperature
  LastMeterValue.solar = doc["solar"];             // no solar value from remote
  client.stop();
  timestamp_telegram = timestamp;
  return meter_value_i32;
}

void Telegram_saveCompleteTelegram()
{
  size_t telegramLength = telegram_receive_bufferIndex + 3; // length + additional bytes
  if (telegramLength > TELEGRAM_LENGTH)
  {
    Log_AddEntry(3003);
    return;
  }

  // copy telegram
  memcpy(TELEGRAM, telegram_receive_buffer, telegram_receive_bufferIndex); // copy main data
  memcpy(TELEGRAM + telegram_receive_bufferIndex, extraBytes, 3);          // copy additional bytes
  TelegramSizeUsed = telegramLength;
  timestamp_telegram = Time_getEpochTime();
  int32_t meter_value = MeterValue_get_from_telegram();
  // if (timestamp_telegram >= PrevMeterValue.timestamp + 10 && meter_value != PrevMeterValue.meter_value)
  // {
  //   PrevMeterValue = LastMeterValue;
  // }
  resetMeterValue(LastMeterValue);               // reset LastMeterValue
  LastMeterValue.meter_value = meter_value;      // get meter value from telegram
  LastMeterValue.timestamp = timestamp_telegram; // save timestamp
  if (temperature_object.isChecked())
  {
    LastMeterValue.temperature = current_temperature;
  }
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
    lastByteTime = millis(); // update time of last byte
    // check if additional bytes must be read
    if (readingExtraBytes)
    {
      extraBytes[extraIndex++] = incomingByte;

      if (extraIndex == 3)
      {
        Telegram_saveCompleteTelegram();
        Telegram_ResetReceiveBuffer();
      }
      continue;
    }

    // Save Byte in Buffer
    if (telegram_receive_bufferIndex < TELEGRAM_LENGTH)
    {
      telegram_receive_buffer[telegram_receive_bufferIndex++] = incomingByte;
    }
    else
    {
      // Serial.println("Error: Buffer Overflow!");
      Log_AddEntry(3001);
      Telegram_ResetReceiveBuffer();
      continue;
    }

    // Check for start signature
    if (telegram_receive_bufferIndex >= sizeof(SML_SIGNATURE_START) &&
        memcmp(telegram_receive_buffer, SML_SIGNATURE_START, sizeof(SML_SIGNATURE_START)) == 0)
    {

      // Check for end signature
      if (telegram_receive_bufferIndex >= sizeof(SML_SIGNATURE_START) + sizeof(SML_SIGNATURE_END))
      {
        if (memcmp(&telegram_receive_buffer[telegram_receive_bufferIndex - sizeof(SML_SIGNATURE_END)], SML_SIGNATURE_END, sizeof(SML_SIGNATURE_END)) == 0)
        {
          // signatur check positive, wait for additional bytes
          readingExtraBytes = true;
        }
      }
    }
  }

  // check for timeout
  if (telegram_receive_bufferIndex > 0 && (millis() - lastByteTime > TELEGRAM_TIMEOUT_MS))
  {
    // Serial.println("Error: Timeout!");
    // Log_AddEntry(3002);
    Telegram_ResetReceiveBuffer();
  }
}

void Webclient_send_log_to_backend()
{
  Serial.println("send_status_report");
  Log_AddEntry(1019);
  WiFiClientSecure client;

  if (UseSslCert_object.isChecked())
  {
    client.setCACert(FullCert);
  }
  else
  {
    client.setInsecure();
  }

  if (!client.connect(backend_host.c_str(), 443))
  {
    Serial.println("Connection to server failed");
    Log_AddEntry(4000);
    return;
  }

  // write binary data from LogBuffer into temporary buffer
  size_t logBufferSize = LOG_BUFFER_SIZE * sizeof(LogEntry);
  uint8_t *logDataBuffer = (uint8_t *)malloc(logBufferSize);
  if (!logDataBuffer)
  {
    Serial.println("Log buffer allocation failed");
    return;
  }
  memcpy(logDataBuffer, logBuffer, logBufferSize);

  String logHeader = "POST ";
  logHeader += backend_path;
  logHeader += "log.php HTTP/1.1\r\n";
  logHeader += "Host: ";
  logHeader += backend_host;
  logHeader += "\r\n";
  logHeader += "Content-Type: application/octet-stream\r\n";
  logHeader += "Content-Length: " + String(logBufferSize) + "\r\n";
  logHeader += "Connection: close\r\n\r\n";
  Serial.println(logHeader);

  client.print(logHeader);
  client.write(logDataBuffer, logBufferSize);

  free(logDataBuffer);

  while (client.connected() || client.available())
  {
    if (client.available())
    {
      String line = client.readStringUntil('\n');
      Serial.println(line);

      if (line.startsWith("HTTP/1.1 200"))
      {
        Serial.println("Log successfully sent");

        Log_AddEntry(1020);
        b_send_log_to_backend = false;
        break;
      }
      else
      {
        b_send_log_to_backend = true;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10)); // Wait for 10ms, no matter how CONFIG_FREERTOS_HZ is set
  }
  client.stop();
}

void Webclient_send_meter_values_to_backend()
{
  Log_AddEntry(1005);
  Log_AddEntry(MeterValue_Num());
  Serial.println("call_backend_V2");
  last_call_backend = millis();
  if (MeterValue_Num() == 0)
  {

    Serial.println("Zero Values to transmit");
    Log_AddEntry(0);
    call_backend_successfull = true;
    return;
  }

  call_backend_successfull = false;

  WiFiClientSecure client;

  if (UseSslCert_object.isChecked())
  {
    client.setCACert(FullCert);
  }
  else
  {
    client.setInsecure();
  }

  if (!client.connect(backend_host.c_str(), 443))
  {
    Serial.println("Connection to server failed");
    Log_AddEntry(4000);
    return;
  }

  size_t bufferSize = Meter_Value_Buffer_Size * sizeof(MeterValue);

  uint8_t *buffer = (uint8_t *)malloc(bufferSize);
  if (!buffer)
  {
    Serial.println("Buffer allocation failed");
    return;
  }
  memcpy(buffer, MeterValues, bufferSize);

  String header = "POST ";
  header += backend_path;
  header += "?ID=";
  header += backend_ID;
  header += "&token=";
  header += String(backend_token);
  header += "&uptime=";
  header += String(millis() / 60000);
  header += "&time=";
  header += String(Time_getFormattedTime());
  header += "&PV_included=true";
  header += "&heap=";
  header += String(ESP.getFreeHeap());
  header += "&transmittedValues=";
  header += String(MeterValue_Num());

  header += " HTTP/1.1\r\n";

  header += "Host: ";
  header += backend_host;
  header += "\r\n";
  header += "Content-Type: application/octet-stream\r\n";
  header += "Content-Length: " + String(bufferSize) + "\r\n";
  header += "Connection: close\r\n\r\n";

  client.print(header);
  client.write(buffer, bufferSize);

  free(buffer);

  while (client.connected() || client.available())
  {
    if (client.available())
    {
      String line = client.readStringUntil('\n');
      Serial.println(line);

      if (line.startsWith("HTTP/1.1 200"))
      {
        Serial.println("MeterValues successfully sent");
        call_backend_successfull = true;
        MeterValues_clear_Buffer();
        last_call_backend = millis();
        Log_AddEntry(1021);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10)); // Wait for 10ms, no matter how CONFIG_FREERTOS_HZ is set
  }

  client.stop();
}

bool MeterValue_store(bool override)
{
  if (ESP.getFreeHeap() < 1000)
  {
    Log_AddEntry(1015);
    Serial.println("Not enough free heap to store another value");
    return false;
  }

  if (mystrom_PV_object.isChecked())
  {
    myStrom_get_Meter_value();
  }

  if (LastMeterValue.meter_value <= 0)
  {
    Log_AddEntry(1200);
    return false;
  }

  if (LastMeterValue.meter_value == PrevMeterValue.meter_value
    && LastMeterValue.solar == PrevMeterValue.solar)
  {
    Log_AddEntry(1201);
    return false;
  }

  

  // var where to write
  int write_i = 0;
  if (override)
  {
    write_i = meter_value_override_i;
  }
  else
  {
    write_i = meter_value_NON_override_i;
  }
  Serial.println("where to write: " + String(write_i));

  if (MeterValues[write_i].timestamp == 0 && MeterValues[write_i].meter_value == 0 && MeterValues[write_i].temperature == 0)
  {
    // if place where wo want to write is full we assume entire buffer is used
    meter_value_buffer_full = false;
  }
  else
  {
    meter_value_buffer_full = true;
  }

  // if I shall override, go ahead.
  // If I must not override, I need to be sure that buffer not full yet
  if (override == true || meter_value_buffer_full == false)
  {

    MeterValues[write_i] = LastMeterValue; // copy last meter value to buffer

    // calculate next writing location
    if (override == true)
    {
      // increase counter as we are writing ascending
      meter_value_override_i++;
      if (meter_value_override_i >= Meter_Value_Buffer_Size)
      {
        meter_value_override_i = 0;
        meter_value_buffer_overflow = true;
      }
    }
    else
    {
      // decrease counter as we are writing descending
      meter_value_NON_override_i--;
      if (meter_value_override_i < 0)
      {
        meter_value_override_i = Meter_Value_Buffer_Size - 1;
        meter_value_buffer_overflow = true;
      }
    }
  }
  else
  {
    Log_AddEntry(1016);
    MeterValue_trigger_non_override = false; //deactivate so that is not retriggered
    return false;
    Serial.println("Buffer Full, no space to write new value!");
  }
  
  PrevMeterValue = LastMeterValue; // save last meter value as previous
  return true;
}

void handle_check_wifi_connection()
{
  wl_status_t current_wifi_status = WiFi.status();
  if (DebugSetOffline_object.isChecked())
  {
    current_wifi_status = WL_CONNECTION_LOST;
  }

  if (millis() - last_wifi_check > 500)
  {
    last_wifi_check = millis();

    if (current_wifi_status == WL_CONNECTED && wifi_connected)
    {
      // Still wifi_connected
    }
    else if (current_wifi_status == WL_CONNECTED && !wifi_connected)
    {
      Log_AddEntry(1008);
      Serial.println("Connection has returned: Resetting Backend Timer, starting OTA");
      ArduinoOTA.begin();
      wifi_connected = true;
      wifi_reconnection_time = millis();
      call_backend_successfull = false;

      if (firstTime == true)
      {
        firstTime = false;
      }
      else
      {
        b_send_log_to_backend = true;
      }
    }
    else if (current_wifi_status != WL_CONNECTED && wifi_connected)
    {
      // Wifi lost
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
      current_temperature = (Temp_sensors.getTempCByIndex(0) * 100);
      read_temperature = true;
    }
  }
}
void handle_call_backend()
{
  if (wifi_connected && millis() - wifi_reconnection_time > 60000)
  {
    if (
        (!call_backend_successfull && millis() - last_call_backend > 30000) || ((Time_getMinutes()) % atoi(backend_call_minute) == 0 && Time_getEpochTime() % 60 > 5 // little delay to wait for latest metering value
                                                                                && millis() - last_call_backend > 60000))
    {
      Webclient_Send_Meter_Values_to_backend_wrapper();
      if (b_send_log_to_backend == true)
      {
        Webclient_Send_Log_to_backend_wrapper();
      }
    }
  }
}
// void dynTaf()
// {
//   int dT = LastMeterValue.timestamp - PrevMeterValue.timestamp;;
//   if (dT > 0 && LastMeterValue.meter_value > PrevMeterValue.meter_value)
//   {
//     currentPower = (float)(360 * (LastMeterValue.meter_value - PrevMeterValue.meter_value)) / (dT);
//     if(currentPower > LastPower * int(tafdyn_multiplicator) || currentPower < LastPower / int(tafdyn_multiplicator) || abs(currentPower-LastPower) >= int(tafdyn_absolute))
//     {
//       MeterValue_store(false);
//       Log_AddEntry(1018);
//     }
//     LastPower = currentPower;
//   }
// }
unsigned long last_meter_value_attempt = 0;
void handle_MeterValue_store()
{
  if(!MeterValue_trigger_override && !MeterValue_trigger_non_override)
  {
    return; // nothing to do
  }
  if(millis() - last_meter_value_attempt < 1000)
  {
    return;
  }
  last_meter_value_attempt = millis();

  bool retVal = false;
  if (MeterValue_trigger_override == true)
  {
    retVal = MeterValue_store(true);
    if(retVal == true)
    {
      last_taf7_meter_value = millis();
    }

  }
  else if (MeterValue_trigger_non_override == true)
  {
    
    retVal = MeterValue_store(false);
   if(retVal == true)
    {
      last_taf14_meter_value = millis();
    }
  }

  if(retVal == true)
  {
    Log_AddEntry(1017);
    last_taf14_meter_value = millis();
    last_meter_value = millis();
    MeterValue_trigger_override = false;
    MeterValue_trigger_non_override = false;
  }
  // else
  // {
  //   last_meter_value += 1000;
  // }
}


void handle_MeterValue_trigger()
{
  if (MeterValue_trigger_override == false &&
      taf7_b_object.isChecked() &&
      ((Time_getEpochTime() - 1) % (atoi(taf7_param) * 60) < 15) &&
      (millis() - last_taf7_meter_value > 45000))
  {
    
    Log_AddEntry(1010);
    MeterValue_trigger_override = true;
    
  }
  if (MeterValue_trigger_override == false && 
    !wifi_connected && millis() - last_meter_value > 900000)
  {
    Log_AddEntry(1014);
    MeterValue_trigger_override = true;
    
  }
  if (MeterValue_trigger_override == false &&
      MeterValue_trigger_non_override == false &&
      taf14_b_object.isChecked() &&
      millis() - last_meter_value >= 1000UL * max(1UL, (unsigned long)atoi(taf14_param)) &&
      millis() - last_taf14_meter_value >= 1000UL * max(1UL, (unsigned long)atoi(taf14_param))
    )
  {
    if(meter_value_buffer_full == true)
    {
      last_taf14_meter_value = millis();
      Log_AddEntry(9999);
    }
    else
    {
      Log_AddEntry(1011);
      MeterValue_trigger_non_override = true;
    }
  }
  // if(tafdyn_b_object.isChecked())
  // {
  //   dynTaf();
  // }
}
void loop()
{
  iotWebConf.doLoop();
  ArduinoOTA.handle();
  handle_temperature();
  handle_Telegram_receive();
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
  case ESP_RST_UNKNOWN:
    return "Unknown";
  case ESP_RST_POWERON:
    return "Power on";
  case ESP_RST_EXT:
    return "External reset";
  case ESP_RST_SW:
    return "Software reset";
  case ESP_RST_PANIC:
    return "Exception/panic";
  case ESP_RST_INT_WDT:
    return "Interrupt watchdog";
  case ESP_RST_TASK_WDT:
    return "Task watchdog";
  case ESP_RST_WDT:
    return "Other watchdogs";
  case ESP_RST_DEEPSLEEP:
    return "Deep sleep";
  case ESP_RST_BROWNOUT:
    return "Brownout";
  case ESP_RST_SDIO:
    return "SDIO";
  default:
    return "Unknown";
  }
}
#endif

String Time_formatUptime()
{
  int64_t uptimeMicros = esp_timer_get_time(); // time in microsends
  int64_t uptimeMillis = uptimeMicros / 1000;  // transform to milli seconds
  int64_t uptimeSeconds = uptimeMillis / 1000; // transform to seconds

  // calculating days, hours, minutes and seconds
  int days = uptimeSeconds / 86400;
  uptimeSeconds %= 86400;
  int hours = uptimeSeconds / 3600;
  uptimeSeconds %= 3600;
  int minutes = uptimeSeconds / 60;
  int seconds = uptimeSeconds % 60;

  // formatting
  char buffer[20];
  sprintf(buffer, "%02dd %02dh%02dm%02ds", days, hours, minutes, seconds);
  return String(buffer);
}
void Webserver_HandleRoot()
{
  // -- Let IotWebConf test and handle captive portal requests.
  if (iotWebConf.handleCaptivePortal())
  {
    // -- Captive portal request were already served.
    return;
  }
  String s;
  s.reserve(8000);

  s += R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no" />
  <title>)rawliteral";
  s += thingName;
  s += R"rawliteral(</title>)rawliteral";
  s += HTML_STYLE;
  s += R"rawliteral(<style>
    body { font-family: sans-serif; margin: 1em; }
    table { border-collapse: collapse; width: 100%; max-width: 7<00px; }
    th, td { border: 1px solid #ccc; padding: 6px 12px; text-align: left; }
    ul { list-style-type: square; padding-left: 20px; }
    li { margin-bottom: 0.3em; }
    a { color: #0066cc; text-decoration: none; }
    a:hover { text-decoration: underline; }
    font[color="red"] { color: red; }
  </style>
</head>
<body>

<p>Go to <a href='config'><b>configuration page</b></a> to change <i>italic</i> values.</p>

<h2>Last Meter Value</h2>
<table>
  <tr><th>Time</th><th>Meter Value</th><th>Temperature</th><th>Solar</th></tr>
  <tr>
    <td>)rawliteral";
  s += String(Time_getEpochTime() - LastMeterValue.timestamp) + " s ago";
  s += R"rawliteral(</td>
    <td>)rawliteral";
  s += String(LastMeterValue.meter_value);
  s += R"rawliteral(</td>
    <td>)rawliteral";
  s += String(LastMeterValue.temperature / 100.0) + " °C";
  s += R"rawliteral(</td>
    <td>)rawliteral";
  s += String(LastMeterValue.solar);
  s += R"rawliteral(</td>
  </tr>
</table>

<p><a href='StoreMeterValue'>Store Meter Value Now (Taf6)</a></p>

<h3>Meter Value Buffer</h3>
<ul>
  <li>Used / <i>Size</i>: )rawliteral" +
       String(MeterValue_Num()) + " / " + String(Meter_Value_Buffer_Size) + R"rawliteral(</li>
  <li>i override: )rawliteral" +
       String(meter_value_override_i) + R"rawliteral(</li>
  <li>i non override: )rawliteral" +
       String(meter_value_NON_override_i) + R"rawliteral(</li>
  <li>Meter Value Buffer Overflow: )rawliteral" +
       String(meter_value_buffer_overflow) + R"rawliteral(</li>
  <li>Meter Value Buffer Full: )rawliteral" +
       String(meter_value_buffer_full) + R"rawliteral(</li>
  <li><a href='MeterValue_Num2'>Calculate # Meter Values (alternative way)</a></li>
  <li><a href='showMeterValues'>Show Meter Values</a></li>
)rawliteral";

  if (Meter_Value_Buffer_Size != atoi(Meter_Value_Buffer_Size_Char))
  {
    s += "<li><font color='red'>Buffer Size changed, please ";
    if (MeterValue_Num() > 0)
    {
      s += "<a href='sendMeterValues_Task'>Send Meter Values to Backend</a> to not lose (" +
           String(MeterValue_Num()) + ") Meter Values and ";
    }
    s += "<a href='MeterValue_init_Buffer'>Re-Init Meter Value Buffer</a></font></li>\n";
  }
  else
  {
    s += "<li><a href='MeterValue_init_Buffer'>Re-Init Meter Value Buffer</a></li>\n";
  }

  s += R"rawliteral(
  <li><a href='showLastMeterValue'>Show Last Meter Value (JSON)</a></li>
</ul>

<h3>Telegram Parse Config</h3>
<ul>
  <li><i>Prefix Begin (usually 0):</i> )rawliteral";
  s += String(atoi(telegram_prefix));
  s += R"rawliteral(</li>
  <li><i>Meter Value Offset:</i> )rawliteral";
  s += String(atoi(telegram_offset));
  s += R"rawliteral(</li>
  <li><i>Meter Value Length:</i> )rawliteral";
  s += String(atoi(telegram_length));
  s += R"rawliteral(</li>
  <li><i>Suffix Begin:</i> )rawliteral";
  s += String(atoi(telegram_suffix));
  s += R"rawliteral(</li>
  <li><a href='showTelegram'>Show Telegram</a> (<a href='showTelegramRaw'>Raw</a>)</li>
</ul>

<h3>Backend Config</h3>
<ul>
  <li><i>Backend Endpoint:</i> )rawliteral";
  s += backend_endpoint;
  s += R"rawliteral(</li>
  <li>Backend Host: )rawliteral";
  s += backend_host;
  s += R"rawliteral(</li>
  <li>Backend Path: )rawliteral";
  s += backend_path;
  s += R"rawliteral(</li>
  <li><i>Backend Call Minute:</i> )rawliteral";
  s += String(atoi(backend_call_minute));
  s += R"rawliteral(</li>
  <li><i>Backend ID:</i> )rawliteral";
  s += backend_ID;
  s += R"rawliteral(</li>
  <li><i>Backend Token:</i> )rawliteral";
  s += backend_token;
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
  <li><a href='sendStatus_Task'>Send Status Report to Backend</a></li>
  <li><a href='sendMeterValues_Task'>Send Meter Values to Backend</a></li>
  <li><a href='sendboth_Task'>Send Meter Values and Status Report to Backend</a></li>
</ul>

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
  <li><i>Taf14 Read Meter Interval:</i> )rawliteral";
  s += String(atoi(taf14_param));
  s += R"rawliteral(</li>
</ul>

<h3>Additional Meter</h3>
<ul>
  <li><i>Temperature Sensor:</i> )rawliteral";
  s += (temperature_object.isChecked() ? "activated" : "deactivated");
  s += R"rawliteral(</li>
  <li><i>MyStrom:</i> )rawliteral";
  s += (mystrom_PV_object.isChecked() ? "activated" : "deactivated");
  s += R"rawliteral(</li>
  <li><i>MyStrom IP:</i> )rawliteral";
  s += mystrom_PV_IP;
  s += R"rawliteral(</li>
</ul>

<h3>Debug Helper</h3>
<ul>
  <li><i>Set Device Offline:</i> )rawliteral";
  s += (DebugSetOffline_object.isChecked() ? "activated" : "deactivated");
  s += R"rawliteral(</li>
  <li><i>Get Meter Values from other SMGWLite:</i> )rawliteral";
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
  <li>Water Mark Main Task: )rawliteral";
  s += String(uxTaskGetStackHighWaterMark(NULL));
  s += R"rawliteral(</li>
  <li>Water Mark Meter Values: )rawliteral";
  s += String(watermark_meter_buffer);
  s += R"rawliteral(</li>
  <li>Water Mark Logs: )rawliteral";
  s += String(watermark_log_buffer);
  s += R"rawliteral(</li>
  <li>Uptime (min): )rawliteral";
  s += Time_formatUptime();
  s += R"rawliteral(</li>
)rawliteral";

#if defined(ESP32)
  s += "<li>Reset Reason: " + Log_get_reset_reason() + "</li>\n";
#elif defined(ESP8266)
  s += "<li>Reset Reason: " + String(ESP.getResetReason()) + " / " + String(ESP.getResetInfo()) + "</li>\n";
#endif

  s += R"rawliteral(
  <li>System time (UTC): )rawliteral";
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




<h3>Log Buffer (last 10 entries / current buffer index )rawliteral";
s += String(logIndex);
s += R"rawliteral()</h3>
<ul>
  <li><a href='showLogBuffer'>Show entire Logbuffer</a></li>
  <li><a href='resetLogBuffer'>Reset Log</a></li>
)rawliteral";
  s += Log_BufferToString(10);
  s += R"rawliteral(

</body>
</html>
)rawliteral";

  server.send(200, "text/html", s);
}
void Webserver_ShowCert()
{
  server.send(200, "text/html", String(FullCert));
}

void Webserver_ShowTelegram_Raw()
{
  String  s = "<div class='block'>Receive Buffer</div><textarea name='cert' rows='10' cols='80'>";
  for(int i = 0; i < TELEGRAM_LENGTH; i++)
  {
    if (i > 0)
      s += " ";
    s += String(telegram_receive_buffer[i]);
  }
  s += "</textarea><br><br><div class='block'>Receive Buffer Hex</div><textarea name='cert' rows='10' cols='80'>";
  for(int i = 0; i < TELEGRAM_LENGTH; i++)
  {
    if (i > 0)
      s += " ";
    s += String(telegram_receive_buffer[i], HEX);
  }
  s += "</textarea><br><br><div class='block'>Validated Telegram</div><textarea name='cert' rows='10' cols='80'>";
  for(int i = 0; i < TELEGRAM_LENGTH; i++)
  {
    if (i > 0)
      s += " ";
    s += String(telegram_receive_buffer[i]);
  }
 


  s += "<br><br></textarea><br><br><div class='block'>Validated Telegram Hex</div><textarea name='cert' rows='10' cols='80'>";
  for(int i = 0; i < TELEGRAM_LENGTH; i++)
  {
    if (i > 0)
      s += " ";
    s += String(telegram_receive_buffer[i], HEX);
  }
  s += "</textarea>";
  server.send(200, "text/html", s);

}

void Webserver_ShowTelegram()
{
  // -- Let IotWebConf test and handle captive portal requests.
  if (iotWebConf.handleCaptivePortal())
  {
    // -- Captive portal request were already served.
    return;
  }
  String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
  s += "<title>SMGWLite - Show Telegram</title>";
  s += HTML_STYLE;

  s += "<br>Last Byte received @ " + String(millis()-lastByteTime) + "ms ago<br>";
  s += "<br>Last Validated Telegram @ " + String(timestamp_telegram) + " = " + Time_formatTimestamp(timestamp_telegram) + ": " + String(Time_getEpochTime() - timestamp_telegram) + "s old<br>";
    
  if (!Telegram_prefix_suffix_correct())
    s += "<br><font color=red>incomplete telegram</font>";
  s += "<table border=1><tr><th>Index</th><th>Receive Buffer</th><th>Validated Buffer</th></tr>";


  String color;

  int signature_7101 = 9999;
  int k = 0;
  for (int i = 0; i < TELEGRAM_LENGTH; i++)
  {
    if (i < TELEGRAM_LENGTH - 5 && telegram_receive_buffer[i - k] == 7 && telegram_receive_buffer[i + 1 - k] == 1 && telegram_receive_buffer[i + 2 - k] == 0 && telegram_receive_buffer[i + 3 - k] == 1 && telegram_receive_buffer[i + 4 - k] == 8)
    {
      color = "bgcolor=959018";
      signature_7101 = i;
      if(k<5) k++;
    }
    else if (i > signature_7101 && telegram_receive_buffer[i] == 0x77)
    {
      signature_7101 = 9999;
      color = "bgcolor=959018";
    }
    else if (i >= atoi(telegram_offset) && i < atoi(telegram_offset) + atoi(telegram_length))
    {

      color = "bgcolor=cccccc";
    }
    else
      color = "";
    s += "<tr><td>" + String(i) + "</td><td " + String(color) + ">"+String(telegram_receive_buffer[i], HEX)+"</td><td>" + String(TELEGRAM[i], HEX) + "</td></tr>";
  }
  s += "</table";

  s += "</body></html>\n";

  server.send(200, "text/html", s);
}

void Webserver_ShowLastMeterValue()
{

  JsonDocument jsonDoc; // Heap-basiert

  jsonDoc["meter_value"] = LastMeterValue.meter_value;
  jsonDoc["timestamp"] = LastMeterValue.timestamp;
  jsonDoc["temperature"] = LastMeterValue.temperature;
  jsonDoc["solar"] = LastMeterValue.solar;

  String jsonResponse;
  serializeJson(jsonDoc, jsonResponse);

  server.send(200, "application/json", jsonResponse);
}

void Param_configSaved()
{
  Serial.println("Configuration was updated.");

  Led_update_Blink();
  Webclient_splitHostAndPath(String(backend_endpoint), backend_host, backend_path);
  Log_AddEntry(1003);
}

void Led_update_Blink()
{
  if (led_blink_object.isChecked())
    iotWebConf.enableBlink();
  else
  {
    iotWebConf.disableBlink();
    digitalWrite(LED_BUILTIN, LOW);
  }
}