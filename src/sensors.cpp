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
int current_temperature         = METER_VALUE_NONE;

// After a failed request, myStrom is skipped for a while: every attempt runs
// in loop() and blocks it (and with it the web interface) for up to the
// timeout. If the myStrom accepted the connection but answered too slowly
// (5003, 5005), it is alive, so it is retried after 1 min; otherwise after
// 5 min. The pause runs from the start of the failed attempt and the short one
// is just under 1 min, so with TAF14 = 60 s the next store retries (measured
// from the end, the timeout would make it skip that store). Whenever no value
// was read (failure, pause, no WiFi), solar and temperature are
// METER_VALUE_NONE, so the backend gets a gap (NULL) instead of a stale value
// or 0.
static const unsigned long MYSTROM_RETRY_MS      = 5UL * 60 * 1000;
static const unsigned long MYSTROM_SLOW_RETRY_MS = 55UL * 1000;
static const int           MYSTROM_TIMEOUT_S     = 5;
static unsigned long mystrom_failed_at = 0; // millis() at the start of the last failed attempt, 0 = none
static unsigned long mystrom_retry_ms  = MYSTROM_RETRY_MS; // pause after that failure
static unsigned long mystrom_ok_at     = 0; // millis() of the last success, 0 = none
static int           mystrom_power     = 0; // W, from the last successful request (display only)

// Requests /report and writes solar and temperature to LastMeterValue.
// Returns false if no value was read.
static bool myStrom_request()
{
  if (mystrom_failed_at != 0 && millis() - mystrom_failed_at < mystrom_retry_ms) return false;
  // No WiFi (e.g. the boot snapshot runs before the connection is up): skip
  // without counting it as a failure, so it neither logs 5000 nor starts the pause.
  if (!wifi_connected) return false;

  DLOGLN(F("myStrom_get_Meter_value Connecting..."));
  WiFiClient client;
  // WiFiClient::setTimeout() takes seconds (connect and every read). This runs
  // in loop() on every store, so an unreachable myStrom must not block long.
  client.setTimeout(MYSTROM_TIMEOUT_S);
  const unsigned long started = millis();
  mystrom_retry_ms = MYSTROM_RETRY_MS; // set to MYSTROM_SLOW_RETRY_MS below for 5003 / 5005
  if (!client.connect(mystrom_PV_IP, 80)) { Log_Add(5000); DLOGLN(F("myStrom_get_Meter_value Connection failed")); mystrom_failed_at = started; return false; }

  DLOGLN(F("myStrom_get_Meter_value Connected!"));
  client.println(F("GET /report HTTP/1.0"));
  client.print(F("Host: ")); client.println(mystrom_PV_IP);
  client.println(F("Connection: close"));
  if (client.println() == 0) { Log_Add(5001); client.stop(); mystrom_failed_at = started; return false; }

  char status[32] = {0};
  client.readBytesUntil('\r', status, sizeof(status));
  // 5003: no response within the timeout, 5004: response other than 200 OK
  if (status[0] == '\0') { Log_Add(5003); client.stop(); mystrom_failed_at = started; mystrom_retry_ms = MYSTROM_SLOW_RETRY_MS; return false; }
  if (strcmp(status + 9, "200 OK") != 0) { Log_Add(5004); client.stop(); mystrom_failed_at = started; return false; }

  char endOfHeaders[] = "\r\n\r\n";
  if (!client.find(endOfHeaders)) { Log_Add(5005); client.stop(); mystrom_failed_at = started; mystrom_retry_ms = MYSTROM_SLOW_RETRY_MS; return false; }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, client);
  if (error) { Log_Add(5002); client.stop(); mystrom_failed_at = started; return false; }

  LastMeterValue.temperature = doc["temperature"].as<float>() * 100;
  // energy_since_boot is in Ws and can be negative (e.g. a battery that charges
  // and discharges). Stored in 0.1 Wh (Ws / 360): as Ws, int32 would overflow at
  // ~596 kWh (as<int>() then returns 0), in 0.1 Wh only at ~214 MWh.
  LastMeterValue.solar       = (int32_t)lround(doc["energy_since_boot"].as<double>() / 360.0);
  mystrom_power     = (int)lroundf(doc["power"].as<float>());
  mystrom_ok_at     = millis();
  mystrom_failed_at = 0;
  client.stop();
  return true;
}

void myStrom_get_Meter_value()
{
  if (mystrom_PV_object.isChecked() && myStrom_request()) return;
  LastMeterValue.solar       = METER_VALUE_NONE;
  // Without myStrom the temperature comes from the DS18B20 (see telegramTask)
  if (mystrom_PV_object.isChecked()) LastMeterValue.temperature = METER_VALUE_NONE;
}

int myStrom_state()
{
  if (mystrom_failed_at != 0) return 2;
  if (mystrom_ok_at != 0)     return 1;
  return 0;
}

uint32_t myStrom_ago_min()
{
  unsigned long at = mystrom_failed_at != 0 ? mystrom_failed_at : mystrom_ok_at;
  return at != 0 ? (millis() - at) / 60000UL : 0;
}

int myStrom_power() { return mystrom_power; }

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
      // -127 degC (-12700 in raw units) = sensor error / not connected -> no value
      current_temperature = raw_temp > -10000 ? (int)raw_temp : METER_VALUE_NONE;
      read_temperature = true;
    }
  }
}
