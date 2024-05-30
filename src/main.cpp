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
#define CONFIG_VERSION "ar"

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

SoftwareSerial mySerial(D5, D6); // RX, TX

// -- Method declarations.
void handleRoot();
// -- Callback methods.
void configSaved();
bool formValidator(iotwebconf::WebRequestWrapper* webRequestWrapper);

DNSServer dnsServer;
WebServer server(80);

char API_endpointValue[STRING_LEN];
char telegram_offset[NUMBER_LEN];
char telegram_length[NUMBER_LEN];
char telegram_prefix[NUMBER_LEN];
char telegram_suffix[NUMBER_LEN];


char backend_endpoint[STRING_LEN];
char backend_token[STRING_LEN];
char backend_intervall[NUMBER_LEN];
char backend_ID[ID_LEN];



IotWebConf iotWebConf(thingName, &dnsServer, &server, wifiInitialApPassword, CONFIG_VERSION);
// -- You can also use namespace formats e.g.: iotwebconf::TextParameter
IotWebConfTextParameter API_endpoint = IotWebConfTextParameter("Nachwelt", "API_endpoint", API_endpointValue, STRING_LEN);
IotWebConfParameterGroup group1 = IotWebConfParameterGroup("group1", "Telegram Param");
IotWebConfParameterGroup group2 = IotWebConfParameterGroup("group2", "Backend Config");
IotWebConfNumberParameter telegram_offset_object = IotWebConfNumberParameter("Offset", "telegram_offset_object", telegram_offset, NUMBER_LEN, "20", "1..TELEGRAM_LENGTH", "min='1' max='TELEGRAM_LENGTH' step='1'");
IotWebConfNumberParameter telegram_length_object = IotWebConfNumberParameter("Length", "telegram_length_object", telegram_length, NUMBER_LEN, "8", "1..TELEGRAM_LENGTH", "min='1' max='TELEGRAM_LENGTH' step='1'");
IotWebConfNumberParameter telegram_prefix_object = IotWebConfNumberParameter("Prefix Begin", "telegram_prefix", telegram_prefix, NUMBER_LEN, "0", "1..TELEGRAM_LENGTH", "min='0' max='TELEGRAM_LENGTH' step='1'");
IotWebConfNumberParameter telegram_suffix_object = IotWebConfNumberParameter("Suffix Begin", "telegram_suffix", telegram_suffix, NUMBER_LEN, "100", "1..TELEGRAM_LENGTH", "min='100' max='TELEGRAM_LENGTH' step='1'");

IotWebConfTextParameter backend_endpoint_object = IotWebConfTextParameter("backend endpoint", "backend_endpoint", backend_endpoint, STRING_LEN);
IotWebConfTextParameter backend_ID_object = IotWebConfTextParameter("backend ID", "backend_ID", backend_ID, ID_LEN);
IotWebConfTextParameter backend_token_object = IotWebConfTextParameter("backend token", "backend_token", backend_token, STRING_LEN);
IotWebConfNumberParameter backend_intervall_object = IotWebConfNumberParameter("backend intervall", "backend_intervall", backend_intervall, NUMBER_LEN, "20", "5..100 s", "min='5' max='100' step='1'");

WiFiClient client;
WiFiClientSecure clientSecure;

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
  group2.addItem(&backend_intervall_object);

  // iotWebConf.setStatusPin(STATUS_PIN); // If you do not define a status Pin before init, the LED shoud stay off.
  iotWebConf.setConfigPin(CONFIG_PIN);
  iotWebConf.addSystemParameter(&API_endpoint);
  iotWebConf.addParameterGroup(&group1);
  iotWebConf.addParameterGroup(&group2);

  iotWebConf.setConfigSavedCallback(&configSaved);
  iotWebConf.setFormValidator(&formValidator);
  iotWebConf.getApTimeoutParameter()->visible = true;

  // -- Initializing the configuration.
  iotWebConf.init();

  // -- Set up required URL handlers on the web server.
  server.on("/", handleRoot);
  server.on("/config", []{ iotWebConf.handleConfig(); });
  server.on("/restart", []{ ESP.restart(); });
  server.onNotFound([](){ iotWebConf.handleNotFound(); });

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
bool prefix_suffix_correct2(){
  int prefix = atoi(telegram_prefix);
  int suffix = atoi(telegram_suffix);
  

  if(suffix == 0)
  {
    Serial.println("Suffix Must not be 0");
    return false;
  }

  if(TELEGRAM[0] == 0x1B
  && TELEGRAM[1] == 0x1B
  && TELEGRAM[2] == 0x1B
  && TELEGRAM[3] == 0x1B
  && TELEGRAM[m_i_max-7] == 0x1B
  && TELEGRAM[m_i_max-6] == 0x1B
  && TELEGRAM[m_i_max-5] == 0x1B
  && TELEGRAM[m_i_max-4] == 0x1B
  && TELEGRAM[m_i_max-3] == 0x1A) return true;
  else return false;
}

int32_t get_meter_value_from_telegram()
{
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
  return 0;
  Serial.println(F("Connecting..."));

  // Connect to HTTP server

  client.setTimeout(10000);
  if (!client.connect("192.168.188.111", 80)) {
    Serial.println(F("Connection failed"));
    return -1;
  }
  // 192.168.188.111
  // mystrom-switch-b3e3c0

  Serial.println(F("Connected!"));

  // Send HTTP request
  client.println(F("GET /report HTTP/1.0"));
  client.println(F("Host: 192.168.188.111"));
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
    Serial.print(F("Unexpected response: "));
    Serial.println(status);
    client.stop();
    return -3;
  }

  // Skip HTTP headers
  char endOfHeaders[] = "\r\n\r\n";
  if (!client.find(endOfHeaders)) {
    Serial.println(F("Invalid response"));
    client.stop();
    return -4;
  }

  // Allocate the JSON document
  JsonDocument doc;

  // Parse JSON object
  DeserializationError error = deserializeJson(doc, client);
  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
    client.stop();
    return -5;
  }

  // Extract values
  
  return (doc["energy_since_boot"].as<int>());

  // Disconnect
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
  
  Serial.println("call backend");
  Serial.println("delay " + millis() - timestamp_telegram);
  Serial.println("meter " + get_meter_value_from_telegram());
  
  
  Serial.println("pv " + get_meter_value_PV());

  JsonDocument doc;

  doc["meter_value"] = get_meter_value_from_telegram();
  doc["meter_value_PV"] = get_meter_value_PV();
  doc["ID"] = backend_ID;
  doc["token"] = backend_token;
  String json;
  serializeJson(doc, json);
Serial.println("json " + json);
  if (doc["meter_value"] != -2) 
  { 
    Serial.println("Connected to Server sunzilla.de");
    // "/hz/n3.php?meter_value=" + String(meter_value) + "&timestamp_client=" + String(timeClient.getEpochTime());
    //client3.println("GET /hz/n3.php?meter_value=" + String(meter_value) + "&wifi=" + String(wifi_reconnect) + "&uptime=" + String(last_serial/*uptime*/) + "&last_sent=" + String((millis() - last_sent)/1000) + "&timestamp_client=" + String(timeClient.getEpochTime()) +"&temp="+String(Temp_sensors.getTempCByIndex(0))+" HTTP/1.1\r\nHost: sunzilla.de\r\n\r\n");
    ///Serial << "status: " <<client.status();
    // Send request
    HTTPClient http;
    http.begin(clientSecure, "https://sunzilla.de/hz/v1/");
    http.POST(json);

    // Read response
    Serial.print(http.getString());

    // Disconnect
    http.end();
  } 
 // else { Serial.println("connection to sunzilla.de failed"); }
}
void loop()
{
  // -- doLoop should be called as frequently as possible.
  iotWebConf.doLoop();
  ArduinoOTA.handle();

  handle_telegram();
  

   if(millis() - last_call > 1000*max(5, atoi(backend_intervall)))
    {
      call_backend();
      last_call = millis();
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
  s += "<li>Nachwelt: ";
  s += API_endpointValue;
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
  s += "<li>Backend ID: ";
  s += backend_ID;
  s += "<li>Backend Token: ";
  s += backend_token;
s += "<li>Backend Intervall: ";
  s += atoi(backend_intervall);

  s += "</ul>";
  s += "Go to <a href='config'>configure page</a> to change values.";
  s += "<br><b>Detected Meter Value</b>: "+String(get_meter_value_from_telegram());
  s += "<br>Received Telegram from mMe via SML<br><table border=1>";
  receive_telegram(); // Wait for complete Telegram
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
