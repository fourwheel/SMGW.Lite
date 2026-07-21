#include "time_utils.h"
#include <time.h>
#if defined(ESP32)
#include "esp_timer.h"
#endif

unsigned long Time_getEpochTime()
{
  return static_cast<unsigned long>(time(nullptr));
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
