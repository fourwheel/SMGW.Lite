#pragma once
#include <sdkconfig.h>

#define FIRMWARE_VERSION "1.4.3"
#define CONFIG_VERSION   "2906"

// Build target, sent to the backend as "hw" so it picks the matching binary
// from fw_releases/v<version>/<target>/. Must match the PlatformIO env names
// (and thereby the manifests/<version>/<env>/ folders).
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#define FIRMWARE_TARGET "esp32c3"
#else
#define FIRMWARE_TARGET "esp32-nodemcu"
#endif
