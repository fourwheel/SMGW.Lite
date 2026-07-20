#pragma once
#include <Arduino.h>

const int LOG_BUFFER_SIZE = 200;

struct LogEntry {
  unsigned long timestamp;
  unsigned long uptime;
  int statusCode;
};

void   LogBuffer_reset();
void   Log_AddEntry(int statusCode);
String Log_StatusCodeToString(int statusCode);
String Log_BufferToString(int showNumber = LOG_BUFFER_SIZE);

// Raw buffer access for binary backend transmission (do not modify directly)
const LogEntry* Log_getRawBuffer();
int             Log_getIndex();

#if defined(ESP32)
String Log_get_reset_reason();
#endif
