#pragma once

// Called once on first WiFi connect during setup. Checks if this boot is
// a post-OTA validation boot and confirms or rolls back the new firmware.
void FwUpdate_init();

// Called from loop() on an hourly timer. Checks the server for a newer
// firmware version and flashes it if the version string differs.
void FwUpdate_check();
