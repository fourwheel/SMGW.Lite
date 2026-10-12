#pragma once
#include <Arduino.h>

void          Time_begin();         // start SNTP (call once from setup())
bool          Time_setFromHttpDate(const String &line); // fallback: set time from an HTTP "Date:" header while NTP is not synced
unsigned long Time_getEpochTime();
bool          Time_isSynced();      // true once system time is plausible (via NTP or backend Date header)
bool          Time_isPlausible(unsigned long epoch);
int           Time_getMinutes();
String        Time_getFormattedTime();
String        Time_formatTimestamp(unsigned long timestamp);
String        Time_formatUptime();
