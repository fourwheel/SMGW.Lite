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
const char thingName[] = "SMGW.Lite";

// -- Initial password to connect to the Thing, when it creates an own Access Point.
const char wifiInitialApPassword[] = "password";

#define STRING_LEN 128
#define ID_LEN 4
#define NUMBER_LEN 5

// -- Configuration specific key. The value should be modified if config structure was changed.
#define CONFIG_VERSION "1015"

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
#define TELEGRAM_TIMEOUT_MS 30 // timeout for telegramm in ms
size_t TelegramSizeUsed = 0;  // actual size of stored telegram
uint8_t telegram_receive_buffer[TELEGRAM_LENGTH]; // buffer for serial data
size_t telegram_receive_bufferIndex = 0;          // positoin in serial data butter
bool readingExtraBytes = false;                   // reading additional bytes?
uint8_t extraBytes[3];                            // additional bytes after end signature
size_t extraIndex = 0;                            // index of additional bytes
unsigned long lastByteTime = 0;                   // timestamp of last received byte
unsigned long timestamp_telegram;                 // timestamp of telegram
uint8_t TELEGRAM[TELEGRAM_LENGTH]; // buffer for entire telegram


// Meter Value Vsrs 
int meter_value_i = 0;
struct MeterValue
{
  uint32_t timestamp;   // 4 Bytes
  uint32_t meter_value; // 4 Bytes
  uint32_t temperature; // 4 Bytes
};
MeterValue *MeterValues = nullptr; // initiaize with nullptr
unsigned long last_meter_value = 0;
unsigned long previous_meter_value = 0;
int meter_value_buffer_overflow = 0;
int Meter_Value_Buffer_Size = 234;

// Backend Vars
bool call_backend_successfull = true;
SemaphoreHandle_t Sema_Backend; // Mutex / Sempahore for backend call
unsigned long last_call_backend = 0;


// Temperature Vars
#define ONE_WIRE_BUS 4
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature Temp_sensors(&oneWire);
unsigned long last_temperature = 0;
bool read_temperature = false;
int temperature;

// Log Vars
const int LOG_BUFFER_SIZE = 100;

// Tasks vars
int watermark_meter_buffer = 0;
int watermark_log_buffer = 0;

// Wifi Vars
unsigned long wifi_reconnection_time = 0;
unsigned long last_wifi_retry = 0;
bool restart_wifi = false;
unsigned long last_wifi_check;
bool wifi_connected;


// -- Forward declarations.
void handle_call_backend();
void handle_check_wifi_connection();
void handle_MeterValue_store();
void handle_temperature();
void Led_update_Blink();
String Log_BufferToString(int showNumber = LOG_BUFFER_SIZE);
String Log_EntryToString(int i);
String Log_StatusCodeToString(int statusCode);
#if defined(ESP32)
String Log_get_reset_reason();
#endif
void MeterValue_store();
void MeterValues_clear_Buffer();
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
void Telegram_handle();
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
void Webserver_ShowTermperature();
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
char mystrom_PV[STRING_LEN];
char mystrom_PV_IP[STRING_LEN];
char temperature_checkbock[STRING_LEN];
char backend_token[STRING_LEN];
char read_meter_intervall[NUMBER_LEN];
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
IotWebConfParameterGroup groupAdditionalMeter = IotWebConfParameterGroup("groupAdditionalMeter", "Additional Meters & Sensors");
IotWebConfParameterGroup groupSys = IotWebConfParameterGroup("groupSys", "Advanced Sys Config");

IotWebConfNumberParameter telegram_offset_object = IotWebConfNumberParameter("Offset", "telegram_offset_object", telegram_offset, NUMBER_LEN, "20", "1..TELEGRAM_LENGTH", "min='1' max='TELEGRAM_LENGTH' step='1'");
IotWebConfNumberParameter telegram_length_object = IotWebConfNumberParameter("Length", "telegram_length_object", telegram_length, NUMBER_LEN, "8", "1..TELEGRAM_LENGTH", "min='1' max='TELEGRAM_LENGTH' step='1'");
IotWebConfNumberParameter telegram_prefix_object = IotWebConfNumberParameter("Prefix Begin", "telegram_prefix", telegram_prefix, NUMBER_LEN, "0", "1..TELEGRAM_LENGTH", "min='0' max='TELEGRAM_LENGTH' step='1'");
IotWebConfNumberParameter telegram_suffix_object = IotWebConfNumberParameter("Suffix Begin", "telegram_suffix", telegram_suffix, NUMBER_LEN, "100", "1..TELEGRAM_LENGTH", "min='100' max='TELEGRAM_LENGTH' step='1'");

IotWebConfTextParameter backend_endpoint_object = IotWebConfTextParameter("backend endpoint", "backend_endpoint", backend_endpoint, STRING_LEN);
IotWebConfCheckboxParameter led_blink_object = IotWebConfCheckboxParameter("LED Blink", "led_blink", led_blink, STRING_LEN, true);
IotWebConfTextParameter backend_ID_object = IotWebConfTextParameter("backend ID", "backend_ID", backend_ID, ID_LEN);
IotWebConfTextParameter backend_token_object = IotWebConfTextParameter("backend token", "backend_token", backend_token, STRING_LEN);
IotWebConfNumberParameter read_meter_intervall_object = IotWebConfNumberParameter("Taf 14 Meter Intervall (s)", "read_meter_intervall", read_meter_intervall, NUMBER_LEN, "20", "5..100 s", "min='5' max='100' step='1'");
IotWebConfNumberParameter backend_call_minute_object = IotWebConfNumberParameter("backend Call Minute", "backend_call_minute", backend_call_minute, NUMBER_LEN, "5", "", "");

IotWebConfCheckboxParameter mystrom_PV_object = IotWebConfCheckboxParameter("MyStrom PV", "mystrom_PV", mystrom_PV, STRING_LEN, false);
IotWebConfTextParameter mystrom_PV_IP_object = IotWebConfTextParameter("MyStrom PV IP", "mystrom_PV_IP", mystrom_PV_IP, STRING_LEN);
IotWebConfCheckboxParameter temperature_object = IotWebConfCheckboxParameter("Temperatur Sensor", "temperature_checkbock", temperature_checkbock, STRING_LEN, true);
IotWebConfCheckboxParameter UseSslCert_object = IotWebConfCheckboxParameter("Wirk-PKI (Use SSL Cert)", "UseSslCertValue", UseSslCertValue, STRING_LEN, false);

IotWebConfNumberParameter Meter_Value_Buffer_Size_object = IotWebConfNumberParameter("Meter_Value_Buffer_Size", "Meter_Value_Buffer_Size", Meter_Value_Buffer_Size_Char, NUMBER_LEN, "200", "1...1000", "min='1' max='1000' step='1'");

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
  MeterValues = new (std::nothrow) MeterValue[Meter_Value_Buffer_Size];
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
  case 1019:
    return "Sending Log";
  case 1020:
    return "Sending Log successful";
  case 1021:
    return "call_backend successful";
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
  String logString = "<table border=1><tr><th>Index</th><th>Timestamp</th><th>Timestamp</th><th>Uptime</th><th>Statuscode</th><th>Status</th></tr>";

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

void MeterValues_clear_Buffer()
{
  for (int m = 0; m < Meter_Value_Buffer_Size; m++)
  {
    MeterValues[m].timestamp = 0;
    MeterValues[m].meter_value = 0;
    MeterValues[m].temperature = 0;
  }
  meter_value_i = 0;
  meter_value_buffer_overflow = 0;
}
void Webserver_LocationHrefHome(int delay)
{
  String call = "<meta http-equiv='refresh' content = '" + String(delay) + ";url=/'>";
  server.send(200, "text/html", call);
}
void Webserver_ShowMeterValues()
{
  String MeterValues_string = "<table border='1'><tr><th>Index</th><th>Timestamp</th><th>Meter Value</th><th>Termperature </th></tr>";
  for (int m = 0; m < Meter_Value_Buffer_Size; m++)
  {
    MeterValues_string += "<tr><td>" + String(m) + "</td><td>" + String(MeterValues[m].timestamp) + "</td><td>" + String(MeterValues[m].meter_value) + "</td><td>" + String(MeterValues[m].temperature) + "</td></tr>";
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

  String res;

  if (client.connect(backend_host.c_str(), 443))
  {
    res = "Host reachable,<br>Cert correct";
  }
  else
  {
    client.setInsecure(); // If Cert not accepted, try without
    if (client.connect(backend_host.c_str(), 443))
    {
      res = "Host reachable<br>Cert not working.";
    }
    else
    {
      res = "Host not reachable.";
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
  vTaskDelete(NULL);  // delete task when finished
}

void Webserver_UrlConfig()
{
  // -- Set up required URL handlers on the web server.
  server.on("/", Webserver_HandleRoot);
  server.on("/showTelegram", Webserver_ShowTelegram);
  server.on("/showLastMeterValue", Webserver_ShowLastMeterValue);
  server.on("/showTemperature", Webserver_ShowTermperature);
  server.on("/showCert", Webserver_ShowCert);
  server.on("/setCert", Webserver_SetCert);
  server.on("/testBackendConnection", Webserver_TestBackendConnection);
  server.on("/showMeterValues", Webserver_ShowMeterValues);
  server.on("/showLogBuffer", Webserver_ShowLogBuffer);

  server.on("/upload", []
            {
                Webserver_HandleCertUpload();
                Webclient_loadCertToChar(); 
            });

  server.on("/config", []
            { iotWebConf.handleConfig(); });
  server.on("/restart", []
            { 
                Webserver_LocationHrefHome(5);
                delay(100);
                ESP.restart(); 
            });
  server.on("/resetLogBuffer", []
            { 
                Webserver_LocationHrefHome();
                LogBuffer_reset(); 
            });
  server.on("/StoreMeterValue", []
            { Webserver_LocationHrefHome();
              Log_AddEntry(1006);
              MeterValue_store();
            });
  server.on("/MeterValue_init_Buffer", []
            { MeterValue_init_Buffer();
              Webserver_LocationHrefHome(); 
            });
  server.on("/sendboth_Task", []
            { 
              Webserver_LocationHrefHome(2);
              Webclient_Send_Meter_Values_to_backend_wrapper();
              Webclient_Send_Log_to_backend_wrapper(); 
            });
  server.on("/sendStatus_Task", []
            { 
              Webserver_LocationHrefHome(2);
              Webclient_Send_Log_to_backend_wrapper(); 
            });

  server.on("/sendMeterValues_Task", []
            { 
              Webserver_LocationHrefHome(2);
              Webclient_Send_Meter_Values_to_backend_wrapper(); 
            });

  server.on("/setOffline", []
            { wifi_connected = false;
              Webserver_LocationHrefHome(); 
            });

  server.onNotFound([]()
                    { iotWebConf.handleNotFound(); });
}

void setup()
{
  Sema_Backend = xSemaphoreCreateMutex();
  Log_AddEntry(1001);
  Serial.begin(115200);

#if defined(ESP32)
  mySerial.begin(9600, SERIAL_8N1, 15, 16);
#elif defined(ESP8266)
  mySerial.begin(9600);
#endif

  Serial.println();
  Serial.println("Starting up...HELLAU!");

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
  groupAdditionalMeter.addItem(&read_meter_intervall_object);
  groupBackend.addItem(&backend_call_minute_object);
  groupTelegram.addItem(&Meter_Value_Buffer_Size_object);

  groupSys.addItem(&led_blink_object);
  groupAdditionalMeter.addItem(&mystrom_PV_object);
  groupAdditionalMeter.addItem(&mystrom_PV_IP_object);
  groupAdditionalMeter.addItem(&temperature_object);
  groupBackend.addItem(&UseSslCert_object);

  iotWebConf.setStatusPin(STATUS_PIN);
  iotWebConf.setConfigPin(CONFIG_PIN);
  iotWebConf.addParameterGroup(&groupSys);
  iotWebConf.addParameterGroup(&groupTelegram);
  iotWebConf.addParameterGroup(&groupBackend);
  iotWebConf.addParameterGroup(&groupAdditionalMeter);

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

int32_t myStrom_get_Meter_value()
{

  if (!mystrom_PV_object.isChecked())
  {
    return 0;
  }

  Serial.println(F("myStrom_get_Meter_value Connecting..."));

  // Connect to HTTP server
  WiFiClient client;
  client.setTimeout(1000);
  if (!client.connect(mystrom_PV_IP, 80))
  {
    Serial.println(F("myStrom_get_Meter_value Connection failed"));
    return -1;
  }

  Serial.println(F("myStrom_get_Meter_value Connected!"));

  // Send HTTP request
  client.println(F("GET /report HTTP/1.0"));
  client.print(F("Host: "));
  client.println(mystrom_PV_IP);
  client.println(F("Connection: close"));
  if (client.println() == 0)
  {
    Serial.println(F("Failed to send request"));
    client.stop();
    return -2;
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
    return -3;
  }

  // Skip HTTP headers
  char endOfHeaders[] = "\r\n\r\n";
  if (!client.find(endOfHeaders))
  {
    Serial.println(F("myStrom_get_Meter_value Invalid response"));
    client.stop();
    return -4;
  }

  // Allocate the JSON document
  JsonDocument doc;

  // Parse JSON object
  DeserializationError error = deserializeJson(doc, client);
  if (error)
  {
    Serial.print(F("myStrom_get_Meter_value deserializeJson() failed: "));
    Serial.println(error.f_str());
    client.stop();
    return -5;
  }

  // Extract values

  return (doc["energy_since_boot"].as<int>());

  // Disconnect
  client.stop();
}

int32_t MeterValue_get_from_remote()
{
  String meter_value;
  String lastLine;

  // Connect to HTTP server
  WiFiClient client;
  client.setTimeout(1000);
  if (!client.connect("192.168.0.2", 80))
  {
    Serial.println(F("MeterValue_get_from_remote Connection failed"));
    return -1;
  }
  // Serial.println(F("Connected!"));

  // Send HTTP request
  client.println(F("GET /showLastMeterValue HTTP/1.0"));
  client.print(F("Host: "));
  client.println("192.168.0.2");
  client.println(F("Connection: close"));
  if (client.println() == 0)
  {
    Serial.println(F("MeterValue_get_from_remote Failed to send request"));
    client.stop();
    return -2;
  }

  meter_value = client.readString();

  int lastNewlineIndex = meter_value.lastIndexOf('\n');

  // extract part of string after newline
  lastLine = meter_value.substring(lastNewlineIndex + 1);

  // delete spaces
  lastLine.trim();

  
  int32_t meter_value_i32 = lastLine.toInt();

  client.stop();
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
}


void Telegram_ResetReceiveBuffer()
{
  telegram_receive_bufferIndex = 0;
  readingExtraBytes = false;
  extraIndex = 0;
}

void Telegram_handle()
{
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
      // Log_AddEntry(3001);
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
  Log_AddEntry(meter_value_i);
  Serial.println("call_backend_V2");
  last_call_backend = millis();
  if (meter_value_i == 0)
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
  header += "&heap=";
  header += String(ESP.getFreeHeap());
  header += "&meter_value_i=";
  header += String(meter_value_i);
  header += "&meter_value_overflow=";
  header += String(meter_value_buffer_overflow);

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

void MeterValue_store()
{

  last_meter_value = millis();
  if (ESP.getFreeHeap() < 1000)
  {
    Log_AddEntry(1015);
    Serial.println("Not enough free heap to store another value");
    return;
  }

  int32_t meter_value = MeterValue_get_from_telegram();
  if (meter_value <= 0)
  {
    Log_AddEntry(1200);
    return;
  }

  if (meter_value == previous_meter_value)
  {
    Log_AddEntry(1201);
    return;
  }
  previous_meter_value = meter_value;

  Serial.println("buffer i: " + String(meter_value_i));

  MeterValues[meter_value_i].timestamp = timestamp_telegram; 
  MeterValues[meter_value_i].meter_value = meter_value;

  if (temperature_object.isChecked())
  {
    MeterValues[meter_value_i].temperature = temperature;
  }

  Serial.print("Free Heap: ");
  Serial.println(ESP.getFreeHeap());

  meter_value_i++;
  if (meter_value_i >= Meter_Value_Buffer_Size)
  {
    meter_value_i = 0;
    meter_value_buffer_overflow++;
  }
}

void handle_check_wifi_connection()
{
  if (restart_wifi && millis() - last_wifi_retry > 5000)
  {
    restart_wifi = false;
    iotWebConf.goOnLine(false);
    Log_AddEntry(7001);
  }

  if (millis() - last_wifi_check > 500)
  {
    last_wifi_check = millis();

    if (WiFi.status() == WL_CONNECTED && wifi_connected)
    {
      // Still wifi_connected
    }
    else if (WiFi.status() == WL_CONNECTED && !wifi_connected)
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
    else if (WiFi.status() != WL_CONNECTED && wifi_connected)
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
      temperature = (Temp_sensors.getTempCByIndex(0) * 100);
      read_temperature = true;
    }
  }
}
void handle_call_backend()
{
  if (wifi_connected && millis() - wifi_reconnection_time > 60000)
  {
    if ((!call_backend_successfull && millis() - last_call_backend > 30000) || (Time_getMinutes() % atoi(backend_call_minute) == 0 && millis() - last_call_backend > 60000))
    {
      Webclient_Send_Meter_Values_to_backend_wrapper();
      if (b_send_log_to_backend == true)
      {
        Webclient_Send_Log_to_backend_wrapper();
      }
    }
  }
}
void handle_MeterValue_store()
{
  if (!wifi_connected &&
      (Time_getEpochTime() - 1) % 900 < 60 && millis() - last_meter_value > 60000)
  {
    Log_AddEntry(1010);
    MeterValue_store();
  }
  if (!wifi_connected && millis() - last_meter_value > 900000)
  {
    Log_AddEntry(1014);
    MeterValue_store();
  }
  if (wifi_connected && millis() - last_meter_value > 1000UL * max(5UL, (unsigned long)atoi(read_meter_intervall)))
  {
    Log_AddEntry(1011);
    MeterValue_store();
  }
}
void loop()
{
  iotWebConf.doLoop();
  ArduinoOTA.handle();
  handle_temperature();
  Telegram_handle();
  handle_check_wifi_connection();
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

  String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
  s += "<title>" + String(thingName) + "</title></head><body>";
  s += "<br>Go to <a href='config'><b>configuration page</b></a> to change <i>italic</i> values.";
  s += "<br>Telegram Parse config<ul>";
  s += "<li><i>Meter Value Offset:</i> ";
  s += atoi(telegram_offset);
  s += "<li><i>Meter Value length:</i> ";
  s += atoi(telegram_length);
  s += "<li><i>Prefix Begin (usualy 0):</i> ";
  s += atoi(telegram_prefix);
  s += "<li><i>Suffix Begin: </i>";
  s += atoi(telegram_suffix);
  s += "<li>Detected Meter Value [1/10 Wh]: " + String(MeterValue_get_from_telegram());
  s += "<li><a href='showMeterValues'>Show Meter Values</a>";
  s += "<li><a href='showTelegram'>Show Telegram</a>";
  if (Meter_Value_Buffer_Size != atoi(Meter_Value_Buffer_Size_Char))
  {
    s += "<li><font color=red>Buffer Size changed, please ";
    if (meter_value_i > 0)
    {
      s += "<a href='sendMeterValues_Task'>Send Meter Values to Backend</a> to not lose (";
      s += String(meter_value_i);
      s += ") Meter Values and ";
    }
  }
  else
  {
    s += "<li><font>";
  }
  s += "<a href='MeterValue_init_Buffer'>Re-Init Meter Array</a></font>";
  s += "</ul>";
  s += "Backend Config";
  s += "<ul>";

  s += "<li><i>Backend Endpoint:</i> ";
  s += backend_endpoint;

  s += "<li>Backend Host: ";
  s += backend_host;
  s += "<li>Backend Path: ";
  s += backend_path;
  s += "<li><i>Backend ID:</i> ";
  s += backend_ID;
  s += "<li><i>Backend Token:</i> ";
  s += backend_token;
  s += "<li><i>Wirk-PKI (Use SSL Cert):</i> ";
  if (UseSslCert_object.isChecked())
    s += "true";
  else
  {
    s += "false";
  }
  s += "<li><a href='showCert'>Show Cert</a>";
  s += "<li><a href='setCert'>Set Cert</a>";
  s += "<li><a href='testBackendConnection'>Test Backend Connection</a>";
  s += "</ul>";
  s += "Meter Values";
  s += "<ul>";
  s += "<li><i>Taf14 Read Meter Intervall: </i>";
  s += atoi(read_meter_intervall);
  s += "<li><i>Backend call Minute:</i> ";
  s += atoi(backend_call_minute);
  s += "<li>Meter Value Buffer used: ";
  s += String(meter_value_i + meter_value_buffer_overflow * Meter_Value_Buffer_Size) + " / <i>" + String(Meter_Value_Buffer_Size);
  s += "</i><li>Last Backend Call ago (min): ";
  s += String((millis() - last_call_backend) / 60000);
  s += "<br><a href='StoreMeterValue'>Store Meter Value (Taf6)</a>";
  s += "<br><a href='sendStatus_Task'>Send Status Report to Backend</a>";
  s += "<br><a href='sendMeterValues_Task'>Send Meter Values to Backend</a>";
  s += "<br><a href='sendboth_Task'>Send Meter Values and Status Report to Backend</a>";
  s += "</ul>";

  s += "MyStrom config";
  s += "<ul>";
  s += "<li><i>MyStrom:</i> ";
  if (mystrom_PV_object.isChecked())
    s += "activated";
  else
  {
    s += "deactivated";
  }

  s += "<li><i>MyStrom PV IP: </i>";
  s += mystrom_PV_IP;
  s += "<li><b>Detected Meter Value PV</b>: " + String(myStrom_get_Meter_value());
  s += "</ul>";
  s += "Additional Meter";
  s += "<ul>";
  s += "<li><i>Temperature Sensor:</i> ";
  if (temperature_object.isChecked())
    s += "activated";
  else
  {
    s += "deactivated";
  }
  s += "<li>Temperature [1/100 C]: ";
  s += String(temperature);

  s += "</ul>";

  s += "System Info";
  s += "<ul>";
  s += "<li><i>LED blink:</i> ";
  if (led_blink_object.isChecked())
    s += "activated";
  else
  {
    s += "deactivated";
  }
  s += "<li>Water Mark meter Values: ";
  s += String(watermark_meter_buffer);
  s += "<li>Water Mark Logs: ";
  s += String(watermark_log_buffer);
  s += "<li>Uptime (min): ";
  s += Time_formatUptime();

#if defined(ESP32)

  s += "<li>Reset Reason: ";
  s += Log_get_reset_reason();
#elif defined(ESP8266)

  s += "<li>Reset Reason: ";
  s += String(/*esp_reset_reason()*/ ESP.getResetReason());
  s += " / ";
  s += String(/*esp_reset_reason()*/ ESP.getResetInfo());
#endif

  s += "<li>System time (UTC): ";
  s += String(Time_getFormattedTime());
  s += " / ";
  s += String(Time_getEpochTime());
  s += "<li>Build Time: "+String(BUILD_TIMESTAMP);
  s += "<li>Free Heap: ";
  s += String(ESP.getFreeHeap());
  s += "<li>Log Buffer Length (max): " + String(LOG_BUFFER_SIZE);
  s += "<br><a href='showLogBuffer'>Show entire Logbuffer</a>";
  s += "<br><a href='resetLogBuffer'>Reset Log</a>";
  s += "<br><a href='restart'>Restart</a>";
  s += "</ul>";

  s += "<br><br>Log Buffer (last 10 entries / current buffer index " + String(logIndex) + ")<br>";
  s += Log_BufferToString(10);

  s += "<br></body></html>\n";

  server.send(200, "text/html", s);
}
void Webserver_ShowCert()
{
  server.send(200, "text/html", String(FullCert));
}

void Webserver_ShowTermperature()
{
  server.send(200, "text/html", String(temperature));
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

  s += "<br>Received Telegram @ "+String(timestamp_telegram) + " = " +Time_formatTimestamp(timestamp_telegram) + ": "+String(Time_getEpochTime()-timestamp_telegram)+"s old<br><table border=1>";
  
  if (!Telegram_prefix_suffix_correct())
    s += "<br><font color=red>incomplete telegram</font>";

  String color;

  int signature_7101 = 9999;
  for (int i = 0; i < TELEGRAM_LENGTH; i++)
  {
    if (i < TELEGRAM_LENGTH - 5 && TELEGRAM[i] == 7 && TELEGRAM[i + 1] == 1 && TELEGRAM[i + 2] == 0 && TELEGRAM[i + 3] == 1 && TELEGRAM[i + 4] == 8)
    {
      color = "bgcolor=959018";
      signature_7101 = i;
    }
    else if (i > signature_7101 && TELEGRAM[i] == 0x77)
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
    s += "<tr><td>" + String(i) + "</td><td " + String(color) + ">" + String(TELEGRAM[i], HEX) + "</td></tr>";
  }
  s += "</table";

  s += "</body></html>\n";

  server.send(200, "text/html", s);
}

void Webserver_ShowLastMeterValue()
{
  // -- Let IotWebConf test and handle captive portal requests.
  if (iotWebConf.handleCaptivePortal())
  {
    // -- Captive portal request were already served.
    return;
  }

  server.send(200, "text/html", String(MeterValue_get_from_telegram()));
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