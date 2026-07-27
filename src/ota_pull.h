#pragma once

// Called once on first WiFi connect. Checks if this boot is a post-pull-OTA
// validation boot and confirms or triggers bootloader rollback.
void OtaPull_init();

// Called from loop() on an hourly timer. Checks the server for a newer
// firmware version and flashes it autonomously if the version string differs.
void OtaPull_check();
