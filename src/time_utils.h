#pragma once
#include <Arduino.h>

unsigned long Time_getEpochTime();
int           Time_getMinutes();
String        Time_getFormattedTime();
String        Time_formatTimestamp(unsigned long timestamp);
String        Time_formatUptime();
