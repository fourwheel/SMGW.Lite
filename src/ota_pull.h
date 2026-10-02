#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Called once on first WiFi connect. Checks NVS for a pending-validation flag
// set by the previous OTA flash; contacts the backend to confirm the firmware
// works, or rolls back to the previous OTA slot via esp_ota_set_boot_partition.
void OtaPull_init();

// Stores the fw_update object of a confirmed backend response as the current
// offer (a null variant clears it) and requests an install check if the
// offered version differs from the running one. Caller holds Sema_Backend.
void OtaPull_setOffer(JsonVariantConst fw_update);

// Installs the offered firmware (download, SHA256 check, flash, reboot).
// manual = confirmed on the Remote FW Update page: also installs a version
// that was rolled back before or whose download failed within the last hour.
void OtaPull_check(bool manual);

// Asks the backend which firmware is assigned (backend_test) and updates the
// offer. Returns false if the backend is unreachable or its confirmation is
// invalid; version_out is empty if no update is assigned. Caller holds Sema_Backend.
bool OtaPull_query(String& version_out);

// True if version failed post-update validation and was rolled back.
bool OtaPull_isRolledBack(const String& version);
