#include <IotWebConf.h>
#include <IotWebConfUsing.h> // This loads aliases for easier class names.
#include <SPIFFS.h>

// -- Initial name of the Thing. Used e.g. as SSID of the own Access Point.
const char thingName[] = "SMGW.Lite";

// -- Initial password to connect to the Thing, when it creates an own Access Point.
const char wifiInitialApPassword[] = "password";

#define STRING_LEN 128
#define ID_LEN 4
#define NUMBER_LEN 32

// -- Configuration specific key. The value should be modified if config structure was changed.
#define CONFIG_VERSION "1015"

#define HOST_REACHABLE 0x01
#define CERT_CORRECT 0x02

// -- When CONFIG_PIN is pulled to ground on startup, the Thing will use the initial
//      password to buld an AP. (E.g. in case of lost password)
#ifndef D2
#define D2 3
#endif

#define CONFIG_PIN D2

// -- Status indicator pin.
//      First it will light up (kept LOW), on Wifi connection it will blink,
//      when connected to the Wifi it will turn off (kept HIGH).
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

#define STATUS_PIN LED_BUILTIN

int m_i = 0;
int m_i_max = 0;


// Telegramm-Signaturen
const uint8_t SIGNATURE_START[] = {0x1b, 0x1b, 0x1b, 0x1b, 0x01, 0x01, 0x01, 0x01};
const uint8_t SIGNATURE_END[] = {0x1b, 0x1b, 0x1b, 0x1b, 0x1a};


#define TELEGRAM_LENGTH 512
#define TELEGRAM_TIMEOUT_MS 30 // Timeout für Telegramme in Millisekunden
size_t gTelegramSizeUsed = 0;   // Tatsächliche Länge des gespeicherten Telegramms

uint8_t telegram_receive_buffer[TELEGRAM_LENGTH]; // Eingabepuffer für serielle Daten
size_t telegram_receive_bufferIndex = 0;      // Aktuelle Position im Eingabepuffer
bool readingExtraBytes = false;       // Status: Lesen der zusätzlichen Bytes
uint8_t extraBytes[3];                // Zusätzliche Bytes nach der Endsignatur
size_t extraIndex = 0;                // Index für zusätzliche Bytes
unsigned long lastByteTime = 0;       // Zeitstempel des letzten empfangenen Bytes
unsigned long timestamp_telegram;
uint8_t TELEGRAM[TELEGRAM_LENGTH]; // Speicher für das vollständige Telegramm

int meter_value_i = 0;

// Struktur für die Messwerte
struct MeterValue
{
  uint32_t timestamp;   // 4 Bytes (Unix-Timestamp)
  uint32_t meter_value; // 4 Bytes (Zahl bis ~4 Mio.)
  uint32_t temperature; // 4 Bytes (Zahl bis ~4 Mio.)
};

MeterValue *MeterValues = nullptr; // Globaler Zeiger, initialisiert mit nullptr

unsigned long last_meter_value = 0;
unsigned long previous_meter_value = 0;

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

bool wifi_connected;

#include <OneWire.h>
#include <DallasTemperature.h>
#define ONE_WIRE_BUS 4
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature Temp_sensors(&oneWire);

#if defined(ESP32)
HardwareSerial mySerial(1); // RX, TX
#elif defined(ESP8266)
SoftwareSerial mySerial(D5, D6); // RX, TX
#endif
const int LOG_BUFFER_SIZE = 100;
// -- Forward declarations.

void AddLogEntry(int statusCode);
void configSaved();
void handle_call_backend();
void handle_check_wifi_connection();
void handle_MeterValue_store();
void handle_temperature();
void handle_telegram2();
void webClient_loadCertToChar();
void LogBuffer_reset();
void MeterValue_init_Buffer();
void MeterValues_clear_Buffer();
int32_t MeterValue_get_from_remote();
int32_t MeterValue_get_from_telegram();
void MeterValue_store();
void setup();
String LogBufferToString(int showNumber = LOG_BUFFER_SIZE);
String LogEntryToString(int i);
String LogStatusCodeToString(int statusCode);
String time_formatTimestamp(unsigned long timestamp);
String time_formatUptime();
String time_getFormattedTime();
unsigned long time_getEpochTime();
int time_getMinutes();
bool Telegram_prefix_suffix_correct();
void Telegram_saveCompleteTelegram();
void TelegramResetReceiveBuffer();
void webclient_send_log_to_backend();
void webclient_Send_Log_to_backend_Task(void *pvParameters);
void webclient_Send_Log_to_backend_wrapper();
void webclient_send_meter_values_to_backend();
void webclient_Send_Meter_Values_to_backend_Task(void *pvParameters);
void webclient_Send_Meter_Values_to_backend_wrapper();
void webclient_splitHostAndPath(const String &url, String &host, String &path);
void webserverHandleCertUpload();
void webserverHandleRoot();
void webserverLocationHrefHome(int delay = 0);
void webserverSetCert();
void webserverShowCert();
void webserverShowLastMeterValue();
void webserverShowLogBuffer();
void webserverShowMeterValues();
void webserverShowTelegram();
void webserverShowTermperature();
void webserverTestBackendConnection();
void loop();
#if defined(ESP32)
String esp_reset_reason_string();
#endif


int watermark_meter_buffer = 0;
int watermark_log_buffer = 0;

unsigned long wifi_reconnection_time = 0;
unsigned long last_wifi_retry = 0;
bool restart_wifi = false;
DNSServer dnsServer;
WebServer server(80);

int temperature;
int meter_value_buffer_overflow = 0;
char telegram_offset[NUMBER_LEN];
char telegram_length[NUMBER_LEN];
char telegram_prefix[NUMBER_LEN];
char telegram_suffix[NUMBER_LEN];
char Meter_Value_Buffer_Size_Char[NUMBER_LEN] = "123";
int Meter_Value_Buffer_Size = 234;

char backend_endpoint[STRING_LEN];
char led_blink[STRING_LEN];
char UseSslCertValue[STRING_LEN]; // Platz für "0" oder "1"
char mystrom_PV[STRING_LEN];
char mystrom_PV_IP[STRING_LEN];
char temperature_checkbock[STRING_LEN];
char backend_token[STRING_LEN];
char read_meter_intervall[NUMBER_LEN];
char backend_call_minute[NUMBER_LEN];
char backend_ID[ID_LEN];

IotWebConf iotWebConf(thingName, &dnsServer, &server, wifiInitialApPassword, CONFIG_VERSION);
// -- You can also use namespace formats e.g.: iotwebconf::TextParameter

IotWebConfParameterGroup groupTelegram = IotWebConfParameterGroup("groupTelegram", "Telegram Param");
IotWebConfParameterGroup groupBackend = IotWebConfParameterGroup("groupBackend", "Backend Config");
IotWebConfParameterGroup groupAdditionalMeter = IotWebConfParameterGroup("groupAdditionalMeter", "Additional Meters & Sensors");
IotWebConfParameterGroup groupSys = IotWebConfParameterGroup("groupSys", "Advanced Sys Config");

// IotWebConfParameterGroup groupSslCert = IotWebConfParameterGroup("groupSslCert", "SSL Cert");
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





unsigned long time_getEpochTime()
{
  return static_cast<unsigned long>(time(nullptr));
}

int time_getMinutes()
{
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo); // Konvertiere in lokale Zeitstruktur

  return timeinfo.tm_min; // Extrahiere Minuten (0–59)
}

// Definition des Logbuffers

struct LogEntry
{
  unsigned long timestamp; // Zeitstempel in Millisekunden seit Start
  unsigned long uptime;    // Betriebszeit in Sekunden
  int statusCode;          // Statuscode
};
LogEntry logBuffer[LOG_BUFFER_SIZE];
int logIndex = -1; // Index des nächsten Eintrags
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
void AddLogEntry(int statusCode)
{
  // Aktualisiere den Index (Ring-Puffer-Verhalten)
  logIndex = (logIndex + 1) % LOG_BUFFER_SIZE;

  unsigned long uptimeSeconds = millis() / 60000; // Uptime in Sekunden

  // Füge neuen Eintrag in den Ring-Buffer ein
  logBuffer[logIndex].timestamp = time_getEpochTime();
  logBuffer[logIndex].uptime = uptimeSeconds;
  logBuffer[logIndex].statusCode = statusCode;
}
void MeterValue_init_Buffer()
{
  Meter_Value_Buffer_Size = atoi(Meter_Value_Buffer_Size_Char);
  // Alten Speicher freigeben, falls bereits allokiert
  if (MeterValues)
  {
    delete[] MeterValues;
    MeterValues = nullptr; // Setze auf nullptr, um sicherzustellen, dass kein Wildpointer entsteht
  }

  // Speicher reservieren
  MeterValues = new (std::nothrow) MeterValue[Meter_Value_Buffer_Size];
  if (!MeterValues)
  {
    Serial.println("Speicherzuweisung fehlgeschlagen!");
    AddLogEntry(1002);
  }

  MeterValues_clear_Buffer();
}


bool b_send_log_to_backend = false;
// Funktion zur Hinzufügung eines neuen Log-Eintrags
bool firstTime = true;
String LogStatusCodeToString(int statusCode)
{
  switch (statusCode)
  {
  case 1001:
    return "setup()";
  case 1002:
    return "Speicherzuweisung fehlgeschlagen!";
  case 1003:
    return "Config gespeichert";    
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
    return "Telegram Pufferueberlauf";
  case 3002:
    return "Telegram timeout";
  case 3003:
    return "Telegramm zu groß für Speicher";
  case 4000:
    return "Connection to server failed (Cert!?)";
  case 7000:
    return "Stopping Wifi, Backendcall unsuccessfull";
  case 7001:
    return "Restarting Wifi";
  case 8000:
    return "Spiffs not mounted";
  case 8001:
    return "Fehler beim Öffnen der Zertifikatsdatei!";
  case 8002:
    return "Zertifikat gespeichert";
  case 8003:
    return "Fehler beim Öffnen der Cert Datei";
  case 8004:
    return "Kein Zertifikat erhalten!";
  }
  if (statusCode < 200)
  {
    return "# values transmitted";
  }

  return "Unknown status code";
}


String time_getFormattedTime()
{
  time_t now = time(nullptr);
  char timeStr[64];
  struct tm timeinfo;
  localtime_r(&now, &timeinfo); // Lokale Zeit (Zeitzone angewendet)

  strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo); // Zeitformat HH:MM:SS
  return String(timeStr);                                    // Gibt Zeit als lesbaren String aus
}


String time_formatTimestamp(unsigned long timestamp)
{
  // Konvertiere Unix-Timestamp in lokale Zeit
  time_t rawTime = static_cast<time_t>(timestamp);
  struct tm timeinfo;
  localtime_r(&rawTime, &timeinfo);

  char buffer[20];
  strftime(buffer, sizeof(buffer), "%D %H:%M:%S", &timeinfo);
  return String(buffer);
}
String LogEntryToString(int i)
{
  if (logBuffer[i].statusCode == 0)
    return ""; // Leere Einträge überspringen
  String logString = "<tr><td>";
  logString += String(i) + "</td><td>";
  logString += String(logBuffer[i].timestamp) + "</td><td>";
  logString += time_formatTimestamp(logBuffer[i].timestamp) + "</td><td>";
  logString += String(logBuffer[i].uptime) + "</td><td>";
  logString += String(logBuffer[i].statusCode) + "</td><td>";
  logString += LogStatusCodeToString(logBuffer[i].statusCode);

  logString += "</td></tr>";
  return logString;
}
String LogBufferToString(int showNumber)
{
  int showed_number = 0;
  String logString = "<table border=1><tr><th>Index</th><th>Timestamp</th><th>Timestamp</th><th>Uptime</th><th>Statuscode</th><th>Status</th></tr>";

  // Erste Schleife: Neuerer Bereich (ab logIndex rückwärts bis 0)
  for (int i = logIndex; i >= 0; i--)
  {
    logString += LogEntryToString(i);
    showed_number++;
    if(showed_number >= showNumber)
    {
      return logString + "</table>";
    }
  }
 
  // Zweite Schleife: Älterer Bereich (vom Ende des Buffers rückwärts bis nach logIndex)
  if (logIndex < LOG_BUFFER_SIZE - 1)
  {
    // logString += "<tr><td>-----</td></tr>";
    for (int i = LOG_BUFFER_SIZE - 1; i > logIndex; i--)
    {
      logString += LogEntryToString(i);
      showed_number++;
      if(showed_number >= showNumber)
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
void webserverLocationHrefHome(int delay)
{
  String call = "<meta http-equiv='refresh' content = '" + String(delay) + ";url=/'>";
  server.send(200, "text/html", call);
}
void webserverShowMeterValues()
{
  String MeterValues_string = "<table border='1'><tr><th>Index</th><th>Timestamp</th><th>Meter Value</th><th>Termperature </th></tr>";
  for (int m = 0; m < Meter_Value_Buffer_Size; m++)
  {
    MeterValues_string += "<tr><td>" + String(m) + "</td><td>" + String(MeterValues[m].timestamp) + "</td><td>" + String(MeterValues[m].meter_value) + "</td><td>" + String(MeterValues[m].temperature) + "</td></tr>";
  }
  MeterValues_string += "</table>";
  server.send(200, "text/html", MeterValues_string);
}
void webserverShowLogBuffer()
{
  server.send(200, "text/html", LogBufferToString());
}
void webclient_splitHostAndPath(const String &url, String &host, String &path)
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
void webserverSetCert()
{
  server.send(200, "text/html", "<form action='/upload' method='POST'><textarea name='cert' rows='10' cols='80'>" + String(FullCert) + "</textarea><br><input type='submit'></form>");
}

void webserverTestBackendConnection()
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
    client.setInsecure(); // Falls Zertifikat nicht akzeptiert wird
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

  // ** Anfrage mit ID & Token an das Backend senden **
  String url = backend_path + "?backend_test=true&ID=" + String(backend_ID) + "&token=" + String(backend_token);
  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + backend_host + "\r\n" +
               "Connection: close\r\n\r\n");

  // Warte auf die Antwort
  unsigned long timeout = millis();
  while (client.available() == 0)
  {
    if (millis() - timeout > 5000) // Timeout nach 5 Sekunden
    {
      res += "<br>>No response from server.";
      server.send(200, "text/html", res);
      return;
    }
  }

  // Antwort vom Server lesen
  String response = "";
  while (client.available())
  {
    response += client.readString();
  }

  // ** Prüfen, ob Authentifizierung erfolgreich war **
  if (response.indexOf("200") != -1) // Backend sendet JSON {"success": true}
  {
    res += "<br>ID & Token valid.";
  }
  else
  {
    res += "<br>ID & Token invalid!";
  }
  server.send(200, "text/html", res);
}
void webserverHandleCertUpload()
{
  if (server.hasArg("cert"))
  {
    String cert = server.arg("cert");
    File file = SPIFFS.open("/cert.pem", FILE_WRITE);
    if (file)
    {
      file.println(cert);
      file.close();
      AddLogEntry(8002);
      webserverLocationHrefHome();
    }
    else
    {
      AddLogEntry(8003);
      webserverLocationHrefHome();
      server.send(500, "text/plain", "Fehler beim Öffnen der Datei!");
    }
  }
  else
  {
    AddLogEntry(8004);
    webserverLocationHrefHome();
  }
}
void webClient_loadCertToChar()
{

  File file = SPIFFS.open("/cert.pem", FILE_READ);

  if (!file)
  {

    AddLogEntry(8001);
    return;
  }

  size_t size = file.size();

  // Lese den Dateiinhalt in das char-Array

  file.readBytes(FullCert, size);

  // Füge das Nullterminierungssymbol hinzu

  FullCert[size] = '\0';

  file.close();
}
bool call_backend_V2_successfull = true;
SemaphoreHandle_t Sema_Backend; // Mutex für synchronisierten Zugriff
unsigned long last_call_backend_v2 = 0;

void webclient_Send_Meter_Values_to_backend_Task(void *pvParameters)
{
  if (xSemaphoreTake(Sema_Backend, portMAX_DELAY))
  {
    webclient_send_meter_values_to_backend();
    xSemaphoreGive(Sema_Backend);
  }
  watermark_meter_buffer = uxTaskGetStackHighWaterMark(NULL);
  vTaskDelete(NULL); // Task löschen, wenn fertig
}
void webclient_Send_Log_to_backend_Task(void *pvParameters)
{
  b_send_log_to_backend = false;
  if (xSemaphoreTake(Sema_Backend, portMAX_DELAY))
  {
    webclient_send_log_to_backend();
    xSemaphoreGive(Sema_Backend);
  }
  else
  {
    // b_send_log_to_backend = true;
  }
  watermark_log_buffer = uxTaskGetStackHighWaterMark(NULL);
  vTaskDelete(NULL); // Task löschen, wenn fertig
}

void setup()
{
  Sema_Backend = xSemaphoreCreateMutex();
  AddLogEntry(1001);
  Serial.begin(115200);

#if defined(ESP32)
  mySerial.begin(9600, SERIAL_8N1, 15, 16);
#elif defined(ESP8266)
  mySerial.begin(9600);
#endif

  Serial.println();
  Serial.println("Starting up...HELLAU!");

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

  iotWebConf.setConfigSavedCallback(&configSaved);
  // iotWebConf.setFormValidator(&formValidator);
  iotWebConf.getApTimeoutParameter()->visible = true;

  // -- Initializing the configuration.
  iotWebConf.skipApStartup();
  iotWebConf.init();

  if (led_blink_object.isChecked())
    iotWebConf.enableBlink();
  else
  {
    iotWebConf.disableBlink();
    digitalWrite(LED_BUILTIN, LOW);
  }

  // -- Set up required URL handlers on the web server.
  server.on("/", webserverHandleRoot);
  server.on("/showTelegram", webserverShowTelegram);
  server.on("/showLastMeterValue", webserverShowLastMeterValue);
  server.on("/showTemperature", webserverShowTermperature);
  server.on("/showCert", webserverShowCert);
  server.on("/setCert", webserverSetCert);
  server.on("/testBackendConnection", webserverTestBackendConnection);
  server.on("/showMeterValues", webserverShowMeterValues);
  server.on("/showLogBuffer", webserverShowLogBuffer);
  

  server.on("/upload", []
            {
              webserverHandleCertUpload();
              webClient_loadCertToChar(); });

  server.on("/config", []
            { iotWebConf.handleConfig(); });
  server.on("/restart", []
            { 
              webserverLocationHrefHome(5);
              ESP.restart(); });
  server.on("/resetLogBuffer", []
            { 
              webserverLocationHrefHome();
              LogBuffer_reset(); });
  server.on("/StoreMeterValue", []
            { webserverLocationHrefHome();
            AddLogEntry(1006);
            MeterValue_store(); });
  server.on("/MeterValue_init_Buffer", []
            {MeterValue_init_Buffer();
            webserverLocationHrefHome(); });
  server.on("/sendboth_Task", []
            { 
            
            webserverLocationHrefHome(2);

            webclient_Send_Meter_Values_to_backend_wrapper();
            webclient_Send_Log_to_backend_wrapper(); });
  server.on("/sendStatus_Task", []
            { 
            webserverLocationHrefHome(2);
            webclient_Send_Log_to_backend_wrapper(); });

  server.on("/sendMeterValues_Task", []
            { 
            
            webserverLocationHrefHome(2);
            webclient_Send_Meter_Values_to_backend_wrapper(); });

  server.on("/setOffline", []
            { wifi_connected = false;
            webserverLocationHrefHome(); });

  server.onNotFound([]()
                    { iotWebConf.handleNotFound(); });

  Serial.println("Ready.");
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

  webclient_splitHostAndPath(String(backend_endpoint), backend_host, backend_path);

  if (!SPIFFS.begin(true))
  {
    AddLogEntry(8000);
  }
  webClient_loadCertToChar();

  MeterValue_init_Buffer();

  configTime(0, 0, "ptbnts1.ptb.de", "ptbtime1.ptb.de", "ptbtime2.ptb.de");
}
void webclient_Send_Meter_Values_to_backend_wrapper()
{
  xTaskCreate(webclient_Send_Meter_Values_to_backend_Task, "Send_Meter task", 8192, NULL, 2, NULL);
}

void webclient_Send_Log_to_backend_wrapper()
{
  xTaskCreate(webclient_Send_Log_to_backend_Task, "send log task", 8192, NULL, 2, NULL);
}
// uint8_t BUFFER[TELEGRAM_LENGTH] = {0};
bool Telegram_prefix_suffix_correct()
{
  int prefix = atoi(telegram_prefix);
  int suffix = atoi(telegram_suffix);

  if (suffix == 0)
  {
    AddLogEntry(1203);
    Serial.println("Suffix Must not be 0");
    return false;
  }

  if (TELEGRAM[suffix] == 0x1B && TELEGRAM[suffix + 1] == 0x1B && TELEGRAM[suffix + 2] == 0x1B && TELEGRAM[suffix + 3] == 0x1B && TELEGRAM[prefix] == 0x1B && TELEGRAM[prefix + 1] == 0x1B && TELEGRAM[prefix + 2] == 0x1B && TELEGRAM[prefix + 3] == 0x1B)
    return true;
  else
  {
    AddLogEntry(1204);
    return false;
  }
}


int32_t MeterValue_get_from_telegram()
{

  // return MeterValue_get_from_remote();

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
  // Serial.println(F("Connecting..."));

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

  // Extrahiere den Teil des Strings nach dem letzten Zeilenumbruch
  lastLine = meter_value.substring(lastNewlineIndex + 1);

  // Entferne mögliche Leerzeichen (falls vorhanden)
  lastLine.trim();

  // Konvertiere die letzte Zeile in eine Zahl
  int32_t meter_value_i32 = lastLine.toInt();

  return meter_value_i32;
  client.stop();
}

// Funktion zum Speichern eines vollständigen Telegramms
void Telegram_saveCompleteTelegram()
{
  size_t telegramLength = telegram_receive_bufferIndex + 3; // Telegrammlänge inkl. zusätzlicher Bytes
  if (telegramLength > TELEGRAM_LENGTH)
  {
    // Serial.println("Fehler: Telegramm zu groß für Speicher!");
    AddLogEntry(3003);
    return;
  }

  // Telegramm in TELEGRAM-Array kopieren
  memcpy(TELEGRAM, telegram_receive_buffer, telegram_receive_bufferIndex); // Kopiere Hauptdaten
  memcpy(TELEGRAM + telegram_receive_bufferIndex, extraBytes, 3);  // Kopiere zusätzliche Bytes
  gTelegramSizeUsed = telegramLength;
  timestamp_telegram = time_getEpochTime();
}

// Funktion zum Zurücksetzen des Eingabepuffers
void TelegramResetReceiveBuffer()
{
  telegram_receive_bufferIndex = 0;
  readingExtraBytes = false;
  extraIndex = 0;
}



void handle_telegram2()
{
  // Prüfen, ob Daten verfügbar sind
  while (mySerial.available() > 0)
  {
    uint8_t incomingByte = mySerial.read();
    lastByteTime = millis(); // Zeitstempel aktualisieren

    // Prüfen, ob zusätzliche Bytes gelesen werden müssen
    if (readingExtraBytes)
    {
      extraBytes[extraIndex++] = incomingByte;

      // Wenn alle zusätzlichen Bytes gelesen wurden
      if (extraIndex == 3)
      {
        Telegram_saveCompleteTelegram(); // Telegramm speichern
        TelegramResetReceiveBuffer();          // Eingabepuffer zurücksetzen
        // AddLogEntry(3000);
      }
      continue;
    }

    // Byte im Eingabepuffer speichern
    if (telegram_receive_bufferIndex < TELEGRAM_LENGTH)
    {
      telegram_receive_buffer[telegram_receive_bufferIndex++] = incomingByte;
    }
    else
    {
      // Fehler: Pufferüberlauf
      // Serial.println("Fehler: Pufferüberlauf! Eingabepuffer zurückgesetzt.");
      // AddLogEntry(3001);
      TelegramResetReceiveBuffer();
      continue;
    }

    // Prüfen, ob die Startsignatur erkannt wurde
    if (telegram_receive_bufferIndex >= sizeof(SIGNATURE_START) &&
        memcmp(telegram_receive_buffer, SIGNATURE_START, sizeof(SIGNATURE_START)) == 0)
    {

      // Prüfen, ob die Endsignatur erkannt wurde
      if (telegram_receive_bufferIndex >= sizeof(SIGNATURE_START) + sizeof(SIGNATURE_END))
      {
        if (memcmp(&telegram_receive_buffer[telegram_receive_bufferIndex - sizeof(SIGNATURE_END)], SIGNATURE_END, sizeof(SIGNATURE_END)) == 0)
        {
          // Endsignatur erkannt, auf zusätzliche Bytes warten
          readingExtraBytes = true;
        }
      }
    }
  }

  // Prüfen, ob ein Timeout aufgetreten ist
  if (telegram_receive_bufferIndex > 0 && (millis() - lastByteTime > TELEGRAM_TIMEOUT_MS))
  {
    // Serial.println("Fehler: Timeout! Eingabepuffer zurückgesetzt.");
    // AddLogEntry(3002);
    TelegramResetReceiveBuffer(); // Eingabepuffer zurücksetzen
  }
}



void webclient_send_log_to_backend()
{
  Serial.println("send_status_report");
  AddLogEntry(1019);
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
    // b_send_log_to_backend = true;
    AddLogEntry(4000);
    return;
  }

  // Binärdaten des LogBuffers in Puffer schreiben
  size_t logBufferSize = LOG_BUFFER_SIZE * sizeof(LogEntry);
  uint8_t *logDataBuffer = (uint8_t *)malloc(logBufferSize);
  if (!logDataBuffer)
  {
    Serial.println("Log buffer allocation failed");
    return;
  }
  memcpy(logDataBuffer, logBuffer, logBufferSize);

  // HTTP POST-Anfrage für den Log-Buffer erstellen
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
  // Header und Log-Daten senden
  client.print(logHeader);
  client.write(logDataBuffer, logBufferSize);

  free(logDataBuffer); // Speicher freigeben

  // Antwort des Servers lesen
  while (client.connected() || client.available())
  {
    if (client.available())
    {
      String line = client.readStringUntil('\n');
      Serial.println(line);

      if (line.startsWith("HTTP/1.1 200"))
      {
        Serial.println("Log successfully sent");

        AddLogEntry(1020);
        b_send_log_to_backend = false;
        break;
      }
      else
      {
        b_send_log_to_backend = true;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10)); // Wartet genau 10 ms, egal wie CONFIG_FREERTOS_HZ gesetzt ist
  }
  client.stop();
}

void webclient_send_meter_values_to_backend()
{
  AddLogEntry(1005);
  AddLogEntry(meter_value_i);
  Serial.println("call_backend_V2");
  last_call_backend_v2 = millis();
  if (meter_value_i == 0)
  {

    Serial.println("Zero Values to transmit");
    AddLogEntry(0);
    call_backend_V2_successfull = true;
    return;
  }

  call_backend_V2_successfull = false;

  // Verbindung zum Server herstellen
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
    AddLogEntry(4000);
    return;
  }

  // Binärdaten in Puffer schreiben
  size_t bufferSize = Meter_Value_Buffer_Size * sizeof(MeterValue);
  // size_t bufferSize = (meter_value_i+1) * 3 * sizeof(unsigned long);
  uint8_t *buffer = (uint8_t *)malloc(bufferSize);
  if (!buffer)
  {
    Serial.println("Buffer allocation failed");
    return;
  }
  memcpy(buffer, MeterValues, bufferSize);

  // HTTP POST-Anfrage manuell erstellen
  String header = "POST ";
  header += backend_path;
  header += "?ID=";
  header += backend_ID;
  header += "&token=";
  header += String(backend_token);
  header += "&uptime=";
  header += String(millis() / 60000);
  header += "&time=";
  header += String(time_getFormattedTime());
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

  // Header senden
  client.print(header);

  // Binärdaten senden
  client.write(buffer, bufferSize);

  free(buffer); // Speicher freigeben

  // Antwort des Servers lesen
  while (client.connected() || client.available())
  {
    if (client.available())
    {
      String line = client.readStringUntil('\n');
      Serial.println(line);

      if (line.startsWith("HTTP/1.1 200"))
      {
        Serial.println("MeterValues successfully sent");
        call_backend_V2_successfull = true;
        MeterValues_clear_Buffer();
        last_call_backend_v2 = millis();
        AddLogEntry(1021);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10)); // Wartet genau 10 ms, egal wie CONFIG_FREERTOS_HZ gesetzt ist
  }

  client.stop();
}


void MeterValue_store()
{

  last_meter_value = millis();
  if (ESP.getFreeHeap() < 1000)
  {
    AddLogEntry(1015);
    Serial.println("Not enough free heap to store another value");
    return;
  }

  int32_t meter_value = MeterValue_get_from_telegram();
  if (meter_value <= 0)
  {
    AddLogEntry(1200);
    return;
  }

  if (meter_value == previous_meter_value)
  {
    AddLogEntry(1201);
    return;
  }
  previous_meter_value = meter_value;

  Serial.println("buffer i: " + String(meter_value_i));

  MeterValues[meter_value_i].timestamp = timestamp_telegram; // time_getEpochTime();
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

unsigned long last_wifi_check;
int read_meter_intervall_int = 0;

void handle_call_backend()
{
}
void handle_MeterValue_store()
{
}
void handle_check_wifi_connection()
{
}

unsigned long last_temp = 0;
bool read_temp = false;

void handle_temperature()
{
  if (temperature_object.isChecked())
  {
    if (read_temp == true && millis() - last_temp > 20000)
    {
      last_temp = millis();
      Temp_sensors.requestTemperatures();
      read_temp = false;
    }
    else if (read_temp == false && millis() - last_temp > 1000)
    {
      last_temp = millis();
      temperature = (Temp_sensors.getTempCByIndex(0) * 100);
      read_temp = true;
    }
  }
}
void loop()
{

  iotWebConf.doLoop();
  ArduinoOTA.handle();
  handle_temperature();
  handle_telegram2();

  if (restart_wifi && millis() - last_wifi_retry > 5000)
  {
    restart_wifi = false;
    iotWebConf.goOnLine(false);
    AddLogEntry(7001);
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
      AddLogEntry(1008);
      Serial.println("Connection has returned: Resetting Backend Timer, starting OTA");
      ArduinoOTA.begin();
      wifi_connected = true;
      wifi_reconnection_time = millis();
      call_backend_V2_successfull = false;

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
      AddLogEntry(1009);
      wifi_connected = false;
    }
    else
    {
      // Still offline
    }
  }

  if (!wifi_connected &&
      (time_getEpochTime() - 1) % 900 < 60 && millis() - last_meter_value > 60000)
  {
    AddLogEntry(1010);
    MeterValue_store();
  }
  if (!wifi_connected && millis() - last_meter_value > 900000)
  {
    AddLogEntry(1014);
    MeterValue_store();
  }
  if (wifi_connected && millis() - last_meter_value > 1000UL * max(5UL, (unsigned long)atoi(read_meter_intervall)))
  {
    AddLogEntry(1011);
    MeterValue_store();
  }

  if (wifi_connected && millis() - wifi_reconnection_time > 60000)
  {
    if ((!call_backend_V2_successfull && millis() - last_call_backend_v2 > 30000) || (time_getMinutes() % atoi(backend_call_minute) == 0 && millis() - last_call_backend_v2 > 60000))
    {
      webclient_Send_Meter_Values_to_backend_wrapper();
      if (b_send_log_to_backend == true)
      {
        webclient_Send_Log_to_backend_wrapper();
      }
    }
  }
}
#if defined(ESP32)
String esp_reset_reason_string()
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
/**
 * Handle web requests to "/" path.
 */
String time_formatUptime()
{
  int64_t uptimeMicros = esp_timer_get_time(); // Zeit in Mikrosekunden
  int64_t uptimeMillis = uptimeMicros / 1000;  // In Millisekunden umrechnen
  int64_t uptimeSeconds = uptimeMillis / 1000; // In Sekunden umrechnen

  // Berechnung der Tage, Stunden, Minuten, Sekunden
  int days = uptimeSeconds / 86400;
  uptimeSeconds %= 86400;
  int hours = uptimeSeconds / 3600;
  uptimeSeconds %= 3600;
  int minutes = uptimeSeconds / 60;
  int seconds = uptimeSeconds % 60;

  // Formatierte Ausgabe "dd hh-mm-ss"
  char buffer[20];
  sprintf(buffer, "%02dd %02dh%02dm%02ds", days, hours, minutes, seconds);
  return String(buffer);
}
void webserverHandleRoot()
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
  s += "<li>Detected Meter Value: " + String(MeterValue_get_from_telegram());
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
  s += String(meter_value_i + meter_value_buffer_overflow * Meter_Value_Buffer_Size) + " / " + String(Meter_Value_Buffer_Size);
  s += "<li>Last Backend Call ago (min): ";
  s += String((millis() - last_call_backend_v2) / 60000);
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
  s += "<li>temperatur: ";
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
  s += time_formatUptime();

#if defined(ESP32)

  s += "<li>Reset Reason: ";
  s += esp_reset_reason_string();
#elif defined(ESP8266)

  s += "<li>Reset Reason: ";
  s += String(/*esp_reset_reason()*/ ESP.getResetReason());
  s += " / ";
  s += String(/*esp_reset_reason()*/ ESP.getResetInfo());
#endif

  s += "<li>Systemzeit: ";
  s += String(time_getFormattedTime());
  s += " / ";
  s += String(time_getEpochTime());
  s += "<li>Free Heap: ";
  s += String(ESP.getFreeHeap());
  s += "<li>Log Buffer Length (max): " + String(LOG_BUFFER_SIZE);
  s += "<br><a href='showLogBuffer'>Show entire Logbuffer</a>";
  s += "<br><a href='resetLogBuffer'>Reset Log</a>";
  s += "<br><a href='restart'>Restart</a>";
  s += "</ul>";

  s += "<br><br>Log Buffer (last 5 entries / index " + String(logIndex) + ")<br>";
  s += LogBufferToString(5);

  s += "<br></body></html>\n";

  server.send(200, "text/html", s);
}
void webserverShowCert()
{
  server.send(200, "text/html", String(FullCert));
}

void webserverShowTermperature()
{
  Temp_sensors.requestTemperatures();
  server.send(200, "text/html", String(temperature));
}
void webserverShowTelegram()
{
  // -- Let IotWebConf test and handle captive portal requests.
  if (iotWebConf.handleCaptivePortal())
  {
    // -- Captive portal request were already served.
    return;
  }
  String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";

  s += "<br>Received Telegram from mMe via SML<br><table border=1>";
  // receive_telegram(); // Wait for complete Telegram
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

void webserverShowLastMeterValue()
{
  // -- Let IotWebConf test and handle captive portal requests.
  if (iotWebConf.handleCaptivePortal())
  {
    // -- Captive portal request were already served.
    return;
  }

  server.send(200, "text/html", String(MeterValue_get_from_telegram()));
}

void configSaved()
{
  Serial.println("Configuration was updated.");
  if (led_blink_object.isChecked())
    iotWebConf.enableBlink();
  else
  {
    iotWebConf.disableBlink();
    digitalWrite(LED_BUILTIN, LOW);
  }
  webclient_splitHostAndPath(String(backend_endpoint), backend_host, backend_path);
  AddLogEntry(1003);
}
