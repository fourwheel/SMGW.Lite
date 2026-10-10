#include "time_utils.h"
#include "log_buffer.h"
#include <time.h>
#include <sys/time.h>
#if defined(ESP32)
#include "esp_timer.h"
#include "esp_sntp.h"
#endif

// ---------------------------------------------------------------------------
// Time source setup
// NTP servers from three different operators (lwIP supports at most 3), so a
// single operator outage cannot leave the device without time. If UDP/123 is
// blocked entirely, Time_setFromHttpDate() takes the time from the backend's
// HTTPS response instead.
// ---------------------------------------------------------------------------
static volatile bool s_ntp_synced = false;

static void Time_onNtpSync(struct timeval *tv)
{
  if (!s_ntp_synced) Log_AddWithoutTransmit(1028); // log the first sync only, not every 3 h resync
  s_ntp_synced = true;
}

void Time_begin()
{
  sntp_set_time_sync_notification_cb(Time_onNtpSync);
#ifdef TEST_NTP_BLOCKED
  // Test only: simulate a network where NTP (UDP/123) is blocked, to exercise
  // the backend Date-header fallback (expect log 1029 -> 1024, no 1028).
  //   PowerShell: $env:PLATFORMIO_BUILD_FLAGS="-DTEST_NTP_BLOCKED"; pio run -e esp32-nodemcu -t upload
  //               Remove-Item Env:PLATFORMIO_BUILD_FLAGS
  // Then power-cycle the device: a software reset (incl. OTA) keeps the system time.
  configTime(0, 0, "ntp.invalid", "ntp.invalid", "ntp.invalid");
#else
  configTime(0, 0, "ptbtime1.ptb.de", "de.pool.ntp.org", "time.cloudflare.com");
#endif
}

// Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's algorithm).
// Avoids timegm()/mktime(), whose result depends on the TZ setting.
static long Time_daysFromCivil(int y, unsigned m, unsigned d)
{
  y -= m <= 2;
  const long     era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

bool Time_setFromHttpDate(const String &line)
{
  if (s_ntp_synced) return false;   // NTP is the preferred, more precise source
  if (!line.startsWith("Date:")) return false;

  // RFC 7231 IMF-fixdate, e.g. "Date: Sun, 27 Sep 2026 10:15:30 GMT"
  int  day, year, hh, mm, ss;
  char mon[4] = {0};
  if (sscanf(line.c_str(), "Date: %*3s, %d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) != 6) return false;

  static const char *MONTHS = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char *p = strstr(MONTHS, mon);
  if (!p || mon[0] == '\0' || (p - MONTHS) % 3 != 0) return false;
  unsigned month = (unsigned)((p - MONTHS) / 3) + 1;

  unsigned long epoch = (unsigned long)Time_daysFromCivil(year, month, day) * 86400UL
                      + (unsigned long)hh * 3600UL + (unsigned long)mm * 60UL + (unsigned long)ss;
  if (!Time_isPlausible(epoch)) return false;

  // Only correct a real deviation — the header has 1 s resolution, and
  // stepping the clock back and forth on every response would jitter timestamps.
  long delta = (long)epoch - (long)Time_getEpochTime();
  if (delta >= -2 && delta <= 2) return false;

  struct timeval tv = { (time_t)epoch, 0 };
  settimeofday(&tv, nullptr);
  Log_AddWithoutTransmit(1029);
  return true;
}

unsigned long Time_getEpochTime()
{
  return static_cast<unsigned long>(time(nullptr));
}

// Minimum plausible epoch for a reliable NTP sync (2020-01-01 00:00:00 UTC).
// Without a sync the ESP32 counts from 1970 after a power loss; a software
// reset keeps the system time, so it is valid immediately after OTA/restart.
static const unsigned long EPOCH_MIN_PLAUSIBLE = 1577836800UL;

bool Time_isPlausible(unsigned long epoch)
{
  return epoch > EPOCH_MIN_PLAUSIBLE;
}

bool Time_isSynced()
{
  return Time_isPlausible(Time_getEpochTime());
}

int Time_getMinutes()
{
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  return timeinfo.tm_min;
}

String Time_getFormattedTime()
{
  time_t now = time(nullptr);
  char timeStr[64];
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
  return String(timeStr);
}

String Time_formatTimestamp(unsigned long timestamp)
{
  time_t rawTime = static_cast<time_t>(timestamp);
  struct tm timeinfo;
  localtime_r(&rawTime, &timeinfo);
  char buffer[20];
  strftime(buffer, sizeof(buffer), "%D %H:%M:%S", &timeinfo);
  return String(buffer);
}

String Time_formatUptime()
{
  int64_t uptimeMicros  = esp_timer_get_time();
  int64_t uptimeMillis  = uptimeMicros / 1000;
  int64_t uptimeSeconds = uptimeMillis / 1000;

  int days    = (int)(uptimeSeconds / 86400); uptimeSeconds %= 86400;
  int hours   = (int)(uptimeSeconds / 3600);  uptimeSeconds %= 3600;
  int minutes = (int)(uptimeSeconds / 60);
  int seconds = (int)(uptimeSeconds % 60);

  char buffer[20];
  sprintf(buffer, "%02dd %02dh%02dm%02ds", days, hours, minutes, seconds);
  return String(buffer);
}
