#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// MeterValue — working copy struct for the most recent telegram reading.
// The ring-buffer uses a compact packed binary layout instead (see MeterValueBuffer).
// ---------------------------------------------------------------------------
struct MeterValue {
  uint32_t timestamp;        // Unix epoch, seconds
  uint32_t meter_value_180;  // OBIS 1.8.0 consumption counter, unit: 0.1 Wh
  uint32_t temperature;      // temperature * 100 (e.g. 2150 = 21.50 degC)
  uint32_t solar;            // MyStrom / solar energy counter
  uint32_t meter_value_280;  // OBIS 2.8.0 feed-in counter, unit: 0.1 Wh
  uint32_t power_import;     // OBIS 1.7.0 import power, unit: W
  uint32_t power_export;     // OBIS 2.7.0 export power, unit: W
  int32_t  net_power;        // OBIS 16.7.0 net power, unit: W (negative = feed-in)
};

// Runtime feature flags — set by MeterValue_init_Buffer() from config
extern bool     config_temperature_enabled;
extern bool     config_solar_enabled;
extern bool     config_obis280_enabled;

// Ring-buffer state
extern uint8_t* MeterValueBuffer;
extern int      Meter_Value_Buffer_Size;
extern int      meter_value_override_i;
extern int      meter_value_NON_override_i;
extern bool     meter_value_buffer_overflow;
extern bool     meter_value_buffer_full;

// Working copies — written by telegram parsers, read by store/backend/webserver
extern MeterValue LastMeterValue;
extern MeterValue PrevMeterValue;

// Layout-change tracking — checked by Param_configSaved() in main.cpp
extern int  last_init_buffer_kb;
extern bool last_init_temp;
extern bool last_init_solar;
extern bool last_init_280;

// Display helpers used by sysinfo handler
extern bool          meter_value_buffer_is_auto;
static const size_t  BUFFER_REFERENCE_BYTES = 16384;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
size_t MeterValue_EntrySize();
String MeterValue_BuildFieldsParam();
void   MeterValue_write(int index, uint32_t ts, uint32_t m180,
                        uint32_t temp, uint32_t solar, uint32_t m280);
void   MeterValue_read(int index, uint32_t &ts, uint32_t &m180,
                       uint32_t &temp, uint32_t &solar, uint32_t &m280);
bool   MeterValue_slot_empty(int index);
int    MeterValue_slots_from_budget(size_t budgetBytes);
int    MeterValue_calc_max_slots_for_display();
void   MeterValue_init_Buffer();
void   resetMeterValue(MeterValue &val);
void   MeterValues_clear_Buffer();
int    MeterValue_Num();
int    MeterValue_Num2();
