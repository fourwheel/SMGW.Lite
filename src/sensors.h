#pragma once
#include <OneWire.h>
#include <DallasTemperature.h>

extern DallasTemperature Temp_sensors;
extern unsigned long     last_temperature;
extern bool              read_temperature;

void myStrom_get_Meter_value();
void handle_temperature();
