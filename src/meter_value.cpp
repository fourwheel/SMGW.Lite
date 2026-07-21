#include "meter_value.h"
#include "app_globals.h"
#include "log_buffer.h"
#include "debug_log.h"

// ---------------------------------------------------------------------------
// Globals defined here — declared as extern in meter_value.h
// ---------------------------------------------------------------------------
bool     config_temperature_enabled = false;
bool     config_solar_enabled       = false;
bool     config_obis280_enabled     = false;

uint8_t* MeterValueBuffer           = nullptr;
int      Meter_Value_Buffer_Size    = 234;
bool     meter_value_buffer_overflow = false;
bool     meter_value_buffer_full     = false;
int      meter_value_override_i      = 0;
int      meter_value_NON_override_i  = 233; // Meter_Value_Buffer_Size - 1

MeterValue LastMeterValue = {};
MeterValue PrevMeterValue = {};

int  last_init_buffer_kb = -1;
bool last_init_temp      = false;
bool last_init_solar     = false;
bool last_init_280       = false;

bool         meter_value_buffer_is_auto  = false;

// ---------------------------------------------------------------------------
// Buffer layout
// ---------------------------------------------------------------------------
size_t MeterValue_EntrySize()
{
  size_t s = 4 + 4;
  if (config_temperature_enabled) s += 4;
  if (config_solar_enabled)       s += 4;
  if (config_obis280_enabled)     s += 4;
  return s;
}

static inline size_t MeterValue_Offset(int index)
{
  return (size_t)index * MeterValue_EntrySize();
}

String MeterValue_BuildFieldsParam()
{
  String fields = "fields=ts,m180";
  if (config_temperature_enabled) fields += ",temp";
  if (config_solar_enabled)       fields += ",solar";
  if (config_obis280_enabled)     fields += ",m280";
  return fields;
}

// ---------------------------------------------------------------------------
// Buffer read / write
// ---------------------------------------------------------------------------
void MeterValue_write(int index, uint32_t ts, uint32_t m180,
                      uint32_t temp, uint32_t solar, uint32_t m280)
{
  if (!MeterValueBuffer) return;
  size_t o = MeterValue_Offset(index);
  memcpy(MeterValueBuffer + o, &ts,   4); o += 4;
  memcpy(MeterValueBuffer + o, &m180, 4); o += 4;
  if (config_temperature_enabled) { memcpy(MeterValueBuffer + o, &temp,  4); o += 4; }
  if (config_solar_enabled)       { memcpy(MeterValueBuffer + o, &solar, 4); o += 4; }
  if (config_obis280_enabled)     { memcpy(MeterValueBuffer + o, &m280,  4); }
}

void MeterValue_read(int index, uint32_t &ts, uint32_t &m180,
                     uint32_t &temp, uint32_t &solar, uint32_t &m280)
{
  ts = 0; m180 = 0; temp = 0; solar = 0; m280 = 0;
  if (!MeterValueBuffer) return;
  size_t o = MeterValue_Offset(index);
  memcpy(&ts,   MeterValueBuffer + o, 4); o += 4;
  memcpy(&m180, MeterValueBuffer + o, 4); o += 4;
  if (config_temperature_enabled) { memcpy(&temp,  MeterValueBuffer + o, 4); o += 4; }
  if (config_solar_enabled)       { memcpy(&solar, MeterValueBuffer + o, 4); o += 4; }
  if (config_obis280_enabled)     { memcpy(&m280,  MeterValueBuffer + o, 4); }
}

bool MeterValue_slot_empty(int index)
{
  uint32_t ts, m180, temp, solar, m280;
  MeterValue_read(index, ts, m180, temp, solar, m280);
  return (ts == 0 && m180 == 0);
}

// ---------------------------------------------------------------------------
// Slot count / budget
// ---------------------------------------------------------------------------
int MeterValue_slots_from_budget(size_t budgetBytes)
{
  size_t entrySize = MeterValue_EntrySize();
  if (entrySize == 0) return 8;
  int slots = (int)(budgetBytes / entrySize);
  if (slots < 8) slots = 8;
  return slots;
}

int MeterValue_calc_max_slots_for_display()
{
  int configuredKB = atoi(Meter_Value_Buffer_Size_Char);
  size_t budget = (configuredKB > 0)
                  ? (size_t)configuredKB * 1024
                  : BUFFER_REFERENCE_BYTES;
  return MeterValue_slots_from_budget(budget);
}

// ---------------------------------------------------------------------------
// Buffer lifecycle
// ---------------------------------------------------------------------------
void MeterValue_init_Buffer()
{
  config_temperature_enabled = config_temperature_object.isChecked();
  config_solar_enabled       = config_solar_object.isChecked();
  config_obis280_enabled     = config_280_object.isChecked();

  int configuredKB = atoi(Meter_Value_Buffer_Size_Char);
  size_t budget;
  if (configuredKB <= 0) {
    budget                     = BUFFER_REFERENCE_BYTES;
    meter_value_buffer_is_auto = true;
  } else {
    budget                     = (size_t)configuredKB * 1024;
    meter_value_buffer_is_auto = false;
  }

  Meter_Value_Buffer_Size = MeterValue_slots_from_budget(budget);

  if (MeterValueBuffer) {
    delete[] MeterValueBuffer;
    MeterValueBuffer = nullptr;
  }

  size_t total = (size_t)Meter_Value_Buffer_Size * MeterValue_EntrySize();
  MeterValueBuffer = new uint8_t[total];
  if (!MeterValueBuffer) {
    DLOGLN("MeterValue buffer allocation failed!");
    Log_AddEntry(1002);
    return;
  }

  memset(MeterValueBuffer, 0, total);

  meter_value_override_i      = 0;
  meter_value_NON_override_i  = Meter_Value_Buffer_Size - 1;
  meter_value_buffer_overflow = false;
  meter_value_buffer_full     = false;

  DLOGF("MeterValue buffer init: %d slots x %d bytes = %d bytes total (%s)\n",
        Meter_Value_Buffer_Size, (int)MeterValue_EntrySize(), (int)total,
        meter_value_buffer_is_auto ? "auto" : "manual");
  DLOGF("  Fields: ts,m180%s%s%s\n",
        config_temperature_enabled ? ",temp"  : "",
        config_solar_enabled       ? ",solar" : "",
        config_obis280_enabled     ? ",m280"  : "");

  last_init_buffer_kb = atoi(Meter_Value_Buffer_Size_Char);
  last_init_temp      = config_temperature_enabled;
  last_init_solar     = config_solar_enabled;
  last_init_280       = config_obis280_enabled;
}

void resetMeterValue(MeterValue &val)
{
  int32_t  saved_solar = val.solar;
  uint32_t saved_temp  = val.temperature;
  val = MeterValue{};
  if (mystrom_PV_object.isChecked()) val.solar = saved_solar;
  val.temperature = saved_temp;
}

void MeterValues_clear_Buffer()
{
  if (!MeterValueBuffer) return;
  memset(MeterValueBuffer, 0, (size_t)Meter_Value_Buffer_Size * MeterValue_EntrySize());
  meter_value_override_i      = 0;
  meter_value_NON_override_i  = Meter_Value_Buffer_Size - 1;
  meter_value_buffer_overflow = false;
  meter_value_buffer_full     = false;
}

// ---------------------------------------------------------------------------
// Counters
// ---------------------------------------------------------------------------
int MeterValue_Num()
{
  if (meter_value_buffer_full || meter_value_buffer_overflow)
    return Meter_Value_Buffer_Size;
  return (meter_value_override_i + ((Meter_Value_Buffer_Size - 1) - meter_value_NON_override_i));
}

int MeterValue_Num2()
{
  int count = 0;
  for (int i = 0; i < Meter_Value_Buffer_Size; i++)
    if (!MeterValue_slot_empty(i)) count++;
  return count;
}
