#pragma once
#include <Arduino.h>

// Serial config scanner
#define SERIAL_SCAN_TIMEOUT_MS  10000  // max listen time per candidate config (ms)
#define SERIAL_SCAN_MIN_BYTES   8      // minimum bytes required to attempt detection

struct SerialScanEntry {
    uint32_t    baudRate;
    uint32_t    uartConfig;
    const char* label;
};

extern const SerialScanEntry SERIAL_SCAN_TABLE[];
extern const int             SERIAL_SCAN_TABLE_SIZE;

// Active serial configuration
uint32_t SerialScan_getActiveBaud();
uint32_t SerialScan_getActiveConfig();

// Scan lifecycle (called from telegramTask in main.cpp)
void SerialScan_requestScan();   // reset state + arm pending flag
bool SerialScan_consumePending();  // read-and-clear pending flag
bool SerialScan_isRunning();

// Status JSON for /serialScanStatus endpoint
String SerialScan_getStatusJson();

// Web UI helpers
String SerialScan_buildTableRows();
String SerialScan_activeLabel();
bool   SerialConfig_setByIndex(int idx);

// Called once from setup()
void SerialConfig_load();

// Long-running scan — called from telegramTask when consumePending() returns true
void SerialScan_run();
