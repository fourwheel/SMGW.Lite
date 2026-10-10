// WiFi supervision, backend recovery and the self-recovery supervisor task.
#include "supervisor.h"
#include "app_globals.h"
#include "webserver_main.h"
#include "log_buffer.h"
#include "debug_log.h"
#include "time_utils.h"
#include <Preferences.h>
#include <ArduinoOTA.h>

unsigned long last_wifi_check;
unsigned long last_reconnect_attempt      = 0;

// ---------------------------------------------------------------------------
// handle_wifi_setup_lifecycle — owns both ends of an in-progress /wifiSetup
// connection attempt: dropping the AP a couple of seconds after a confirmed
// success, and aborting a failed attempt from the main loop rather than only
// when the browser happens to poll /wifiStatus.
//
// ESP32 has a single radio shared between AP and STA, which must run on the
// same channel. While the STA is actively negotiating (and, with a wrong
// password, repeatedly retrying the auth handshake), that radio time is
// taken away from the softAP, which is exactly the AP the user's browser is
// connected to — so the very request needed to detect and abort the failed
// attempt can itself be delayed or dropped. Running the check unconditionally
// in the main loop means the retrying STA connection gets aborted as soon as
// possible, regardless of whether the config portal is currently reachable.
// ---------------------------------------------------------------------------
void handle_wifi_setup_lifecycle()
{
  if (g_apStopAt > 0 && millis() >= g_apStopAt)
  {
    g_apStopAt = 0;
    DLOGLN("WiFi-Setup: AP hold time elapsed, handing over to IotWebConf.");
    iotWebConf.forceApMode(false); // _forceApMode was true -> triggers changeState(Connecting)
  }

  static unsigned long failSince = 0;

  if (!g_wifiSetupPending) { failSince = 0; return; }

  wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED)
  {
    failSince = 0;
    Webserver_CheckWifiSetupFallback(); // handled by /wifiStatus normally; this is the fallback if the client never polls again
    return;
  }

  bool definitiveFailure = status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL;
  if (definitiveFailure) {
    if (failSince == 0) failSince = millis();
  } else {
    failSince = 0; // status recovered (e.g. back to idle/disconnected mid-handshake) - don't act on a blip
  }

  // ESP32 can report WL_CONNECT_FAILED transiently for a moment during a
  // handshake that still goes on to succeed - require the failure to persist
  // for a bit before treating it as final, so a real connection isn't killed
  // by this loop-driven watchdog on its very first bad reading.
  bool failureConfirmed = definitiveFailure && (millis() - failSince > 3000);
  bool timedOut         = millis() - g_wifiSetupStartedAt > 20000;
  if (!failureConfirmed && !timedOut) return;

  failSince           = 0;
  g_wifiSetupPending = false;
  g_wifiSetupFailed  = true;
  WiFi.disconnect();
  Log_Add(7002);
  DLOGLN("WiFi-Setup: connection attempt failed, freeing radio for AP.");
  Webserver_ClearPendingWifiCredentials();
}

void handle_check_wifi_connection()
{
  wl_status_t current_wifi_status = WiFi.status();
  if (DebugSetOffline_object.isChecked()) current_wifi_status = WL_CONNECTION_LOST;

  if (millis() - last_wifi_check > 500)
  {
    last_wifi_check = millis();

    if (current_wifi_status == WL_CONNECTED && wifi_connected)
    {
      // Still connected — nothing to do
    }
    else if (current_wifi_status == WL_CONNECTED && !wifi_connected)
    {
      Log_Add(1008);
      DLOGLN("Connection has returned: Resetting Backend Timer, starting OTA");
      ArduinoOTA.begin();
      wifi_connected         = true;
      call_backend_successfull = false;
      b_send_log_to_backend  = true; // sent with the next backend call
      IPAddress localIP = WiFi.localIP();
      IPlastOctet = localIP[3];
    }
    else if (current_wifi_status != WL_CONNECTED && wifi_connected)
    {
      Log_Add(1009);
      wifi_connected = false;
    }
    else
    {
      // Still offline — periodically trigger reconnect attempt
      if (millis() - last_reconnect_attempt > 60000)
      {
        last_reconnect_attempt = millis();
        // Skip while a /wifiSetup attempt is in flight: it manages
        // forceApMode() itself, and IotWebConf::forceApMode(false) clears
        // the internal _forceApMode flag even when it can't leave ApMode
        // right away (it just logs and stays put) - if that flag is cleared
        // out from under us, IotWebConf's own AP timeout can tear the AP
        // down entirely (WiFi.mode(WIFI_STA)) once _apTimeoutMs elapses,
        // right in the middle of our own connection attempt.
        if (!g_wifiSetupPending)
        {
          iotwebconf::NetworkState state = iotWebConf.getState();
          if (state == iotwebconf::NetworkState::ApMode)
          {
            DLOGLN("AP mode: triggering reconnect via IotWebConf");
            iotWebConf.forceApMode(false);
          }
          // In Connecting state: let IotWebConf manage its own reconnect cycle.
          // Calling WiFi.begin() here would reset an in-progress association
          // attempt, making reconnection harder (especially with hidden SSIDs).
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// handle_backend_recovery
// Reconnects WiFi if no backend call has been acknowledged for 30 min while
// WiFi reports connected — covers a link that is up but passes no traffic.
// IotWebConf notices the disconnect and connects again. Unlike a restart this
// keeps the RAM buffer, so a backend outage costs no data: the reconnect just
// repeats until the backend answers again (log 7000).
// With a long backend interval the limit grows to two intervals + 5 min, so a
// regular gap between two calls never counts as "no success". The calls run
// at minutes divisible by the interval, so the longest gap is 60 min.
// ---------------------------------------------------------------------------
void handle_backend_recovery()
{
  static unsigned long last_reconnect = 0;

  if (!wifi_connected || g_wifiSetupPending || backend_host.isEmpty()) return;

  unsigned long interval_min = (unsigned long)min(cached_backend_call_minute, 60);
  unsigned long limit_ms     = max(30UL, 2 * interval_min + 5) * 60UL * 1000UL;
  if (millis() - last_backend_success < limit_ms) return;
  if (last_reconnect != 0 && millis() - last_reconnect < limit_ms) return;

  last_reconnect = millis();
  Log_Add(7000);
  DLOGLN("No acknowledged backend call within the limit - reconnecting WiFi");
  WiFi.disconnect();
}

// ---------------------------------------------------------------------------
// supervisorTask
// Restarts the device if it is stuck without rebooting by itself:
//  - loop() has not started a new pass for 10 min (hung main loop), or
//  - a meter value or log upload task has held Sema_Backend for 10 min; while
//    it hangs, no backend call gets through anymore.
// Longest regular durations (all limits set in code), well below 10 min:
//  - meter upload: DNS 15 s + connect 10 s + TLS handshake 30 s + payload in
//    1 KB writes of max. 10 s each (16 KB: 160 s, only on a nearly dead link)
//    + response 15 s = ~4 min worst case, normally a few seconds
//  - log upload: ~1 min (handshake 10 s, one write)
//  - loop(): OTA pull up to ~7 min (Sema_Backend 30 s, DNS 15 s, connect
//    15 s, handshake 30 s, request/headers 75 s, download capped at 4 min);
//    meter upload after a config change or espota update (Sema_Backend 15 s
//    + upload as above) ~4.5 min; Check Remote FW Update page and each
//    post-update validation attempt ~3.5 min; connection test page ~1 min;
//    myStrom fetch a few seconds; optical flash test 20 s
// The cause is kept in NVS across the restart and logged on boot (1120-1122).
// During a post-update validation (OtaPull_validate()) any restart makes the
// bootloader boot the previous firmware (6028).
// Values still in the RAM buffer are lost, but a stuck device would not
// deliver them either.
// ---------------------------------------------------------------------------
static void Supervisor_restart(int cause)
{
  // Fallback: restart after 5 s even if the NVS write below never returns
  // (e.g. the hung code holds the NVS lock). esp_timer callbacks run in their
  // own high-priority task, independent of that lock.
  esp_timer_create_args_t args = {};
  args.callback = [](void *) { esp_restart(); };
  args.name     = "recovery_restart";
  esp_timer_handle_t fallback = nullptr;
  if (esp_timer_create(&args, &fallback) == ESP_OK) esp_timer_start_once(fallback, 5 * 1000000ULL);

  Preferences prefs;
  prefs.begin("recovery", false);
  prefs.putInt("cause", cause);
  prefs.end();
  esp_restart();
}

void supervisorTask(void *pvParameters)
{
#ifdef TEST_SELF_RECOVERY
  static const unsigned long LIMIT_MS = 60UL * 1000; // test build: 1 min instead of 10
#else
  static const unsigned long LIMIT_MS = 10UL * 60 * 1000;
#endif
  for (;;)
  {
    vTaskDelay(pdMS_TO_TICKS(10000));
    // Read the timestamps before millis(): read the other way round, a value
    // updated in between would be newer than "now" and underflow the difference.
    unsigned long loop_hb       = g_loop_heartbeat;
    unsigned long meter_started = g_meter_task_started;
    unsigned long log_started   = g_log_task_started;
    unsigned long now           = millis();

    if (now - loop_hb > LIMIT_MS)                             Supervisor_restart(1120);
    if (meter_started != 0 && now - meter_started > LIMIT_MS) Supervisor_restart(1121);
    if (log_started   != 0 && now - log_started   > LIMIT_MS) Supervisor_restart(1122);
  }
}
