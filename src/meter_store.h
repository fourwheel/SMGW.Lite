#pragma once
#include <Arduino.h>
#include "meter_value.h"

void handle_MeterValue_trigger();
void handle_MeterValue_store();
bool MeterValue_store(bool override);
bool MeterValue_unchangedSincePrev(const MeterValue &v);
