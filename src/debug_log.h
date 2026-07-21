#pragma once

#ifdef SERIAL_DEBUG
  #define DLOG(x)    Serial.print(x)
  #define DLOGLN(x)  Serial.println(x)
  #define DLOGF(...) Serial.printf(__VA_ARGS__)
#else
  #define DLOG(x)    do {} while(0)
  #define DLOGLN(x)  do {} while(0)
  #define DLOGF(...) do {} while(0)
#endif
