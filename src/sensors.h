#pragma once
#include <OneWire.h>
#include <DallasTemperature.h>

extern DallasTemperature Temp_sensors;
extern unsigned long     last_temperature;
extern bool              read_temperature;

void myStrom_get_Meter_value();
// Status for the web UI: 0 = not requested yet, 1 = last request ok,
// 2 = last request failed (retried after 5 min).
int      myStrom_state();
uint32_t myStrom_ago_min(); // minutes since the last request result
int      myStrom_power();   // W, from the last successful request
void handle_temperature();
