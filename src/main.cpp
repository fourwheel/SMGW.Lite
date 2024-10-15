/**
 * IotWebConf03CustomParameters.ino -- IotWebConf is an ESP8266/ESP32
 *   non blocking WiFi/AP web configuration library for Arduino.
 *   https://github.com/prampec/IotWebConf 
 *
 * Copyright (C) 2020 Balazs Kelemen <prampec+arduino@gmail.com>
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

/**
 * Example: Custom parameters
 * Description:
 *   In this example it is shown how to attach your custom parameters
 *   to the config portal. Your parameters will be maintained by 
 *   IotWebConf. This means, they will be loaded from/saved to EEPROM,
 *   and will appear in the config portal.
 *   Note the configSaved and formValidator callbacks!
 *   (See previous examples for more details!)
 * 
 * Hardware setup for this example:
 *   - An LED is attached to LED_BUILTIN pin with setup On=LOW.
 *   - [Optional] A push button is attached to pin D2, the other leg of the
 *     button should be attached to GND.
 */

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
#define CONFIG_PIN D2

// -- Status indicator pin.
//      First it will light up (kept LOW), on Wifi connection it will blink,
//      when connected to the Wifi it will turn off (kept HIGH).
#define STATUS_PIN LED_BUILTIN

int m_i = 0;
int m_i_max = 0;

#define TELEGRAM_LENGTH 700


#include <SoftwareSerial.h>


#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include "NTPClient.h"

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 36000000);


// First we include the libraries
#include <OneWire.h> 
#include <DallasTemperature.h>
#define ONE_WIRE_BUS 4 
OneWire oneWire(ONE_WIRE_BUS); 
DallasTemperature Temp_sensors(&oneWire);

SoftwareSerial mySerial(D5, D6); // RX, TX

// -- Method declarations.
void handleRoot();
void showTelegram();
void showTemperature();
// -- Callback methods.
void configSaved();
bool formValidator(iotwebconf::WebRequestWrapper* webRequestWrapper);

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

WiFiClient client;
WiFiClientSecure clientSecure;


//DynamicJsonDocument docArray(2048);
JsonDocument docArray;
JsonArray dataArray = docArray.to<JsonArray>();

void setup() 
{
  Serial.begin(115200);
  mySerial.begin(9600);
  
  Serial.println();
  Serial.println("Starting up...");

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
  iotWebConf.setFormValidator(&formValidator);
  iotWebConf.getApTimeoutParameter()->visible = true;

  // -- Initializing the configuration.
  iotWebConf.init();
  if(led_blink_object.isChecked()) iotWebConf.enableBlink();
  else iotWebConf.disableBlink();

  // -- Set up required URL handlers on the web server.
  server.on("/", handleRoot);
  server.on("/showTelegram", showTelegram);
  server.on("/showTemperature", showTemperature);
  server.on("/config", []{ iotWebConf.handleConfig(); });
  server.on("/restart", []{ ESP.restart(); });
  server.onNotFound([](){ iotWebConf.handleNotFound(); });

  timeClient.begin();

  Serial.println("Ready.");

    ArduinoOTA.onStart([]() {
    Serial.println("Start");
  });
  // ArduinoOTA.onEnd([]() {
  //   Serial.println("\nEnd");
  // });
  // ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
  //   Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  // });
  // ArduinoOTA.onError([](ota_error_t error) {
  //   Serial.printf("Error[%u]: ", error);
  //   if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
  //   else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
  //   else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
  //   else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
  //   else if (error == OTA_END_ERROR) Serial.println("End Failed");
  // });
  ArduinoOTA.begin();
  client = WiFiClient();
  //client.setInsecure();

  clientSecure = WiFiClientSecure();
  clientSecure.setInsecure();

}
uint8_t TELEGRAM[TELEGRAM_LENGTH] = {0}; //0x1B, 0x1B, 0x1B, 0x1B, 0x1, 0x1, 0x1, 0x1, 0x76, 0x2, 0x1, 0x62, 0x0, 0x62, 0x0, 0x72, 0x65, 0x0, 0x0, 0x1, 0x1, 0x76, 0x1, 0x1, 0x5, 0x4D, 0x58, 0x8, 0x0, 0xB, 0xA, 0x1, 0x5A, 0x50, 0x41, 0x0, 0x1, 0x32, 0xF1, 0x32, 0x72, 0x62, 0x1, 0x65, 0x0, 0x8, 0x58, 0x4E, 0x1, 0x63, 0xB3, 0x5F, 0x0, 0x76, 0x2, 0x2, 0x62, 0x0, 0x62, 0x0, 0x72, 0x65, 0x0, 0x0, 0x7, 0x1, 0x77, 0x1, 0xB, 0xA, 0x1, 0x5A, 0x50, 0x41, 0x0, 0x1, 0x32, 0xF1, 0x32, 0x7, 0x1, 0x0, 0x62, 0xA, 0xFF, 0xFF, 0x72, 0x62, 0x1, 0x65, 0x0, 0x8, 0x58, 0x4D, 0x7E, 0x77, 0x7, 0x1, 0x0, 0x60, 0x32, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x4, 0x5A, 0x50, 0x41, 0x1, 0x77, 0x7, 0x1, 0x0, 0x60, 0x1, 0x0, 0xFF, 0x1, 0x1, 0x1, 0x1, 0xB, 0xA, 0x1, 0x5A, 0x50, 0x41, 0x0, 0x1, 0x32, 0xF1, 0x32, 0x1, 0x77, 0x7, 0x1, 0x0, 0x1, 0x8, 0x0, 0xFF, 0x65, 0x0, 0x8, 0x1, 0x4, 0x1, 0x62, 0x1E, 0x52, 0xFF, 0x69, 0x0, 0x0, 0x0, 0x0, 0x0, 0x2, 0x9A, 0x8B, 0x1, 0x77, 0x7, 0x1, 0x0, 0x2, 0x8, 0x0, 0xFF, 0x1, 0x1, 0x62, 0x1E, 0x52, 0xFF, 0x69, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x37, 0x9, 0x1, 0x77, 0x7, 0x1, 0x0, 0x10, 0x7, 0x0, 0xFF, 0x1, 0x1, 0x62, 0x1B, 0x52, 0x0, 0x55, 0x0, 0x0, 0x0, 0x3E, 0x1, 0x77, 0x7, 0x1, 0x0, 0x20, 0x7, 0x0, 0xFF, 0x1, 0x1, 0x62, 0x23, 0x52, 0xFF, 0x69, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x3, 0x1, 0x77, 0x7, 0x1, 0x0, 0x34, 0x7, 0x0, 0xFF, 0x1, 0x1, 0x62, 0x23, 0x52, 0xFF, 0x69, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x9, 0x26, 0x1, 0x77, 0x7, 0x1, 0x0, 0x48, 0x7, 0x0, 0xFF, 0x1, 0x1, 0x62, 0x23, 0x52, 0xFF, 0x69, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x3, 0x1, 0x77, 0x7, 0x1, 0x0, 0x1F, 0x7, 0x0, 0xFF, 0x1, 0x1, 0x62, 0x21, 0x52, 0xFE, 0x69, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1, 0x77, 0x7, 0x1, 0x0, 0x33, 0x7, 0x0, 0xFF, 0x1, 0x1, 0x62, 0x21, 0x52, 0xFE, 0x69, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x34, 0x1, 0x77, 0x7, 0x1, 0x0, 0x47, 0x7, 0x0, 0xFF, 0x1, 0x1, 0x62, 0x21, 0x52, 0xFE, 0x69, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1, 0x77, 0x7, 0x1, 0x0, 0xE, 0x7, 0x0, 0xFF, 0x1, 0x1, 0x62, 0x2C, 0x52, 0xFF, 0x69, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1, 0xF3, 0x1, 0x77, 0x7, 0x1, 0x0, 0x0, 0x2, 0x0, 0x0, 0x1, 0x1, 0x1, 0x1, 0x3, 0x30, 0x34, 0x1, 0x77, 0x7, 0x1, 0x0, 0x60, 0x5A, 0x2, 0x1}; //, 0x1, 0x1, 0x1, 0x1, 0x5, 0x71, 0x7B, 0x4C, 0x78, 0x1, 0x1, 0x1, 0x63, 0x9, 0x11, 0x0, 0x76, 0x2, 0x3, 0x62, 0x0, 0x62, 0x0, 0x72, 0x65, 0x0, 0x0, 0x2, 0x1, 0x71, 0x1, 0x63, 0x28, 0x94, 0x0, 0x0, 0x1B, 0x1B, 0x1B, 0x1B, 0x1A, 0x1, 0xA2, 0x46};
uint8_t BUFFER[TELEGRAM_LENGTH] = {0};
bool prefix_suffix_correct(){
  int prefix = atoi(telegram_prefix);
  int suffix = atoi(telegram_suffix);
  

  if(suffix == 0)
  {
    Serial.println("Suffix Must not be 0");
    return false;
  }

  if(TELEGRAM[suffix] == 0x1B
  && TELEGRAM[suffix+1] == 0x1B
  && TELEGRAM[suffix+2] == 0x1B
  && TELEGRAM[suffix+3] == 0x1B
  && TELEGRAM[prefix] == 0x1B
  && TELEGRAM[prefix+1] == 0x1B
  && TELEGRAM[prefix+2] == 0x1B
  && TELEGRAM[prefix+3] == 0x1B) return true;
  else return false;
}
// bool prefix_suffix_correct2(){
//   int prefix = atoi(telegram_prefix);
//   int suffix = atoi(telegram_suffix);
  

//   if(suffix == 0)
//   {
//     Serial.println("Suffix Must not be 0");
//     return false;
//   }

//   if(TELEGRAM[0] == 0x1B
//   && TELEGRAM[1] == 0x1B
//   && TELEGRAM[2] == 0x1B
//   && TELEGRAM[3] == 0x1B
//   && TELEGRAM[m_i_max-7] == 0x1B
//   && TELEGRAM[m_i_max-6] == 0x1B
//   && TELEGRAM[m_i_max-5] == 0x1B
//   && TELEGRAM[m_i_max-4] == 0x1B
//   && TELEGRAM[m_i_max-3] == 0x1A) return true;
//   else return false;
// }
int32_t get_meter_value_from_primary();
int32_t get_meter_value_from_telegram()
{
  return get_meter_value_from_primary();




  int offset = atoi(telegram_offset);
  int length = atoi(telegram_length);
  int32_t meter_value = -1;
  // Serial.print("offset ");
  // Serial.print(offset);
  // Serial.print("\nlength ");
  // Serial.println(length);

  if(!prefix_suffix_correct()) return -2;

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

int last_serial;

int32_t get_meter_value_PV() {
  if(!mystrom_PV_object.isChecked())
  {
    return 0;
  }
  
  Serial.println(F("get_meter_value_PV Connecting..."));

  // Connect to HTTP server

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
int32_t get_meter_value_from_primary() {
 
  // Serial.println(F("Connecting..."));

  // Connect to HTTP server

  client.setTimeout(1000);
  if (!client.connect("192.168.0.2", 80)) {
    Serial.println(F("get_meter_value_from_primary Connection failed"));
    return -1;
  }
  // Serial.println(F("Connected!"));

  // Send HTTP request
  client.println(F("GET /showMeterValue HTTP/1.0"));
  client.print(F("Host: "));
  client.println("192.168.0.2");
  client.println(F("Connection: close"));
  if (client.println() == 0) {
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


void receive_telegram(){
  while(mySerial.available())
  {
    BUFFER[m_i] = mySerial.read();
    //Serial.print(TELEGRAM[m_i], HEX);
    //Serial.println(millis());
    m_i++;
  
    m_i_max = max(m_i_max, m_i);

    if(m_i >= TELEGRAM_LENGTH) {
      m_i = 0;
      Serial.println("ERROR Buffer Size exceeded");
    }

    last_serial = millis();
    
    
  }
  //else Serial.print("nix empfangen\n");
}
int timestamp_telegram;
void reset_telegram()
{
  //Serial.println(m_i);
  //Serial.println(get_meter_value_from_telegram(atoi(telegram_offset),atoi(telegram_length)));
  //Serial.println(" reset buffer");
  bool transfer = false;
  if(BUFFER[0] != 0x00 && BUFFER[1] != 0x00 && BUFFER[2] != 0x00) {
    Serial.print(millis());
    Serial.println(" Transfering Buffer");
    
     transfer = true; 
     }
  
    for(int q = 0; q < TELEGRAM_LENGTH; q++)
    {
      //Serial.println(q + " " + BUFFER[q]);
      if(transfer) TELEGRAM[q] = BUFFER[q];// cpoy received message, so that only a complete telegram is processed
      BUFFER[q] = 0;
    }
    if(transfer) Serial.println(get_meter_value_from_telegram());

  
  m_i = 0;
  m_i_max = 0;
  timestamp_telegram = last_serial;
  last_serial = millis();
  // Serial.println("meter " + get_meter_value_from_telegram());
}
void handle_telegram(){
  receive_telegram();
  if(millis() - last_serial > 500) reset_telegram();
}
int last_call = 0;
void call_backend(){
  return;
  Serial.println("call backend V1");

  if(temperature_object.isChecked()) 
    {
    Temp_sensors.requestTemperatures();
    temperature = Temp_sensors.getTempCByIndex(0);
    } 
  Serial.println("delay " + millis() - timestamp_telegram);
  Serial.println("meter " + get_meter_value_from_telegram());
  
  
  Serial.println("pv " + get_meter_value_PV());

  JsonDocument doc;

  doc["meter_value"] = get_meter_value_from_telegram();
  doc["meter_value_PV"] = get_meter_value_PV();
  doc["ID"] = backend_ID;
  doc["token"] = backend_token;
  doc["temperature"] = temperature;
  String json;
  serializeJson(doc, json);
Serial.println("json " + json);
  if (doc["meter_value"] != -2) 
  { 
    Serial.println("Connected to Backend @ " + String(backend_endpoint));
    
    HTTPClient http;
    http.begin(clientSecure, backend_endpoint);
    http.POST(json);

    // Read response
    Serial.println("reading Response:");
    Serial.print(http.getString());

    // Disconnect
    http.end();
  } 
 // else { Serial.println("connection to sunzilla.de failed"); }
}

bool call_backend_V2_successfull;

int last_call_backend_v2 = 0;
void call_backend_V2(){


  Serial.println("call backend V2");
  if (docArray.size() > 0) 
  { 
    call_backend_V2_successfull = false;
    Serial.println("call backend V2 Connect to Backend @ " + String(backend_endpoint));
      Serial.print("Free Heap1: ");
  Serial.println(ESP.getFreeHeap());
    WiFiClientSecure client;
      Serial.print("Free Heap2: ");
  Serial.println(ESP.getFreeHeap());
    client.setInsecure();
      Serial.print("Free Heap3: ");
  Serial.println(ESP.getFreeHeap());
    if (!client.connect("zwergenkoenig.com", 443)) {
        Serial.print("Free Heap5: ");
  Serial.println(ESP.getFreeHeap());
      Serial.println("call backend V2 Connection failed WiFiClientSecure");
      return;
    }
  Serial.print("Free Heap6: ");
  Serial.println(ESP.getFreeHeap());
    JsonDocument doc;
Serial.print("Free Heap8: ");
  Serial.println(ESP.getFreeHeap());
    // create an object
    JsonObject object = doc.to<JsonObject>();
Serial.print("Free Heap9: ");
  Serial.println(ESP.getFreeHeap());
    object["ID"] = "Z17";
    object["values"] = docArray;

Serial.print("Free Heap10: ");
  Serial.println(ESP.getFreeHeap());
    String json;
    Serial.print("Free Heap11: ");
  Serial.println(ESP.getFreeHeap());
    serializeJson(doc, json);

    Serial.print("Free Heap12: ");
  Serial.println(ESP.getFreeHeap());
    // Serial.println("json " + json);

    // Prepare the HTTP request
    client.println("POST /hz/v2/ HTTP/1.1");
    client.println("Host: zwergenkoenig.com");
    client.println("Content-Type: application/json");
    client.println("Connection: close");
    client.print("Content-Length: ");
    Serial.print("Free Heap13: ");
  Serial.println(ESP.getFreeHeap());
    client.println(json.length());
    client.println();
    client.println(json);
    Serial.print("Free Heap14: ");
  Serial.println(ESP.getFreeHeap());

    // Read the response
    while (client.connected()) {
      String line = client.readStringUntil('\n');
      Serial.print("Free Heap15: ");
  Serial.println(ESP.getFreeHeap());
      // The first line of the response contains the HTTP status code, e.g., "HTTP/1.1 200 OK"
      if (line.startsWith("HTTP/")) {
        int statusCode = line.substring(9, 12).toInt();  // Extract the 3-digit status code
        Serial.print("call backend V2 HTTP Status Code: ");
        Serial.println(statusCode);
        Serial.print("Free Heap16: ");
  Serial.println(ESP.getFreeHeap());
        // Check the status code
        if (statusCode == 200) {
          Serial.println("call backend V2 Success: HTTP 200 OK");
          call_backend_V2_successfull = true;
          last_call_backend_v2 = millis();
        } else {
          Serial.print("call backend V2 HTTP Error: ");
          Serial.println(statusCode);
        }
        break;
      }
    }

    //String response = client.readString();
    //Serial.println("call backend V2 Response: " + response);
    Serial.print("Free Heap17: ");
  Serial.println(ESP.getFreeHeap());
    client.stop();
    Serial.print("Free Heap18: ");
  Serial.println(ESP.getFreeHeap());

    // CONSUMING TOO MUCH HEAP AND NOT RELEASING IT!
    // //HTTPClient http;
    // HTTPClient* http = new HTTPClient();
    // call_backend_V2_successfull = http->begin(clientSecure, backend_endpoint);
    // Serial.print("Free Heap6 ");
    // Serial.println(ESP.getFreeHeap());
    // (void)http->POST(json);
    // //if(http->POST(json) == 200)
    // {
      
    //   call_backend_V2_successfull = true;
    //   Serial.print("Free Heap6.5 ");
    //   Serial.println(ESP.getFreeHeap());
    // }
    
    // // Read response
    // Serial.print(http->getString());
    // Serial.print("Free Heap7 ");
    // Serial.println(ESP.getFreeHeap());
    // // Disconnect
    // http->end();
    // delete http;  // Free the allocated memory


    doc.clear();

  } 
  if(call_backend_V2_successfull == true)
  {
    docArray.clear();
  }

  
}


const int meter_values_buffer_length = 10;
unsigned long meter_values[meter_values_buffer_length][2] = {0};
int32_t previous_meter_value = 0;
void store_meter_value() 
{

  if(ESP.getFreeHeap() < 1000)
  {
    Serial.println("Not enough free heap to store another value");
    return;
  }

  int32_t meter_value = get_meter_value_from_telegram();
  if(meter_value <= 0) return;
  if(meter_value == previous_meter_value) return;
  previous_meter_value = meter_value;
  
  JsonObject newEntry = docArray.add<JsonObject>(); //docArray.createNestedObject();
  newEntry["ts"] = timeClient.getEpochTime();
  

  
  newEntry["meter"] = meter_value; //();
  if(temperature_object.isChecked()) 
  {
  Temp_sensors.requestTemperatures();
  newEntry["temp"] = Temp_sensors.getTempCByIndex(0);
  } 

  Serial.print("Size: ");
  Serial.println(docArray.size());
  Serial.print("Free Heap: ");
  Serial.println(ESP.getFreeHeap());
  // String jsonString;
  // serializeJson(docArray, jsonString);
  // Serial.println(jsonString);

  
}


void loop()
{
  // -- doLoop should be called as frequently as possible.
  iotWebConf.doLoop();
  ArduinoOTA.handle();
  timeClient.update();
  handle_telegram();
  

   if(millis() - last_call > 1000*max(5, atoi(read_meter_intervall)))
    {
      call_backend();
      last_call = millis();
      store_meter_value();
    }

    if(timeClient.getMinutes() %  atoi(backend_call_minute) == 0 && millis() - last_call_backend_v2 > 60000)
    {
      
      call_backend_V2();
      
    }
    
}



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
  s += String(temperature);
  s += "<li>Systemzeit: ";
  s += String(timeClient.getFormattedTime());
  s += " / ";
  s += String(timeClient.getEpochTime());
  s += "</ul>";

  s += "</ul><br>";
  s += "Free Heap ";
  s += String(ESP.getFreeHeap());

  
  s += "<br>Go to <a href='config'>configure page</a> to change values.";
  s += "<br><a href='showTelegram'>Show Telegram</a>";
  s += "<br><b>Detected Meter Value</b>: "+String(get_meter_value_from_telegram());
  s += "<br><b>Detected Meter Value PV</b>: "+String(get_meter_value_PV());

  String jsonString;
  serializeJson(docArray, jsonString);
  s += "<br><br>";
  s += jsonString;
s += "<br><br>";

  s += "<br></body></html>\n";

  server.send(200, "text/html", s);
}
void showTemperature(){
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
  //receive_telegram(); // Wait for complete Telegram
  if(!prefix_suffix_correct()) s += "<br><font color=red>incomplete telegram</font>";

 String color;
 
 int signature_7101 = 9999;
  for (int i = 0; i < TELEGRAM_LENGTH; i++) {
    if(i<TELEGRAM_LENGTH-5 && TELEGRAM[i] == 7 && TELEGRAM[i+1] == 1 && TELEGRAM[i+2] == 0 && TELEGRAM[i+3] == 1 && TELEGRAM[i+4] == 8) 
    { 
      color = "bgcolor=959018";
      signature_7101 = i;
    }
    else if(i>signature_7101 && TELEGRAM[i] == 0x77)
    {
      signature_7101 = 9999;
      color = "bgcolor=959018";
    }
    else if(i>=atoi(telegram_offset) && i<atoi(telegram_offset)+atoi(telegram_length))
    {

      color = "bgcolor=cccccc";
    }
    else color = "";
    s += "<tr><td>" + String(i) + "</td><td "+String(color)+">"+String(TELEGRAM[i], HEX)+"</td></tr>";
  }
  s += "</table";


  s += "</body></html>\n";

  server.send(200, "text/html", s);
}




void configSaved()
{
  Serial.println("Configuration was updated.");
  if(led_blink_object.isChecked()) iotWebConf.enableBlink();
  else iotWebConf.disableBlink();
}

bool formValidator(iotwebconf::WebRequestWrapper* webRequestWrapper)
{
  Serial.println("Validating form.");
  bool valid = true;

/*
  int l = webRequestWrapper->arg(API_endpoint.getId()).length();
  if (l < 3)
  {
    API_endpoint.errorMessage = "Please provide at least 3 characters for this test!";
    valid = false;
  }
*/
  return valid;
}
