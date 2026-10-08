#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Called once early in setup(). If the previous OTA pull flashed this image
// (NVS pending flag) and it is not confirmed yet, starts the post-update
// validation; otherwise confirms the running image. Logs 6028 if the pulled
// update was rolled back by the bootloader (restart before validation).
void OtaPull_boot();

// Called from loop(). While a validation is in progress: tries backend_test
// once per minute when WiFi is up; on success confirms the image, after 15 min
// since boot without success rolls back to the previous firmware.
void OtaPull_validate();

// True while the post-update validation is in progress; no other update is
// installed until it is done.
bool OtaPull_isValidating();

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
