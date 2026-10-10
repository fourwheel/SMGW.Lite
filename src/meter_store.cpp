// Storing meter values in the buffer and the TAF7 / TAF14 store triggers.
#include "meter_store.h"
#include "app_globals.h"
#include "meter_value.h"
#include "telegram.h"
#include "log_buffer.h"
#include "debug_log.h"
#include "time_utils.h"
#include "sensors.h"

bool MeterValue_trigger_override     = false;
bool MeterValue_trigger_non_override = false;
// MeterValue_trigger_is_taf7 narrows MeterValue_trigger_override down to TAF7
// TAF7 mark stores. The override trigger is also set by the boot snapshot and
// by a manual TAF6 store (/StoreMeterValue); all three share the override store
// path, but only a TAF7 mark store may remove a TAF14 entry stored <10 s before
// it (see handle_MeterValue_store()). A manual TAF6 must keep every reading it
// is asked for, so it must never remove anything.
bool MeterValue_trigger_is_taf7      = false;
bool boot_snapshot_done              = false; // boot snapshot: fired once after first telegram + reliable time
bool last_store_was_override         = false; // true when the last successful store was a TAF7 override
unsigned long last_meter_value_successful = 0;
unsigned long last_taf7_meter_value       = 0;
unsigned long last_taf14_meter_value      = 0;

// True if the counters in v equal the last successfully stored value
// (PrevMeterValue), i.e. storing v would only duplicate that entry.
bool MeterValue_unchangedSincePrev(const MeterValue &v)
{
  return PrevMeterValue.timestamp > 0
      && v.meter_value_180 == PrevMeterValue.meter_value_180
      && v.meter_value_280 == PrevMeterValue.meter_value_280
      && v.solar           == PrevMeterValue.solar;
}

// ---------------------------------------------------------------------------
// MeterValue_store  (Written by Claude)
// Stores LastMeterValue into the packed ring-buffer.
//
// Ring-buffer strategy (unchanged from original):
//   override=true  (TAF7,  high-priority): writes ascending from index 0 upward.
//   override=false (TAF14, low-priority):  writes descending from the last index.
// This ensures TAF7 values always have space and are never overwritten by TAF14.
//
// The only structural change vs. the original: instead of assigning a struct
// slot directly (MeterValues[i] = LastMeterValue), we call MeterValue_write()
// which packs only the enabled fields into the byte buffer.
// Bugfix: the original non-override wrap-around check tested meter_value_override_i
// instead of meter_value_NON_override_i — corrected here.
// ---------------------------------------------------------------------------
bool MeterValue_store(bool override)
{
  if (ESP.getFreeHeap() < 1000) { Log_Add(1015); DLOGLN("Not enough free heap to store another value"); return false; }

  if (mystrom_PV_object.isChecked()) myStrom_get_Meter_value();

  // Snapshot LastMeterValue once, after the blocking myStrom call, and use only
  // the snapshot below. Copied under the lock, so it never mixes two telegrams
  // (telegramTask may replace LastMeterValue on the other core meanwhile).
  MeterValue snap = MeterValue_getLast();

  // Without a 1.8.0 value the trigger stays set and the store is retried on
  // every pass, so 1200 was logged several times a minute (repeats are only
  // suppressed while no other entry comes in between). Log it at most every
  // 30 min (like 3005/3007), so it still reaches the backend without filling
  // the log ring and uploading the log with every backend call. A valid value
  // restarts the cycle.
  static unsigned long last_zero_log = 0;
  if (snap.meter_value_180 <= 0)
  {
    if (last_zero_log == 0 || millis() - last_zero_log >= 1800000UL)
    {
      Log_Add(1200);
      last_zero_log = millis();
    }
    return false;
  }
  last_zero_log = 0;

  // The telegram may have been parsed before the time sync even if the store
  // runs after it. Refuse it; the trigger stays set and the next telegram
  // (seconds later) carries a valid timestamp.
  if (!Time_isPlausible(snap.timestamp)) { Log_Add(1027); return false; }

  // Skip storing if the value has not changed since the last successful store.
  if (MeterValue_unchangedSincePrev(snap))
  {
    // Timestamp unchanged = no new telegram received (e.g. meter reader slipped off).
    // Never re-store a frozen reading regardless of elapsed time — this prevents
    // an infinite loop where TAF7 keeps re-queuing the same stale entry after the
    // backend acknowledges it and the buffer is cleared.
    if (snap.timestamp == PrevMeterValue.timestamp)
    {
      Log_AddWithoutTransmit(1201);
      return false;
    }
    // New telegram received but counter value unchanged. Override stores (TAF7,
    // boot snapshot, manual TAF6) store it anyway, right away, so every TAF7
    // mark gets its own entry. TAF14 stores it again only after 15 min as an
    // "alive" heartbeat.
    if (override == false && millis() - last_meter_value_successful < 900000)
    {
      Log_AddWithoutTransmit(1201);
      return false;
    }
  }

  // Select write index based on priority:
  //   override (TAF7)      -> ascending from front
  //   non-override (TAF14) -> descending from back
  int write_i = override ? meter_value_override_i : meter_value_NON_override_i;
  DLOGLN("where to write: " + String(write_i));

  // Check if the target slot is still empty — empty means free space remains
  meter_value_buffer_full = !MeterValue_slot_empty(write_i);

  if (override == true || meter_value_buffer_full == false)
  {
    // Always the real telegram time, also for TAF7: flooring it to the TAF7
    // mark would trade real precision for a cosmetically clean timestamp.
    uint32_t storeTimestamp = snap.timestamp;

    // Write the current reading into the packed buffer at the selected slot.
    // Fields that are disabled (temperature, solar, obis280) are silently
    // skipped inside MeterValue_write() — they consume no bytes.
    MeterValue_write(write_i,
      storeTimestamp,
      snap.meter_value_180,
      snap.temperature,
      snap.solar,
      snap.meter_value_280
    );

    if (override)
    {
      // TAF7: advance write pointer upward; wrap around on overflow.
      // Intentionally overwrites TAF14 data when the buffer is full —
      // TAF7 (timed snapshots) has higher priority than TAF14 (interval readings).
      meter_value_override_i++;
      if (meter_value_override_i >= Meter_Value_Buffer_Size)
      {
        meter_value_override_i = 0;
        meter_value_buffer_overflow = true;
      }
    }
    else
    {
      // TAF14: advance write pointer downward; wrap around on underflow.
      // Bugfix: original code checked meter_value_override_i here by mistake.
      meter_value_NON_override_i--;
      if (meter_value_NON_override_i < 0)
      {
        meter_value_NON_override_i = Meter_Value_Buffer_Size - 1;
        meter_value_buffer_overflow = true;
      }
    }
  }
  else
  {
    Log_Add(1016);
    MeterValue_trigger_non_override = false; // prevent immediate re-trigger
    DLOGLN("Buffer full, no space to write new value!");
    return false;
  }

  PrevMeterValue = snap; // remember last stored value for change detection
  return true;
}

unsigned long last_meter_value_store   = 0;
unsigned long last_meter_value_trigger = 0;

void handle_MeterValue_store()
{
  if (!MeterValue_trigger_override && !MeterValue_trigger_non_override) return; // nothing to do
  if (millis() - last_meter_value_store < 1000) return;
  last_meter_value_store = millis();

  // Upload in flight: defer instead of writing into a buffer that is about to
  // be cleared. The trigger stays set, so the store is retried in ~1 s.
  if (xSemaphoreTake(Sema_MeterBuffer, 0) != pdTRUE) { Log_AddWithoutTransmit(1026); return; }

  bool retVal = false;
  if (MeterValue_trigger_override == true)
  {
    // Remember the entry stored before this one: if it is a TAF14 reading taken
    // less than TAF7_REPLACE_WINDOW_S before the TAF7 value at the mark, it is removed
    // below so the TAF7 mark isn't preceded by a near-duplicate.
    static const uint32_t TAF7_REPLACE_WINDOW_S = 10;
    uint32_t prevTs       = PrevMeterValue.timestamp;
    bool     prevWasTaf14 = !last_store_was_override && prevTs > 0;

    retVal = MeterValue_store(true);
    if (retVal == true)
    {
      last_taf7_meter_value   = millis();
      last_store_was_override = true;

      // Runs only after a successful store, i.e. at most once per TAF7 mark —
      // a refused store that is retried can never remove further entries.
      // The slot is checked against the remembered timestamp, so nothing else
      // is removed if the buffer was uploaded/cleared or wrapped in between.
      uint32_t newTs = PrevMeterValue.timestamp;
      int      lastTaf14_i = meter_value_NON_override_i + 1;
      if (MeterValue_trigger_is_taf7 && prevWasTaf14 &&
          newTs >= prevTs && newTs - prevTs < TAF7_REPLACE_WINDOW_S &&
          lastTaf14_i < Meter_Value_Buffer_Size)
      {
        uint32_t ts, m180, solar, m280;
        int32_t  temp;
        MeterValue_read(lastTaf14_i, ts, m180, temp, solar, m280);
        if (ts == prevTs)
        {
          MeterValue_ClearSlot(lastTaf14_i);
          meter_value_NON_override_i = lastTaf14_i;
          Log_AddWithoutTransmit(1025);
        }
      }
    }
    // Refused although nothing changed since the last stored value — only
    // possible when no new telegram arrived since then (frozen reading). That
    // value already is the reading at this moment, so drop the trigger instead
    // of retrying and storing it late once a telegram comes in.
    else if (MeterValue_unchangedSincePrev(LastMeterValue)) MeterValue_trigger_override = false;
    if (!MeterValue_trigger_override || retVal) MeterValue_trigger_is_taf7 = false;
  }
  else if (MeterValue_trigger_non_override == true)
  {
    retVal = MeterValue_store(false);
    if (retVal == true) { last_taf14_meter_value = millis(); last_store_was_override = false; }
  }
  xSemaphoreGive(Sema_MeterBuffer);

  if (retVal == true)
  {
    Log_AddWithoutTransmit(1017);
    last_taf14_meter_value      = millis();
    last_meter_value_successful = millis();
    MeterValue_trigger_override     = false;
    MeterValue_trigger_non_override = false;
  }
}

void handle_MeterValue_trigger()
{
  // No TAF triggers before the system time is valid (NTP or backend Date
  // header, see time_utils): readings would carry
  // 1970 timestamps (rejected by the backend anyway), the TAF7 marks would be
  // meaningless, and a trigger left pending would later store a "TAF7 mark" value
  // at an arbitrary time. The boot snapshot below takes the first value once
  // the time is valid.
  if (!Time_isSynced()) return;

  // Boot snapshot: fire once as soon as the first telegram has been received.
  if (!boot_snapshot_done && startup_print_done)
  {
    boot_snapshot_done              = true;
    Log_AddWithoutTransmit(1024);
    MeterValue_trigger_override     = true;
    MeterValue_trigger_non_override = false;
    MeterValue_trigger_is_taf7      = false;
    return;
  }

  // True during the first 15s after a TAF7 mark (e.g. HH:00/:15/:30/:45).
  // TAF14 is held back for the whole window (see below) so it cannot land a few
  // seconds after the TAF7 snapshot; a TAF14 reading right before the mark is
  // removed once the TAF7 value is stored (see handle_MeterValue_store()).
  bool taf7WindowOpen = taf7_b_object.isChecked() &&
                         ((Time_getEpochTime() - 1) % ((unsigned long)cached_taf7_param * 60) < 15);

  if (MeterValue_trigger_override == false &&
      taf7WindowOpen &&
      (millis() - last_taf7_meter_value > 45000))
  {
    // TAF7 always stores its own value at the mark. A TAF14 reading
    // stored less than 10 s before it is removed after the TAF7 store succeeded
    // (see handle_MeterValue_store()).
    Log_AddWithoutTransmit(1010);
    last_taf7_meter_value = millis(); // one TAF7 trigger per TAF7 mark
    MeterValue_trigger_override     = true;
    MeterValue_trigger_non_override = false;
    MeterValue_trigger_is_taf7      = true;
  }
  else if (MeterValue_trigger_override == false &&
           MeterValue_trigger_non_override == false &&
           taf14_b_object.isChecked() &&
           !taf7WindowOpen &&
           millis() - last_meter_value_successful >= 1000UL * (unsigned long)cached_taf14_param &&
           millis() - last_taf14_meter_value      >= 1000UL * (unsigned long)cached_taf14_param)
  {
    if (meter_value_buffer_full == true) { last_taf14_meter_value = millis(); Log_Add(1206); }
    else { Log_AddWithoutTransmit(1011); MeterValue_trigger_non_override = true; }
  }
}
