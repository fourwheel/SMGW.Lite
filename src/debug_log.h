#pragma once

// ---------------------------------------------------------------------------
// Serial debug macros
// Build with -DSERIAL_DEBUG (env:esp32c3_debug) to enable Serial output.
// In the release build (env:esp32c3) all DLOG* calls compile to nothing.
// OTA progress/error output intentionally uses Serial directly so it always
// works regardless of this flag.
// ---------------------------------------------------------------------------
#ifdef SERIAL_DEBUG
  #define DLOG(x)    Serial.print(x)
  #define DLOGLN(x)  Serial.println(x)
  #define DLOGF(...) Serial.printf(__VA_ARGS__)
#else
  #define DLOG(x)    do {} while(0)
  #define DLOGLN(x)  do {} while(0)
  #define DLOGF(...) do {} while(0)
#endif
