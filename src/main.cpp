#include <IotWebConf.h>
#include <IotWebConfUsing.h> // This loads aliases for easier class names.

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



// WiFiUDP ntpUDP;
// NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 36000000);

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

// WiFiClient client;
// WiFiClientSecure clientSecure;

const char* rootCACertificate = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)EOF";


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
int logIndex = 0; // Index des nächsten Eintrags
void resetLogBuffer() {
    for (int i = 0; i < LOG_BUFFER_SIZE; ++i) {
        logBuffer[i].timestamp = 0;
        logBuffer[i].uptime = 0;
        logBuffer[i].statusCode = 0;
    }
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
        case 1010: return ".15min meter reading trigger";
        case 1014: return "900s meter reading trigger";
        case 1015: return "not enough heap to store value";
        case 1011: return "individual meter reading trigger";
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
    
    unsigned long uptimeSeconds = millis() / 60000; // Uptime in Sekunden

    // Füge neuen Eintrag in den Ring-Buffer ein
    logBuffer[logIndex].timestamp = timeClient_getEpochTime();
    logBuffer[logIndex].uptime = uptimeSeconds;
    logBuffer[logIndex].statusCode = statusCode;

    // Aktualisiere den Index (Ring-Puffer-Verhalten)
    logIndex = (logIndex + 1) % LOG_BUFFER_SIZE;
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

//Funktion zur Darstellung des Logbuffers als String
// String LogBufferToString() {
//     String logString = "";
//     for (int i = 0; i < LOG_BUFFER_SIZE; i++) {
//       if(logBuffer[i].statusCode == 0) continue;
//         //logString += "  {\"Index\": " + String(i) + ", ";
//         logString += String(logBuffer[i].timestamp) + ", ";
//         logString += String(logBuffer[i].uptime) + ", ";
//         logString += String(logBuffer[i].statusCode) + ": ";
//         logString += StatusCodeToString(logBuffer[i].statusCode);
        
//         if (i < LOG_BUFFER_SIZE - 1) {
//             logString += "<br>";
//         } else {
//             logString += "<br>";
//         }
//     }
//     logString += "<br>";
//     return logString;
// }
void sortLogEntries(LogEntry* entries, size_t size) {
    // Array stabil sortieren: Nach timestamp aufsteigend
    std::stable_sort(entries, entries + size, [](const LogEntry &a, const LogEntry &b) {
        return a.timestamp < b.timestamp;
    });
}
String LogBufferToString() {
    // Temporäres Array für die sortierten Einträge
    LogEntry sortedEntries[LOG_BUFFER_SIZE];
    
    // Ringbuffer in das temporäre Array kopieren
    for (int i = 0; i < LOG_BUFFER_SIZE; i++) {
        sortedEntries[i] = logBuffer[i];
    }
    
    sortLogEntries(sortedEntries, LOG_BUFFER_SIZE);
    // // Array sortieren: Nach timestamp aufsteigend
    // std::sort(sortedEntries, sortedEntries + LOG_BUFFER_SIZE, [](const LogEntry &a, const LogEntry &b) {
    //     return a.timestamp < b.timestamp;
    // });

    // String erstellen
    String logString = "";
    for (int i = 0; i < LOG_BUFFER_SIZE; i++) {
        if (sortedEntries[i].statusCode == 0) continue; // Leere Einträge überspringen

        logString += String(sortedEntries[i].timestamp) + ", ";
        logString += String(sortedEntries[i].uptime) + ", ";
        logString += String(sortedEntries[i].statusCode) + ": ";
        logString += StatusCodeToString(sortedEntries[i].statusCode);
        
        logString += "<br>";
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
void location_href_home()
  {
    server.send(200, "text/html", "<meta http-equiv='refresh' content = '0;url=/'>");
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
  server.on("/config", []
            { iotWebConf.handleConfig(); });
  server.on("/restart", []
            { ESP.restart(); });
  server.on("/resetLog", []
            { 
              location_href_home();
              resetLogBuffer();
             }); 
  server.on("/StoreMeterValue", []
            { location_href_home();
            store_meter_value(); });               
            
  server.on("/sendStatus", []
            { send_status_report = true;
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
  timeClient.begin(); // do we still need this?
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

// Still needed?  
  // client = WiFiClient();
  // clientSecure = WiFiClientSecure();
  //clientSecure.setInsecure();
  
  clear_data_array();

}
// uint8_t TELEGRAM[TELEGRAM_LENGTH] = {0}; // 0x1B, 0x1B, 0x1B, 0x1B, 0x1, 0x1, 0x1, 0x1, 0x76, 0x2, 0x1, 0x62, 0x0, 0x62, 0x0, 0x72, 0x65, 0x0, 0x0, 0x1, 0x1, 0x76, 0x1, 0x1, 0x5, 0x4D, 0x58, 0x8, 0x0, 0xB, 0xA, 0x1, 0x5A, 0x50, 0x41, 0x0, 0x1, 0x32, 0xF1, 0x32, 0x72, 0x62, 0x1, 0x65, 0x0, 0x8, 0x58, 0x4E, 0x1, 0x63, 0xB3, 0x5F, 0x0, 0x76, 0x2, 0x2, 0x62, 0x0, 0x62, 0x0, 0x72, 0x65, 0x0, 0x0, 0x7, 0x1, 0x77, 0x1, 0xB, 0xA, 0x1, 0x5A, 0x50, 0x41, 0x0, 0x1, 0x32, 0xF1, 0x32, 0x7, 0x1, 0x0, 0x62, 0xA, 0xFF, 0xFF, 0x72, 0x62, 0x1, 0x65, 0x0, 0x8, 0x58, 0x4D, 0x7E, 0x77, 0x7, 0x1, 0x0, 0x60, 0x32, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x4, 0x5A, 0x50, 0x41, 0x1, 0x77, 0x7, 0x1, 0x0, 0x60, 0x1, 0x0, 0xFF, 0x1, 0x1, 0x1, 0x1, 0xB, 0xA, 0x1, 0x5A, 0x50, 0x41, 0x0, 0x1, 0x32, 0xF1, 0x32, 0x1, 0x77, 0x7, 0x1, 0x0, 0x1, 0x8, 0x0, 0xFF, 0x65, 0x0, 0x8, 0x1, 0x4, 0x1, 0x62, 0x1E, 0x52, 0xFF, 0x69, 0x0, 0x0, 0x0, 0x0, 0x0, 0x2, 0x9A, 0x8B, 0x1, 0x77, 0x7, 0x1, 0x0, 0x2, 0x8, 0x0, 0xFF, 0x1, 0x1, 0x62, 0x1E, 0x52, 0xFF, 0x69, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x37, 0x9, 0x1, 0x77, 0x7, 0x1, 0x0, 0x10, 0x7, 0x0, 0xFF, 0x1, 0x1, 0x62, 0x1B, 0x52, 0x0, 0x55, 0x0, 0x0, 0x0, 0x3E, 0x1, 0x77, 0x7, 0x1, 0x0, 0x20, 0x7, 0x0, 0xFF, 0x1, 0x1, 0x62, 0x23, 0x52, 0xFF, 0x69, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x3, 0x1, 0x77, 0x7, 0x1, 0x0, 0x34, 0x7, 0x0, 0xFF, 0x1, 0x1, 0x62, 0x23, 0x52, 0xFF, 0x69, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x9, 0x26, 0x1, 0x77, 0x7, 0x1, 0x0, 0x48, 0x7, 0x0, 0xFF, 0x1, 0x1, 0x62, 0x23, 0x52, 0xFF, 0x69, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x3, 0x1, 0x77, 0x7, 0x1, 0x0, 0x1F, 0x7, 0x0, 0xFF, 0x1, 0x1, 0x62, 0x21, 0x52, 0xFE, 0x69, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1, 0x77, 0x7, 0x1, 0x0, 0x33, 0x7, 0x0, 0xFF, 0x1, 0x1, 0x62, 0x21, 0x52, 0xFE, 0x69, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x34, 0x1, 0x77, 0x7, 0x1, 0x0, 0x47, 0x7, 0x0, 0xFF, 0x1, 0x1, 0x62, 0x21, 0x52, 0xFE, 0x69, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1, 0x77, 0x7, 0x1, 0x0, 0xE, 0x7, 0x0, 0xFF, 0x1, 0x1, 0x62, 0x2C, 0x52, 0xFF, 0x69, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1, 0xF3, 0x1, 0x77, 0x7, 0x1, 0x0, 0x0, 0x2, 0x0, 0x0, 0x1, 0x1, 0x1, 0x1, 0x3, 0x30, 0x34, 0x1, 0x77, 0x7, 0x1, 0x0, 0x60, 0x5A, 0x2, 0x1}; //, 0x1, 0x1, 0x1, 0x1, 0x5, 0x71, 0x7B, 0x4C, 0x78, 0x1, 0x1, 0x1, 0x63, 0x9, 0x11, 0x0, 0x76, 0x2, 0x3, 0x62, 0x0, 0x62, 0x0, 0x72, 0x65, 0x0, 0x0, 0x2, 0x1, 0x71, 0x1, 0x63, 0x28, 0x94, 0x0, 0x0, 0x1B, 0x1B, 0x1B, 0x1B, 0x1A, 0x1, 0xA2, 0x46};
// size_t TELEGRAM_SIZE_USED = 0;
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
    // Serial.print(TELEGRAM[offset + i]);
    // Serial.print("\t");
    // Serial.println((shift));

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
  // 192.168.188.111
  // mystrom-switch-b3e3c0

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

  // Serial.println("Meter Value from Primary: ");
  // Serial.println(meter_value);
  // Serial.println("Meter Value from Primary END ");
  // Finde die Position des letzten Zeilenumbruchs
  int lastNewlineIndex = meter_value.lastIndexOf('\n');

  // Extrahiere den Teil des Strings nach dem letzten Zeilenumbruch
  lastLine = meter_value.substring(lastNewlineIndex + 1);

  // Entferne mögliche Leerzeichen (falls vorhanden)
  lastLine.trim();

  // Konvertiere die letzte Zeile in eine Zahl
  int32_t meter_value_i32 = lastLine.toInt();
  // Disconnect
  // Serial.print("get_meter_value_from_primary Extrahiert: ");
  // Serial.println(meter_value_i32);
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
  // Telegramm ausgeben
  // Serial.println("Telegramm erfolgreich gespeichert:");
  // for (size_t i = 0; i < TELEGRAM_SIZE_USED; i++) {
  //   Serial.printf("%02X ", TELEGRAM[i]);
  // }
  // Serial.println();

  // Hier kann die Verarbeitung des Telegramms hinzugefügt werden
  //processTelegram();
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
  
  // Serial.println(m_i);
  // Serial.println(get_meter_value_from_telegram(atoi(telegram_offset),atoi(telegram_length)));
  // Serial.println(" reset buffer");
  bool transfer = false;
  if (BUFFER[0] != 0x00 && BUFFER[1] != 0x00 && BUFFER[2] != 0x00)
  {
    // Serial.print(millis());
    // Serial.println(" Transfering Buffer");

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
  if (transfer)
  {
    // Serial.print("Meter Value: ");
    // Serial.println(get_meter_value_from_telegram());
  }

  m_i = 0;
  m_i_max = 0;
  timestamp_telegram = timeClient_getEpochTime(); //last_serial;
  last_serial = millis();
  // Serial.println("meter " + get_meter_value_from_telegram());
}

void handle_telegram()
{
  receive_telegram();
  if (millis() - last_serial > 30)
    reset_telegram();
}
// unsigned long last_call = 0;

bool call_backend_V2_successfull = true;

unsigned long last_call_backend_v2 = 0;


void send_status_report_function()
{
  Serial.println("send_status_report");
  AddLogEntry(1019);
   WiFiClientSecure client;
  // client.setInsecure(); // Zertifikatsprüfung deaktivieren (für Testzwecke)
  client.setCACert(rootCACertificate);
  if (!client.connect("ip87-106-235-113.pbiaas.com", 443))
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
    String logHeader = "POST /hz/v3/log.php HTTP/1.1\r\n";
    logHeader += "Host: ip87-106-235-113.pbiaas.com\r\n";
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

  if (meter_value_i == 0)
  {
    last_call_backend_v2 = millis();
    Serial.println("Zero Values to transmit");
    call_backend_V2_successfull = true;
    return;
  }

  call_backend_V2_successfull = false;

  // Verbindung zum Server herstellen
  WiFiClientSecure client;
  // client.setInsecure(); // Zertifikatsprüfung deaktivieren (für Testzwecke)
  client.setCACert(rootCACertificate);
  if (!client.connect("ip87-106-235-113.pbiaas.com", 443))
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
  String header = "POST /hz/v3/?ID=";
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
  // header += "Host: DOMAIN.URL from IotWebConf\r\n";
  header += "Host: ip87-106-235-113.pbiaas.com";
  //header += backend_endpoint;
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
void loop()
{
  // -- doLoop should be called as frequently as possible.
  iotWebConf.doLoop();
  ArduinoOTA.handle();
  // timeClient.update();
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
      // configTime(0, 0, "pool.ntp.org", "time.nist.gov");
      if(firstTime == true) 
      {
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        // while (time(nullptr) < 100000) { // Warten, bis Zeit synchronisiert ist
        //   delay(100);
        // }
        firstTime = false;
      }
      else
      {
        send_status_report = true;
        //call_backend_V2();
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
    if (!call_backend_V2_successfull || (timeClient_getMinutes() % atoi(backend_call_minute) == 0 && millis() - last_call_backend_v2 > 60000))
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

  s += "<li>Log Buffer:<br>";
  s += LogBufferToString();

  s += "</ul><br>";
  s += "Free Heap ";
  s += String(ESP.getFreeHeap());

  s += "<br>Go to <a href='config'>configure page</a> to change values.";
  s += "<br><a href='showTelegram'>Show Telegram</a>";
  s += "<br><a href='StoreMeterValue'>Store Meter Value</a>";
  s += "<br><a href='sendStatus'>Send Status Report with next backend call</a>";
  s += "<br><a href='callBackend'>Call Backend</a>";
  s += "<br><a href='resetLog'>Reset Log</a>";
  s += "<br><a href='restart'>Restart</a>";


  s += "<br><br>";

  s += "<br></body></html>\n";

  server.send(200, "text/html", s);
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
    
}
