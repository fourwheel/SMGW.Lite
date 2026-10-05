#pragma once
#include <Arduino.h>

const int LOG_BUFFER_SIZE = 200;

// The number of values to transmit is logged as LOG_VALUE_COUNT_BASE + n, so it
// can't collide with a status code (a full buffer holds more than 1000 values).
// Firmware up to 1.4.1 logged the bare number n (0-999 in the dashboards).
const int LOG_VALUE_COUNT_BASE = 100000;

struct LogEntry {
  unsigned long timestamp;
  unsigned long uptime;
  int statusCode;
};

void   LogBuffer_reset();
// Log_Add() is the normal call: it writes the entry and marks the log for
// upload with the next backend call (usually within 2 min), long before the
// ring buffer (~1 h of normal operation) overwrites it. Use it for every
// error, warning or event worth seeing in the backend.
// Log_AddWithoutTransmit() only writes the entry; use it for routine entries
// that occur in every backend/TAF cycle and would otherwise cause an upload on
// every call. Returns false if the entry was dropped as a consecutive duplicate.
void   Log_Add(int statusCode);
bool   Log_AddWithoutTransmit(int statusCode);
String Log_StatusCodeToString(int statusCode);
extern const char LOG_TABLE_HEADER_HTML[];
String Log_EntryRowByAge(int n);            // HTML row of the n-th newest entry, "" if unused
String Log_BufferToString(int showNumber);  // short excerpt (table only), e.g. last 10 for /sysinfo

// Raw buffer access for binary backend transmission (do not modify directly)
const LogEntry* Log_getRawBuffer();
int             Log_getIndex();

#if defined(ESP32)
String Log_get_reset_reason();
#endif
