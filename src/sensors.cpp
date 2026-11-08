// Additional sensors: DS18B20 temperature and myStrom PV switch.
#include "sensors.h"
#include "app_globals.h"
#include "meter_value.h"
#include "log_buffer.h"
#include "debug_log.h"
#include <ArduinoJson.h>

// Temperature vars
#define ONE_WIRE_BUS 4
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature Temp_sensors(&oneWire);
unsigned long last_temperature  = 0;
bool read_temperature           = false;
int current_temperature         = 0;

// After a failed request, myStrom is skipped for MYSTROM_RETRY_MS: every
// attempt runs in loop() and blocks it (and with it the web interface) for up
// to the 3 s timeout. Meanwhile, and after any failure, solar and temperature
// keep the last values read, since the parsers carry them over in
// LastMeterValue, so an unreachable myStrom does not store 0.
static const unsigned long MYSTROM_RETRY_MS = 5UL * 60 * 1000;
static unsigned long mystrom_failed_at = 0; // millis() of the last failure, 0 = none

void myStrom_get_Meter_value()
{
  if (!mystrom_PV_object.isChecked()) { LastMeterValue.solar = 0; return; }
  if (mystrom_failed_at != 0 && millis() - mystrom_failed_at < MYSTROM_RETRY_MS) return;

  DLOGLN(F("myStrom_get_Meter_value Connecting..."));
  WiFiClient client;
  // WiFiClient::setTimeout() takes seconds (connect and every read). This runs
  // in loop() on every store, so an unreachable myStrom must not block long.
  client.setTimeout(3);
  if (!client.connect(mystrom_PV_IP, 80)) { Log_Add(5000); DLOGLN(F("myStrom_get_Meter_value Connection failed")); mystrom_failed_at = millis(); return; }

  DLOGLN(F("myStrom_get_Meter_value Connected!"));
  client.println(F("GET /report HTTP/1.0"));
  client.print(F("Host: ")); client.println(mystrom_PV_IP);
  client.println(F("Connection: close"));
  if (client.println() == 0) { Log_Add(5001); client.stop(); mystrom_failed_at = millis(); return; }

  char status[32] = {0};
  client.readBytesUntil('\r', status, sizeof(status));
  if (strcmp(status + 9, "200 OK") != 0) { client.stop(); mystrom_failed_at = millis(); return; }

  char endOfHeaders[] = "\r\n\r\n";
  if (!client.find(endOfHeaders)) { client.stop(); mystrom_failed_at = millis(); return; }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, client);
  if (error) { Log_Add(5002); client.stop(); mystrom_failed_at = millis(); return; }

  LastMeterValue.temperature = doc["temperature"].as<float>() * 100;
  LastMeterValue.solar       = doc["energy_since_boot"].as<int>();
  mystrom_failed_at = 0;
  client.stop();
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
