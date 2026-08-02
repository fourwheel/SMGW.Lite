#pragma once


// Called once on first WiFi connect. Checks NVS for a pending-validation flag
// set by the previous OTA flash; contacts the backend to confirm the firmware
// works, or rolls back to the previous OTA slot via esp_ota_set_boot_partition.
void OtaPull_init();

// Checks the server for a newer firmware version and flashes it autonomously
// if the version string differs. Triggered by backend hint or 24 h fallback.
void OtaPull_check();

// Fetches only the manifest version string — no download, no flash.
// Returns false if the manifest is unreachable or invalid.
bool OtaPull_fetchManifestVersion(String& version_out);
