#include <IotWebConf.h>
#include <IotWebConfUsing.h> // This loads aliases for easier class names.
#include <SPIFFS.h>
// -- Initial name of the Thing. Used e.g. as SSID of the own Access Point.
const char thingName[] = "SMLReader";

// -- Initial password to connect to the Thing, when it creates an own Access Point.
const char wifiInitialApPassword[] = "hammelhammel";

#define STRING_LEN 128
#define ID_LEN 4
#define NUMBER_LEN 32

// -- Configuration specific key. The value should be modified if config structure was changed.
#define CONFIG_VERSION "1015"

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

#define TELEGRAM_LENGTH 512
// Telegramm-Signaturen
const uint8_t SIGNATURE_START[] = {0x1b, 0x1b, 0x1b, 0x1b, 0x01, 0x01, 0x01, 0x01};
const uint8_t SIGNATURE_END[] = {0x1b, 0x1b, 0x1b, 0x1b, 0x1a};

#define BUFFER_SIZE 512 // Maximale Puffergröße für Eingangsdaten
#define TELEGRAM_SIZE 512 // Maximale Größe eines Telegramms
#define TIMEOUT_MS 30    // Timeout für Telegramme in Millisekunden

uint8_t buffer[BUFFER_SIZE];   // Eingabepuffer für serielle Daten
size_t bufferIndex = 0;        // Aktuelle Position im Eingabepuffer
bool readingExtraBytes = false; // Status: Lesen der zusätzlichen Bytes
uint8_t extraBytes[3];          // Zusätzliche Bytes nach der Endsignatur
size_t extraIndex = 0;          // Index für zusätzliche Bytes
unsigned long lastByteTime = 0; // Zeitstempel des letzten empfangenen Bytes
unsigned long timestamp_telegram;
uint8_t TELEGRAM[TELEGRAM_SIZE]; // Speicher für das vollständige Telegramm
size_t TELEGRAM_SIZE_USED = 0;   // Tatsächliche Länge des gespeicherten Telegramms




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


// -- Forward declarations.
void handleRoot();
void showTelegram();
void showMeterValue();
void showTemperature();
void showCert();
void configSaved();
void call_backend_V2();
void reset_telegram();
void store_meter_value();

DNSServer dnsServer;
WebServer server(80);

float temperature;

char telegram_offset[NUMBER_LEN];
char telegram_length[NUMBER_LEN];
char telegram_prefix[NUMBER_LEN];
char telegram_suffix[NUMBER_LEN];

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

IotWebConfParameterGroup group1 = IotWebConfParameterGroup("group1", "Telegram Param");
IotWebConfParameterGroup group2 = IotWebConfParameterGroup("group2", "Backend Config");
// IotWebConfParameterGroup groupSslCert = IotWebConfParameterGroup("groupSslCert", "SSL Cert");
IotWebConfNumberParameter telegram_offset_object = IotWebConfNumberParameter("Offset", "telegram_offset_object", telegram_offset, NUMBER_LEN, "20", "1..TELEGRAM_LENGTH", "min='1' max='TELEGRAM_LENGTH' step='1'");
IotWebConfNumberParameter telegram_length_object = IotWebConfNumberParameter("Length", "telegram_length_object", telegram_length, NUMBER_LEN, "8", "1..TELEGRAM_LENGTH", "min='1' max='TELEGRAM_LENGTH' step='1'");
IotWebConfNumberParameter telegram_prefix_object = IotWebConfNumberParameter("Prefix Begin", "telegram_prefix", telegram_prefix, NUMBER_LEN, "0", "1..TELEGRAM_LENGTH", "min='0' max='TELEGRAM_LENGTH' step='1'");
IotWebConfNumberParameter telegram_suffix_object = IotWebConfNumberParameter("Suffix Begin", "telegram_suffix", telegram_suffix, NUMBER_LEN, "100", "1..TELEGRAM_LENGTH", "min='100' max='TELEGRAM_LENGTH' step='1'");

IotWebConfTextParameter backend_endpoint_object = IotWebConfTextParameter("backend endpoint", "backend_endpoint", backend_endpoint, STRING_LEN);
IotWebConfCheckboxParameter led_blink_object = IotWebConfCheckboxParameter("LED Blink", "led_blink", led_blink, STRING_LEN, true);
IotWebConfTextParameter backend_ID_object = IotWebConfTextParameter("backend ID", "backend_ID", backend_ID, ID_LEN);
IotWebConfTextParameter backend_token_object = IotWebConfTextParameter("backend token", "backend_token", backend_token, STRING_LEN);
IotWebConfNumberParameter read_meter_intervall_object = IotWebConfNumberParameter("Read Meter Intervall", "read_meter_intervall", read_meter_intervall, NUMBER_LEN, "20", "5..100 s", "min='5' max='100' step='1'");
IotWebConfNumberParameter backend_call_minute_object = IotWebConfNumberParameter("backend Call Minute", "backend_call_minute", backend_call_minute, NUMBER_LEN, "5", "", "");

IotWebConfCheckboxParameter mystrom_PV_object = IotWebConfCheckboxParameter("MyStrom PV", "mystrom_PV", mystrom_PV, STRING_LEN, false);
IotWebConfTextParameter mystrom_PV_IP_object = IotWebConfTextParameter("MyStrom PV IP", "mystrom_PV_IP", mystrom_PV_IP, STRING_LEN);
IotWebConfCheckboxParameter temperature_object = IotWebConfCheckboxParameter("Temperatur Sensor", "temperature_checkbock", temperature_checkbock, STRING_LEN, true);
IotWebConfCheckboxParameter UseSslCert_object = IotWebConfCheckboxParameter("Use SSL Cert", "UseSslCertValue", UseSslCertValue, STRING_LEN, false);



int meter_value_i = 0;
const int data_buffer = 200;
unsigned long data[data_buffer + 1][3];

// Definition des Logbuffers
const int LOG_BUFFER_SIZE = 100;
struct LogEntry {
    unsigned long timestamp; // Zeitstempel in Millisekunden seit Start
    unsigned long uptime;    // Betriebszeit in Sekunden
    int statusCode;          // Statuscode
};
LogEntry logBuffer[LOG_BUFFER_SIZE];
int logIndex = -1; // Index des nächsten Eintrags
void resetLogBuffer() {
    for (int i = 0; i < LOG_BUFFER_SIZE; ++i) {
        logBuffer[i].timestamp = 0;
        logBuffer[i].uptime = 0;
        logBuffer[i].statusCode = 0;
    }
    logIndex = -1;
}

bool send_status_report = false;
// Funktion zur Hinzufügung eines neuen Log-Eintrags
bool firstTime = true;
String StatusCodeToString(int statusCode) {
    switch (statusCode) {
        case 1013: return "clear_data_array()";
        case 1001: return "setup()";
        case 1005: return "call_backend()";
        case 1006: return "store_meter_value()";
        case 1008: return "WiFi returned";
        case 1009: return "WiFi lost";
        case 1010: return "Taf7 meter reading trigger";
        case 1002: return "Taf6 meter reading trigger";
        case 1014: return "Taf7-900s meter reading trigger";
        case 1015: return "not enough heap to store value";
        case 1011: return "Taf14 meter reading trigger";
        case 1012: return "call backend trigger";
        case 1019: return "Sending Log";
        case 1020: return "Sending Log successful";
        case 1021: return "call_backend successful";
        case 1200: return "meter value <= 0"; 
        case 1201: return "current Meter value = previous meter value";
        case 1203: return "Suffix Must not be 0";
        case 1204: return "prefix suffix not correct";
        case 1205: return "Error Buffer Size Exceeded";
        case 3000: return "Complete Telegram received";
        case 3001: return "Telegram Pufferueberlauf";
        case 3002: return "Telegram timeout";
        case 3003: return "Telegramm zu groß für Speicher";
        case 4000: return "Connection to server failed (Cert!?)";
        case 8000: return "Spiffs not mounted";
        case 8001: return "Fehler beim Öffnen der Zertifikatsdatei!";
        case 8002: return "Zertifikat gespeichert";
        case 8003: return "Fehler beim Öffnen der Cert Datei";
        case 8004: return "Kein Zertifikat erhalten!";
        //default: return "Unknown status code";
    }
    if(statusCode < 200)
    {
      return "# values transmitted";
    }

    return "Unknown status code";
}
// Hilfsfunktion: Holt die aktuelle Zeit als time_t (Unix-Timestamp)
time_t getCurrentTime() {
    return time(nullptr);
}

unsigned long timeClient_getEpochTime() {
    return static_cast<unsigned long>(getCurrentTime());
}

int timeClient_getMinutes() {
    time_t now = getCurrentTime();
    struct tm timeinfo;
    localtime_r(&now, &timeinfo); // Konvertiere in lokale Zeitstruktur

    return timeinfo.tm_min;      // Extrahiere Minuten (0–59)
}

String timeClient_getFormattedTime() {
    time_t now = getCurrentTime();
    char timeStr[64];
    struct tm timeinfo;
    localtime_r(&now, &timeinfo); // Lokale Zeit (Zeitzone angewendet)

    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo); // Zeitformat HH:MM:SS
    return String(timeStr);        // Gibt Zeit als lesbaren String aus
}
void AddLogEntry(int statusCode) {
    // Aktualisiere den Index (Ring-Puffer-Verhalten)
    logIndex = (logIndex + 1) % LOG_BUFFER_SIZE;

    unsigned long uptimeSeconds = millis() / 60000; // Uptime in Sekunden

    // Füge neuen Eintrag in den Ring-Buffer ein
    logBuffer[logIndex].timestamp = timeClient_getEpochTime();
    logBuffer[logIndex].uptime = uptimeSeconds;
    logBuffer[logIndex].statusCode = statusCode;

       
}

// Funktion zum Debuggen des Logbuffers
void PrintLogBuffer() {
    Serial.println("LogBuffer-Inhalt:");
    for (int i = 0; i < LOG_BUFFER_SIZE; i++) {
        Serial.print("Index ");
        Serial.print(i);
        Serial.print(": Timestamp=");
        Serial.print(logBuffer[i].timestamp);
        Serial.print(", Uptime=");
        Serial.print(logBuffer[i].uptime);
        Serial.print(", StatusCode=");
        Serial.println(logBuffer[i].statusCode);
    }
}

void sortLogEntries(LogEntry* entries, size_t size) {
    // Array stabil sortieren: Nach timestamp aufsteigend
    std::stable_sort(entries, entries + size, [](const LogEntry &a, const LogEntry &b) {
        return a.timestamp < b.timestamp;
    });
}

String formatTimestamp(unsigned long timestamp) {
    // Konvertiere Unix-Timestamp in lokale Zeit
    time_t rawTime = static_cast<time_t>(timestamp);
    struct tm timeinfo;
    localtime_r(&rawTime, &timeinfo);

    char buffer[20];
    strftime(buffer, sizeof(buffer), "%D %H:%M:%S", &timeinfo);
    return String(buffer);
}
String LogBufferEntryToString(int i)
{
        if (logBuffer[i].statusCode == 0) return ""; // Leere Einträge überspringen
        String logString;
        logString += String(i) + ", ";
        logString += String(logBuffer[i].timestamp) + ", ";
        logString += formatTimestamp(logBuffer[i].timestamp) + ", ";
        logString += String(logBuffer[i].uptime) + ", ";
        logString += String(logBuffer[i].statusCode) + ": ";
        logString += StatusCodeToString(logBuffer[i].statusCode);
        
        logString += "<br>";
        return logString;
}
String LogBufferToString() {

    String logString = "";

    // Erste Schleife: Neuerer Bereich (ab logIndex rückwärts bis 0)
    for (int i = logIndex; i >= 0; i--) {
        logString += LogBufferEntryToString(i);
    }
    logString += "-----<br>";
    // Zweite Schleife: Älterer Bereich (vom Ende des Buffers rückwärts bis nach logIndex)
     if (logIndex < LOG_BUFFER_SIZE - 1) {
        for (int i = LOG_BUFFER_SIZE - 1; i > logIndex; i--) {
            logString += LogBufferEntryToString(i);
        }
     }
    
        return logString;
    }

void clear_data_array()
{
  for (int m = 0; m < data_buffer; m++)
  {
    data[m][0] = 0;
    data[m][1] = 0;
    data[m][2] = 0;
  }
  meter_value_i = 0;
  //AddLogEntry(1013);
}
void location_href_home(int delay = 0)
  {
    String call = "<meta http-equiv='refresh' content = '"+ String(delay)+";url=/'>";
    server.send(200, "text/html", call);
  }
void splitHostAndPath(const String& url, String& host, String& path) {
    // Suche nach dem ersten "/"
    int slashIndex = url.indexOf('/');

    if (slashIndex == -1) {
        // Kein "/" gefunden -> Alles ist der Host
        host = url;
        path = "/";
    } else {
        // Host ist der Teil vor dem ersten "/"
        host = url.substring(0, slashIndex);
        // Pfad ist der Teil ab dem ersten "/"
        path = url.substring(slashIndex);
    }
}
    String backend_host;
    String backend_path;
    
char* FullCert = new char[2000];
void SetSslCert() {
  server.send(200, "text/html", "<form action='/upload' method='POST'><textarea name='cert' rows='10' cols='80'>"+String(FullCert)+"</textarea><br><input type='submit'></form>");
}
void handleCertUpload() {
  if (server.hasArg("cert")) {
    String cert = server.arg("cert");
    File file = SPIFFS.open("/cert.pem", FILE_WRITE);
    if (file) {
      file.println(cert);
      file.close();
      AddLogEntry(8002);
      location_href_home();
    } else {
      AddLogEntry(8003);
      location_href_home();
      server.send(500, "text/plain", "Fehler beim Öffnen der Datei!");
    }
  } else {
    AddLogEntry(8004);
    location_href_home();

  }
}
 void loadCertToCharArray() {

  File file = SPIFFS.open("/cert.pem", FILE_READ);

  if (!file) {

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
void setup()
{
  AddLogEntry(1001);
  Serial.begin(115200);

  #if defined(ESP32)
  mySerial.begin(9600, SERIAL_8N1, 15, 16);
  #elif defined(ESP8266)
  mySerial.begin(9600);
  #endif

  

  Serial.println();
  Serial.println("Starting up...HELLAU!");

  group1.addItem(&telegram_offset_object);
  group1.addItem(&telegram_length_object);
  group1.addItem(&telegram_prefix_object);
  group1.addItem(&telegram_suffix_object);
  group2.addItem(&backend_endpoint_object);
  group2.addItem(&backend_ID_object);
  group2.addItem(&backend_token_object);
  group2.addItem(&read_meter_intervall_object);
  group2.addItem(&backend_call_minute_object);

  group2.addItem(&led_blink_object);
  group2.addItem(&mystrom_PV_object);
  group2.addItem(&mystrom_PV_IP_object);
  group2.addItem(&temperature_object);
  group2.addItem(&UseSslCert_object);
  


  iotWebConf.setStatusPin(STATUS_PIN);
  iotWebConf.setConfigPin(CONFIG_PIN);

  iotWebConf.addParameterGroup(&group1);
  iotWebConf.addParameterGroup(&group2);

  iotWebConf.setConfigSavedCallback(&configSaved);
  // iotWebConf.setFormValidator(&formValidator);
  iotWebConf.getApTimeoutParameter()->visible = true;

  // -- Initializing the configuration.
  iotWebConf.skipApStartup();
  iotWebConf.init();
  
  if (led_blink_object.isChecked())
    iotWebConf.enableBlink();
  else {
    iotWebConf.disableBlink();
    digitalWrite(LED_BUILTIN, LOW);

  }
  
  // -- Set up required URL handlers on the web server.
  server.on("/", handleRoot);
  server.on("/showTelegram", showTelegram);
  server.on("/showMeterValue", showMeterValue);
  server.on("/showTemperature", showTemperature);
  server.on("/showCert", showCert);
  server.on("/setCert", SetSslCert);
  
  server.on("/upload", []
              {
              handleCertUpload();
              loadCertToCharArray();
              });

  server.on("/config", []
            { iotWebConf.handleConfig(); });
  server.on("/restart", []
            { 
              location_href_home(5);
              ESP.restart(); });
  server.on("/resetLog", []
            { 
              location_href_home();
              resetLogBuffer();
             }); 
  server.on("/StoreMeterValue", []
            { location_href_home();
            AddLogEntry(1002);
            store_meter_value(); });               
            
  server.on("/sendStatus", []
            { 
            send_status_report = true;
            location_href_home(); });         
  server.on("/callBackend", []
            { 
            location_href_home();
            call_backend_V2();
            });   
            
  server.on("/setOffline", []
            { wifi_connected = false;
            location_href_home(); }); 
  
              
            
  server.onNotFound([]()
                    { iotWebConf.handleNotFound(); });

#if defined(ESP8266)
  // timeClient.begin(); // do we still need this?
#endif

  Serial.println("Ready.");
    ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
      else // U_SPIFFS
        type = "filesystem";

      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      Serial.println("Start updating " + type);
    })
    .onEnd([]() {
      Serial.println("\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

  clear_data_array();
    
  splitHostAndPath(String(backend_endpoint), backend_host, backend_path);
  // fullKey = String(publicKeyChunk1) + String(publicKeyChunk2) + String(publicKeyChunk3) + String(publicKeyChunk4);
  // fullKey.replace(" ", "");  // Entfernt alle Leerzeichen
  // fullKey.replace("\n", ""); // Entfernt alle Zeilenumbrüche
  // fullKey.trim();
  // compare_string = compareStrings((formatCertificateWithLineBreaks(fullKey)), rootCACertificate);
  if (!SPIFFS.begin(true)) {
    AddLogEntry(8000);
  }
  loadCertToCharArray();
}

uint8_t BUFFER[TELEGRAM_LENGTH] = {0};
bool prefix_suffix_correct()
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

int32_t get_meter_value_from_primary();
int32_t get_meter_value_from_telegram()
{
  
  // return get_meter_value_from_primary();

  int offset = atoi(telegram_offset);
  int length = atoi(telegram_length);
  int32_t meter_value = -1;
  
  if (!prefix_suffix_correct())
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




unsigned long last_serial;

int32_t get_meter_value_PV()
{
  
  if(!mystrom_PV_object.isChecked())
  {
    return 0;
  }

  Serial.println(F("get_meter_value_PV Connecting..."));

  // Connect to HTTP server
  WiFiClient client;
  client.setTimeout(1000);
  if (!client.connect(mystrom_PV_IP, 80)) {
    Serial.println(F("get_meter_value_PV Connection failed"));
    return -1;
  }

  Serial.println(F("get_meter_value_PV Connected!"));

  // Send HTTP request
  client.println(F("GET /report HTTP/1.0"));
  client.print(F("Host: "));
  client.println(mystrom_PV_IP);
  client.println(F("Connection: close"));
  if (client.println() == 0) {
    Serial.println(F("Failed to send request"));
    client.stop();
    return -2;
  }

  // Check HTTP status
  char status[32] = {0};
  client.readBytesUntil('\r', status, sizeof(status));
  // It should be "HTTP/1.0 200 OK" or "HTTP/1.1 200 OK"
  if (strcmp(status + 9, "200 OK") != 0) {
    Serial.print(F("get_meter_value_PV Unexpected response: "));
    Serial.println(status);
    client.stop();
    return -3;
  }

  // Skip HTTP headers
  char endOfHeaders[] = "\r\n\r\n";
  if (!client.find(endOfHeaders)) {
    Serial.println(F("get_meter_value_PV Invalid response"));
    client.stop();
    return -4;
  }

  // Allocate the JSON document
  JsonDocument doc;

  // Parse JSON object
  DeserializationError error = deserializeJson(doc, client);
  if (error) {
    Serial.print(F("get_meter_value_PV deserializeJson() failed: "));
    Serial.println(error.f_str());
    client.stop();
    return -5;
  }

  // Extract values

  return (doc["energy_since_boot"].as<int>());

  // Disconnect
  client.stop();
}
String meter_value;
String lastLine;
int32_t get_meter_value_from_primary()
{

  // Serial.println(F("Connecting..."));

  // Connect to HTTP server
  WiFiClient client;
  client.setTimeout(1000);
  if (!client.connect("192.168.0.2", 80))
  {
    Serial.println(F("get_meter_value_from_primary Connection failed"));
    return -1;
  }
  // Serial.println(F("Connected!"));

  // Send HTTP request
  client.println(F("GET /showMeterValue HTTP/1.0"));
  client.print(F("Host: "));
  client.println("192.168.0.2");
  client.println(F("Connection: close"));
  if (client.println() == 0)
  {
    Serial.println(F("get_meter_value_from_primary Failed to send request"));
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
void saveCompleteTelegram() {
  size_t telegramLength = bufferIndex + 3; // Telegrammlänge inkl. zusätzlicher Bytes
  if (telegramLength > TELEGRAM_SIZE) {
    // Serial.println("Fehler: Telegramm zu groß für Speicher!");
    AddLogEntry(3003);
    return;
  }

  // Telegramm in TELEGRAM-Array kopieren
  memcpy(TELEGRAM, buffer, bufferIndex); // Kopiere Hauptdaten
  memcpy(TELEGRAM + bufferIndex, extraBytes, 3); // Kopiere zusätzliche Bytes
  TELEGRAM_SIZE_USED = telegramLength;
  timestamp_telegram = timeClient_getEpochTime(); //last_serial;
  
}

// Funktion zum Zurücksetzen des Eingabepuffers
void resetBuffer() {
  bufferIndex = 0;
  readingExtraBytes = false;
  extraIndex = 0;
}

// Beispiel-Funktion zur Verarbeitung des Telegramms
void processTelegram() {
  Serial.println("Verarbeitung des Telegramms...");

  // Ausgabe der Nutzdaten (ohne Start- und Endsignatur)
  size_t dataStart = sizeof(SIGNATURE_START);
  size_t dataEnd = TELEGRAM_SIZE_USED - sizeof(SIGNATURE_END) - 3;

  Serial.println("Nutzdaten:");
  for (size_t i = dataStart; i < dataEnd; i++) {
    Serial.printf("Byte %zu: 0x%02X\n", i, TELEGRAM[i]);
  }

  // Zusätzliche Bytes ausgeben
  Serial.println("Zusätzliche Bytes:");
  for (size_t i = TELEGRAM_SIZE_USED - 3; i < TELEGRAM_SIZE_USED; i++) {
    Serial.printf("Byte %zu: 0x%02X\n", i, TELEGRAM[i]);
  }
}


void handle_telegram2() {
  // Prüfen, ob Daten verfügbar sind
  while (mySerial.available() > 0) {
    uint8_t incomingByte = mySerial.read();
    lastByteTime = millis(); // Zeitstempel aktualisieren

    // Prüfen, ob zusätzliche Bytes gelesen werden müssen
    if (readingExtraBytes) {
      extraBytes[extraIndex++] = incomingByte;

      // Wenn alle zusätzlichen Bytes gelesen wurden
      if (extraIndex == 3) {
        saveCompleteTelegram(); // Telegramm speichern
        resetBuffer();          // Eingabepuffer zurücksetzen
        // AddLogEntry(3000);
      }
      continue;
    }

    // Byte im Eingabepuffer speichern
    if (bufferIndex < BUFFER_SIZE) {
      buffer[bufferIndex++] = incomingByte;
    } else {
      // Fehler: Pufferüberlauf
      // Serial.println("Fehler: Pufferüberlauf! Eingabepuffer zurückgesetzt.");
      // AddLogEntry(3001);
      resetBuffer();
      continue;
    }

    // Prüfen, ob die Startsignatur erkannt wurde
    if (bufferIndex >= sizeof(SIGNATURE_START) &&
        memcmp(buffer, SIGNATURE_START, sizeof(SIGNATURE_START)) == 0) {
      
      // Prüfen, ob die Endsignatur erkannt wurde
      if (bufferIndex >= sizeof(SIGNATURE_START) + sizeof(SIGNATURE_END)) {
        if (memcmp(&buffer[bufferIndex - sizeof(SIGNATURE_END)], SIGNATURE_END, sizeof(SIGNATURE_END)) == 0) {
          // Endsignatur erkannt, auf zusätzliche Bytes warten
          readingExtraBytes = true;
        }
      }
    }
  }

  // Prüfen, ob ein Timeout aufgetreten ist
  if (bufferIndex > 0 && (millis() - lastByteTime > TIMEOUT_MS)) {
    // Serial.println("Fehler: Timeout! Eingabepuffer zurückgesetzt.");
    // AddLogEntry(3002);
    resetBuffer(); // Eingabepuffer zurücksetzen
  }
}



void receive_telegram()
{
  while (mySerial.available())
  {
    BUFFER[m_i] = mySerial.read();
    // Serial.print(TELEGRAM[m_i], HEX);
    // Serial.println(millis());

    if(/*m_i == atoi(telegram_suffix) + 7*/
    m_i > 8
    && BUFFER[m_i-7] == 0x1b
    && BUFFER[m_i-6] == 0x1b
    && BUFFER[m_i-5] == 0x1b
    && BUFFER[m_i-4] == 0x1b
    && BUFFER[m_i-3] == 0x1a)
    {
      // AddLogEntry(1234);
      reset_telegram();
      return;
    }

    m_i++;

    m_i_max = max(m_i_max, m_i);

    if (m_i >= TELEGRAM_LENGTH)
    {
      m_i = 0;
      Serial.println("ERROR Buffer Size exceeded");
      AddLogEntry(1205);
    }

    last_serial = millis();
  }
  // else Serial.print("nix empfangen\n");
}

void reset_telegram()
{
  
  bool transfer = false;
  if (BUFFER[0] != 0x00 && BUFFER[1] != 0x00 && BUFFER[2] != 0x00)
  {
    transfer = true;
  }

  for (int q = 0; q < TELEGRAM_LENGTH; q++)
  {
    // Serial.println(q + " " + BUFFER[q]);
    if (transfer)
    {
      TELEGRAM[q] = BUFFER[q]; // cpoy received message, so that only a complete telegram is processed
    }
    BUFFER[q] = 0;
  }
  

  m_i = 0;
  m_i_max = 0;
  timestamp_telegram = timeClient_getEpochTime(); //last_serial;
  last_serial = millis();
  
}

void handle_telegram()
{
  receive_telegram();
  if (millis() - last_serial > 30)
    reset_telegram();
}


bool call_backend_V2_successfull = true;

unsigned long last_call_backend_v2 = 0;


void send_status_report_function()
{
  Serial.println("send_status_report");
  AddLogEntry(1019);
   WiFiClientSecure client;


  if(UseSslCert_object.isChecked())
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

    // Log-Buffer stabil sortieren nach Timestamp
    std::stable_sort(logBuffer, logBuffer + LOG_BUFFER_SIZE, [](const LogEntry &a, const LogEntry &b) {
        return a.timestamp < b.timestamp;
    });

    // Binärdaten des LogBuffers in Puffer schreiben
    size_t logBufferSize = LOG_BUFFER_SIZE * sizeof(LogEntry);
    uint8_t *logDataBuffer = (uint8_t *)malloc(logBufferSize);
    if (!logDataBuffer) {
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
        send_status_report = false;
      }
    }
  }
  client.stop();
}
void call_backend_V2()
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
  
  if(UseSslCert_object.isChecked())
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
  size_t bufferSize = data_buffer * 3 * sizeof(unsigned long);
  uint8_t *buffer = (uint8_t *)malloc(bufferSize);
  if (!buffer)
  {
    Serial.println("Buffer allocation failed");
    return;
  }
  memcpy(buffer, data, bufferSize);

  // HTTP POST-Anfrage manuell erstellen
  String header = "POST ";
  header += backend_path;
  header += "?ID=";
  header += backend_ID;
  header += "&uptime=";
  header += String(millis() / 60000);
  header += "&time=";
  header += String(timeClient_getFormattedTime());
  header += "&heap=";
  header += String(ESP.getFreeHeap());
  header += "&meter_value_i=";
  header += String(meter_value_i);

  
  
  
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
        Serial.println("Data successfully sent");
        call_backend_V2_successfull = true;
        clear_data_array();
        last_call_backend_v2 = millis();
        AddLogEntry(1021);
      }
    }
  }


  if(send_status_report == true) {
    send_status_report_function();
    
  }
  client.stop();
}
unsigned long last_meter_value = 0;
int32_t previous_meter_value = 0;
void store_meter_value()
{
  //AddLogEntry(1006);
  last_meter_value = millis();
  if (ESP.getFreeHeap() < 1000)
  {
    AddLogEntry(1015);
    Serial.println("Not enough free heap to store another value");
    return;
  }

  int32_t meter_value = get_meter_value_from_telegram();
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

  meter_value_i++;
  if (meter_value_i >= data_buffer)
    meter_value_i = 0;
  Serial.println("buffer i: " + String(meter_value_i));

  data[meter_value_i][0] = timestamp_telegram; //timeClient_getEpochTime();
  data[meter_value_i][1] = meter_value;

  if (temperature_object.isChecked())
  {
    Temp_sensors.requestTemperatures();
    data[meter_value_i][2] = int(Temp_sensors.getTempCByIndex(0) * 100);
  }

  Serial.print("Free Heap: ");
  Serial.println(ESP.getFreeHeap());
}
void print_data_buffer()
{
  for (int m = 0; m < data_buffer; m++)
  {
    if (data[m][0] != 0)
    {
      Serial.print(data[m][0]);
      Serial.print(" - ");
      Serial.print(data[m][1]);
      Serial.print(" - ");
      Serial.println(data[m][2]);
    }
  }
}
unsigned long last_wifi_check;
int read_meter_intervall_int = 0;

void handle_call_backend()
{
}
void handle_store_meter_value()
{
}
void handle_check_wifi_connection()
{
}
unsigned long wifi_reconnection_time = 0;
unsigned long last_backend_call = 0;
void loop()
{
  
  iotWebConf.doLoop();
  ArduinoOTA.handle();
  
  handle_telegram2();

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
      
      if(firstTime == true) 
      {
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        
        firstTime = false;
      }
      else
      {
        send_status_report = true;
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
      //timeClient_getMinutes() % 15 == 0 
      (timeClient_getEpochTime()-3) % 900 < 60
      && millis() - last_meter_value > 60000)
  {
    AddLogEntry(1010);
    store_meter_value();

  }
  if (!wifi_connected
      && millis() - last_meter_value > 900000)
  {
    AddLogEntry(1014);
    store_meter_value();
  }
  if (wifi_connected && millis() - last_meter_value > 1000UL * max(5UL, (unsigned long)atoi(read_meter_intervall)))
  {
    AddLogEntry(1011);
    store_meter_value();
  }

  if (wifi_connected && millis() - wifi_reconnection_time > 60000)
  {
    if ((!call_backend_V2_successfull && millis() - last_call_backend_v2 > 30000)
     || (timeClient_getMinutes() % atoi(backend_call_minute) == 0 && millis() - last_call_backend_v2 > 60000))
    {
      //AddLogEntry(1012);
      call_backend_V2();
    }
  }
}
#if defined(ESP32)
String esp_reset_reason_string()
{
  switch (esp_reset_reason()) {
      case ESP_RST_UNKNOWN: return "Unknown";
      case ESP_RST_POWERON: return "Power on";
      case ESP_RST_EXT: return "External reset";
      case ESP_RST_SW: return "Software reset";
      case ESP_RST_PANIC: return "Exception/panic";
      case ESP_RST_INT_WDT: return "Interrupt watchdog";
      case ESP_RST_TASK_WDT: return "Task watchdog";
      case ESP_RST_WDT: return "Other watchdogs";
      case ESP_RST_DEEPSLEEP: return "Deep sleep";
      case ESP_RST_BROWNOUT: return "Brownout";
      case ESP_RST_SDIO: return "SDIO";
      default: return "Unknown";
  }
}
#endif
/**
 * Handle web requests to "/" path.
 */


void handleRoot()
{
  // -- Let IotWebConf test and handle captive portal requests.
  if (iotWebConf.handleCaptivePortal())
  {
    // -- Captive portal request were already served.
    return;
  }
  Temp_sensors.requestTemperatures();
    
  String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
  s += "<title>IotWebConf 03 Custom Parameters</title></head><body>Hello world!";
  s += "<ul>";
  s += "<li>Meter Value Offset: ";
  s += atoi(telegram_offset);
  s += "<li>Meter Value length: ";
  s += atoi(telegram_length);
  s += "<li>Prefix Begin (usualy 0): ";
  s += atoi(telegram_prefix);
  s += "<li>Suffix Begin: ";
  s += atoi(telegram_suffix);

  s += "<li>Backend Endpoint: ";
  s += backend_endpoint;
  
  s += "<li>Backend Host: ";
  s += backend_host;
  s += "<li>Backend Path: ";
  s += backend_path;
  s += "<li>LED blink: ";
  s += led_blink;
  s += "<li>Backend ID: ";
  s += backend_ID;
  s += "<li>Backend Token: ";
  s += backend_token;
  s += "<li>Read Meter Intervall: ";
  s += atoi(read_meter_intervall);
  s += "<li>Backend call Minute: ";
  s += atoi(backend_call_minute);
  s += "<li>MyStrom PV : ";
  s += mystrom_PV;
  s += "<li>MyStrom PV IP: ";
  s += mystrom_PV_IP;
  s += "<li>temperatur: ";
  s += String(Temp_sensors.getTempCByIndex(0));
  s += "<li>Ring Buffer i: ";
  s += String(meter_value_i);
  s += "<li>Uptime (min): ";
  s += String(millis() / 60000);
  s += "<li>Last Call ago (min): ";
  s += String((millis() - last_call_backend_v2) / 60000);

   if (UseSslCert_object.isChecked())
    s += "<li>Use SSL Cert: true";
  else {
    s += "<li>Use SSL Cert: false";


  }

  #if defined(ESP32)

  s += "<li>Reset Reason: ";
  s += esp_reset_reason_string();
  #elif defined(ESP8266)

  s += "<li>Reset Reason: ";
  s += String(/*esp_reset_reason()*/ESP.getResetReason());
  s += " / ";
  s += String(/*esp_reset_reason()*/ESP.getResetInfo());
  #endif

  s += "<li>Systemzeit: ";
  s += String(timeClient_getFormattedTime());
  s += " / ";
  s += String(timeClient_getEpochTime());
  s += "<li><b>Detected Meter Value</b>: " + String(get_meter_value_from_telegram());
  s += "<li><b>Detected Meter Value PV</b>: " + String(get_meter_value_PV());
  s += "</ul>";

  s += "</ul><br>";
  s += "Free Heap ";
  s += String(ESP.getFreeHeap());
  

  s += "<br>Go to <a href='config'>configure page</a> to change values.";
  s += "<br><a href='showTelegram'>Show Telegram</a>";
  s += "<br><a href='showCert'>Show Cert</a>";
  s += "<br><a href='setCert'>Set Cert</a>";
  s += "<br><a href='StoreMeterValue'>Store Meter Value</a>";
  s += "<br><a href='sendStatus'>Send Status Report with next backend call</a>";
  s += "<br><a href='callBackend'>Call Backend</a>";
  s += "<br><a href='resetLog'>Reset Log</a>";
  s += "<br><a href='restart'>Restart</a>";
  s += "<br><br>Log Buffer (index " + String(logIndex) + ")<br>";
  s += LogBufferToString();


  s += "<br></body></html>\n";

  server.send(200, "text/html", s);
}
void showCert()
{
  server.send(200, "text/html", String(FullCert));
}

void showTemperature()
{
  Temp_sensors.requestTemperatures();
  server.send(200, "text/html", String(Temp_sensors.getTempCByIndex(0)));
}
void showTelegram()
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
  if (!prefix_suffix_correct())
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


void showMeterValue()
{
  // -- Let IotWebConf test and handle captive portal requests.
  if (iotWebConf.handleCaptivePortal())
  {
    // -- Captive portal request were already served.
    return;
  }
  
  server.send(200, "text/html", String(get_meter_value_from_telegram()));
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
  splitHostAndPath(String(backend_endpoint), backend_host, backend_path);   

}
