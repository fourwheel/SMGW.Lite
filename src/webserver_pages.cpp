// Dashboard (/) and system info (/sysinfo) pages.
#include "webserver_pages.h"
#include "app_globals.h"
#include "html_style.h"
#include "log_buffer.h"
#include "debug_log.h"
#include "time_utils.h"
#include "serial_scan.h"
#include "meter_value.h"
#include "ota_pull.h"
#include "version.h"
#include "telegram.h"

void Webserver_HandleSysInfo()
{
  String s;
  s.reserve(9000);
  s += R"rawliteral(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>SmartMeterLite – Details</title>)rawliteral";
  s += HTML_STYLE_MODERN;
  s += R"rawliteral(</head>
<body>
<div class="logo">&#9889; SmartMeterLite</div>
<a class="back" href="/">&#8592; Home</a>

<div class="cfg-grid">
<a class="cfg-link" href="config" style="max-width:none;">
<span class="cfg-icon">&#9881;</span>
<span class="cfg-text">
<strong>Systemparameter</strong>
</span>
</a>

<a class="cfg-link" href="/wifiScan" style="max-width:none;">
<span class="cfg-icon">&#128246;</span>
<span class="cfg-text">
<strong>WLAN-Netzwerke</strong>
</span>
</a>

<a class="cfg-link" href="PinAssistant" style="max-width:none;">
<span class="cfg-icon">&#128274;</span>
<span class="cfg-text">
<strong>PIN Assistant</strong>
</span>
</a>

<a class="cfg-link" href="PinAssistantDeluxe" style="max-width:none;">
<span class="cfg-icon">&#128274;</span>
<span class="cfg-text">
<strong>PIN Assistant Deluxe</strong>
</span>
</a>
</div>

<div class="card">
<div class="card-title">Last Meter Value <small style="font-weight:400;color:#888;">&mdash; green = in wire format</small></div>
<div class="tbl"><table>)rawliteral";
  {
    const char* W = " style='background:#d4edda'";
    const char* N = "";
    const char* w280  = config_obis280_enabled     ? W : N;
    const char* wTemp = config_temperature_enabled ? W : N;
    const char* wSol  = config_solar_enabled        ? W : N;
    s += "<tr><th>Time</th><th>1.8.0</th><th>2.8.0</th><th>Temp</th><th>MyStrom</th></tr>";
    s += String("<tr><td") + W    + ">" + String(Time_getEpochTime() - LastMeterValue.timestamp) + "s ago</td>"
       + "<td" + W    + ">" + String(LastMeterValue.meter_value_180) + "</td>"
       + "<td" + w280  + ">" + String(LastMeterValue.meter_value_280) + "</td>"
       + "<td" + wTemp + ">" + String(LastMeterValue.temperature / 100.0) + "\xc2\xb0""C</td>"
       + "<td" + wSol  + ">" + String(LastMeterValue.solar) + "</td></tr>";
  }
  s += R"rawliteral(</table></div>
<div class="btns" style="margin-top:.6rem;">
<a class="btn btn-s" href="StoreMeterValue">Store Meter Value Now (TAF6)</a>
<a class="btn btn-s" href="showLastMeterValue">Last Value (JSON)</a>
</div>
</div>

<div class="card">
<div class="card-title">Meter Value Buffer</div>
<div class="kv"><span class="kl">Used / Size</span>)rawliteral";
  s += String(MeterValue_Num()) + " / " + String(Meter_Value_Buffer_Size);
  {
    int cfgKB = atoi(Meter_Value_Buffer_Size_Char);
    s += meter_value_buffer_is_auto
         ? " <small>(auto &mdash; " + String(BUFFER_REFERENCE_BYTES / 1024) + " KB reference budget)</small>"
         : " <small>(manual &mdash; " + String(cfgKB) + " KB budget)</small>";
  }
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Budget</span>)rawliteral";
  {
    int cfgKB = atoi(Meter_Value_Buffer_Size_Char);
    size_t budget = (cfgKB <= 0) ? BUFFER_REFERENCE_BYTES : (size_t)cfgKB * 1024;
    s += String(budget) + " bytes &nbsp;|&nbsp; " + String(MeterValue_EntrySize()) + " bytes/slot &nbsp;&rarr;&nbsp; " + String(MeterValue_slots_from_budget(budget)) + " slots";
  }
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Capacity (free heap)</span>)rawliteral";
  {
    int maxSlots = MeterValue_calc_max_slots_for_display();
    int maxMinutes_taf7  = (maxSlots > 0 && atoi(taf7_param)  > 0) ? (maxSlots * atoi(taf7_param))  : 0;
    int maxMinutes_taf14 = (maxSlots > 0 && atoi(taf14_param) > 0) ? (maxSlots * atoi(taf14_param) / 60) : 0;
    s += String(maxSlots) + " slots";
    { int d = maxMinutes_taf7 / 1440, h = (maxMinutes_taf7 % 1440) / 60, m = maxMinutes_taf7 % 60;
      s += " &nbsp;|&nbsp; TAF7: ~";
      if (d > 0) s += String(d) + "d";
      s += String(h) + "h" + String(m) + "min"; }
    { int d = maxMinutes_taf14 / 1440, h = (maxMinutes_taf14 % 1440) / 60, m = maxMinutes_taf14 % 60;
      s += " &nbsp;|&nbsp; TAF14: ~";
      if (d > 0) s += String(d) + "d";
      s += String(h) + "h" + String(m) + "min"; }
  }
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Wire format</span>)rawliteral";
  s += MeterValue_BuildFieldsParam();
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Entry size</span>)rawliteral";
  s += String(MeterValue_EntrySize()) + " bytes";
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Free heap</span>)rawliteral";
  s += String(ESP.getFreeHeap() / 1024) + " KB <small>(buffer uses " + String((size_t)Meter_Value_Buffer_Size * MeterValue_EntrySize() / 1024) + " KB)</small>";
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Temperature</span>)rawliteral";
  s += String(config_temperature_enabled ? "yes" : "no");
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">MyStrom / Solar</span>)rawliteral";
  s += String(config_solar_enabled ? "yes" : "no");
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Infeed (2.8.0)</span>)rawliteral";
  s += String(config_obis280_enabled ? "yes" : "no");
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">i override / non-override</span>)rawliteral";
  s += String(meter_value_override_i) + " / " + String(meter_value_NON_override_i);
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Buffer Overflow</span>)rawliteral";
  s += String(meter_value_buffer_overflow);
  s += R"rawliteral(</div>
<div class="kv last"><span class="kl">Buffer Full</span>)rawliteral";
  s += String(meter_value_buffer_full);
  s += R"rawliteral(</div>
<div class="btns" style="margin-top:.6rem;">
<a class="btn btn-s" href="showMeterValues">Show Meter Values</a>
<a class="btn btn-s" href="MeterValue_Num2">Count (alternative)</a>
</div>
</div>

<div class="card">
<div class="card-title">Telegram Parse Config</div>
<div class="kv"><span class="kl">Protocol (auto-detected)</span>)rawliteral";
  s += Telegram_protocol_to_string(last_detected_protocol);
  s += R"rawliteral(</div>
<div class="kv last"><span class="kl">Serial config (active)</span>)rawliteral";
  s += SerialScan_activeLabel();
  s += R"rawliteral(</div>
<div class="btns" style="margin-top:.6rem;">
<a class="btn btn-s" href="showTelegram">Show Telegram</a>
<a class="btn btn-s" href="showTelegramRaw">Raw</a>
<a class="btn btn-s" href="showTelegramAnalysis">Analysis</a>
<a class="btn btn-s" href="/serialScan">Baud/Parity Scan</a>
</div>
</div>

<div class="card">
<div class="card-title">Backend Config</div>
<div class="kv"><span class="kl e">Backend Endpoint</span>)rawliteral";
  s += backend_endpoint;
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Host</span>)rawliteral";
  s += backend_host;
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Path</span>)rawliteral";
  s += backend_path;
  s += R"rawliteral(</div>
<div class="kv"><span class="kl e">Call Minute</span>)rawliteral";
  s += String(atoi(backend_call_minute));
  s += R"rawliteral(</div>
<div class="kv"><span class="kl e">Backend ID</span>)rawliteral";
  s += backend_ID;
  s += R"rawliteral(</div>
<div class="kv"><span class="kl e">Use SSL Cert</span>)rawliteral";
  s += (UseSslCert_object.isChecked() ? "true" : "false");
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Last Call Ago</span>)rawliteral";
  s += String((millis() - last_call_backend) / 60000) + " min";
  s += R"rawliteral(</div>
<div class="kv last"><span class="kl">Static Delay</span>)rawliteral";
  s += String(staticDelay) + " s";
  s += R"rawliteral(</div>
<div class="btns" style="margin-top:.6rem;">
<a class="btn btn-s" href="setCert">Set Cert</a>
<a class="btn btn-s" href="testBackendConnection">Test Connection</a>
<a class="btn btn-s" href="sendLog_Task">Send Log</a>
<a class="btn btn-s" href="sendMeterValues_Task">Send Meter Values</a>
<a class="btn btn-s" href="sendboth_Task">Send Both</a>
</div>
</div>

<div class="card">
<div class="card-title">TAF Config</div>
<div class="kv"><span class="kl e">TAF 7</span>)rawliteral";
  s += (taf7_b_object.isChecked() ? "activated" : "not activated");
  s += R"rawliteral(</div>
<div class="kv"><span class="kl e">TAF 7 Minute</span>)rawliteral";
  s += String(atoi(taf7_param));
  s += R"rawliteral(</div>
<div class="kv"><span class="kl e">TAF 14</span>)rawliteral";
  s += (taf14_b_object.isChecked() ? "activated" : "not activated");
  s += R"rawliteral(</div>
<div class="kv"><span class="kl e">TAF 14 Interval</span>)rawliteral";
  s += String(atoi(taf14_param)) + " s";
  s += R"rawliteral(</div>)rawliteral";
  s += R"rawliteral(
</div>
<div class="card">
<div class="card-title">Additional Meters &amp; Sensors</div>
<div class="kv"><span class="kl e">Temperature Sensor</span>)rawliteral";
  s += (temperature_object.isChecked() ? "activated" : "deactivated");
  s += R"rawliteral(</div>
<div class="kv"><span class="kl e">MyStrom (solar)</span>)rawliteral";
  s += (mystrom_PV_object.isChecked() ? "activated" : "deactivated");
  s += R"rawliteral(</div>
<div class="kv last"><span class="kl e">MyStrom IP</span>)rawliteral";
  s += mystrom_PV_IP;
  s += R"rawliteral(</div>
</div>

<div class="card">
<div class="card-title">Helpers</div>
<div class="kv"><span class="kl e">Set Device Offline</span>)rawliteral";
  s += (DebugSetOffline_object.isChecked() ? "activated" : "deactivated");
  s += R"rawliteral(</div>
<div class="kv"><span class="kl e">Values from other SMGWLite</span>)rawliteral";
  s += (DebugFromOtherClient_object.isChecked() ? "activated" : "deactivated");
  s += R"rawliteral(</div>
<div class="kv last"><span class="kl e">Remote Client IP</span>)rawliteral";
  s += String(DebugMeterValueFromOtherClientIP);
  s += R"rawliteral(</div>
</div>

<div class="card">
<div class="card-title">System Info</div>
<div class="kv"><span class="kl">Meter Model</span>)rawliteral";
  s += meter_model.isEmpty() ? "unknown" : meter_model;
  s += R"rawliteral(</div>
<div class="kv"><span class="kl e">LED Blink</span>)rawliteral";
  s += (led_blink_object.isChecked() ? "activated" : "deactivated");
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Watermark Main</span>)rawliteral";
  s += String(uxTaskGetStackHighWaterMark(NULL));
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Watermark Meter Values</span>)rawliteral";
  s += String(watermark_meter_buffer);
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Watermark Logs</span>)rawliteral";
  s += String(watermark_log_buffer);
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Watermark Telegram</span>)rawliteral";
  s += String(watermark_telegram);
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Uptime</span>)rawliteral";
  s += Time_formatUptime();
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Reset Reason</span>)rawliteral";
  s += Log_get_reset_reason();
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">System Time (UTC)</span>)rawliteral";
  s += String(Time_getFormattedTime()) + " / " + String(Time_getEpochTime());
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Firmware Version</span>)rawliteral";
  s += String(FIRMWARE_VERSION);
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Config Version</span>)rawliteral";
  s += String(CONFIG_VERSION);
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Build Time</span>)rawliteral";
  s += BUILD_TIMESTAMP;
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Build Branch</span>)rawliteral";
  s += BUILD_BRANCH;
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Free Heap</span>)rawliteral";
  s += String(ESP.getFreeHeap());
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">Chip Temperature</span>)rawliteral";
  s += String(temperatureRead(), 1) + " &deg;C";
  s += R"rawliteral(</div>
<div class="kv"><span class="kl">WiFi RSSI</span>)rawliteral";
  {
    int rssi = WiFi.RSSI();
    s += String(rssi) + " dBm";
    if      (rssi >= -60) s += " (good)";
    else if (rssi >= -75) s += " (ok)";
    else                  s += " (weak)";
  }
  s += R"rawliteral(</div>
<div class="kv last"><span class="kl">Log Buffer (max)</span>)rawliteral";
  s += String(LOG_BUFFER_SIZE);
  s += R"rawliteral(</div>
<div class="btns" style="margin-top:.6rem;">
<a class="btn btn-s" href="update">Upload FW</a>
<a class="btn btn-s" href="checkRemoteFwUpdate">Check Remote FW Update</a>
<a class="btn btn-d" href="restart">Restart</a>
</div>
</div>

<div class="card">
<div class="card-title">Log Buffer <small style="font-weight:400;color:#888;">(last 10 / index )rawliteral";
  s += String(Log_getIndex());
  s += R"rawliteral()</small></div>
<div class="tbl">)rawliteral";
  s += Log_BufferToString(10);
  s += R"rawliteral(</div>)rawliteral";
  s += R"rawliteral(
<div class="btns" style="margin-top:.6rem;">
<a class="btn btn-s" href="showLogBuffer">Show Full Log</a>
<a class="btn btn-s" href="resetLogBuffer">Reset Log</a>
</div>
</div>

</body></html>)rawliteral";

  server.send(200, "text/html", s);
}

// ---------------------------------------------------------------------------
// Webserver_HandleRoot – dashboard (/)
// ---------------------------------------------------------------------------
void Webserver_HandleRoot()
{
  if (iotWebConf.handleCaptivePortal()) return;

  if (redirect_to_sysinfo) {
    redirect_to_sysinfo = false;
    if (wifi_connected) {
      server.sendHeader("Location", "/sysinfo");
      server.send(302, "text/plain", "");
      return;
    }
  }

  bool isApMode        = !wifi_connected;
  bool hasReading      = LastMeterValue.timestamp > 0 && LastMeterValue.meter_value_180 > 0;
  bool hasPinPrecision = hasReading && (LastMeterValue.meter_value_180 % 10000) != 0;
  bool backendCalled   = last_call_backend > 0;
  bool backendOk       = call_backend_successfull;
  uint32_t ageS        = hasReading ? (uint32_t)(Time_getEpochTime() - LastMeterValue.timestamp) : 0;
  const uint32_t kNoTelegramThresholdS = 30;
  uint32_t backAgoMin  = backendCalled ? (millis() - last_call_backend) / 60000UL : 0;

  String s;
  s.reserve(4000);
  s += R"rawliteral(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<meta name="format-detection" content="telephone=no">
<title>SmartMeterLite</title>
<style>
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0;}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;background:#f0f2f7;min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:1.5rem 1rem 3rem;gap:1rem;color:#1a1a1a;}
.logo{font-size:1.5rem;font-weight:800;color:#1a3799;letter-spacing:-.02em;padding-top:.4rem;}
.meter-card{background:#1a3799;color:#fff;border-radius:16px;padding:2rem 2rem 1.6rem;text-align:center;width:100%;max-width:400px;box-shadow:0 4px 20px rgba(26,55,153,.22);}
.m-lbl{font-size:.76rem;opacity:.72;letter-spacing:.08em;text-transform:uppercase;margin-bottom:.6rem;}
.m-row{display:flex;align-items:center;justify-content:space-between;margin-bottom:.3rem;}
.m-row-l{display:flex;align-items:baseline;gap:.35rem;}
.m-arr{font-size:1.2rem;opacity:.8;flex-shrink:0;}
.m-val{font-size:2.3rem;font-weight:800;letter-spacing:-.03em;line-height:1;-webkit-text-fill-color:#fff;}
.m-val2{font-size:1.5rem;font-weight:700;letter-spacing:-.02em;line-height:1;-webkit-text-fill-color:#fff;opacity:.85;}
.m-unit{font-size:.78rem;opacity:.6;align-self:flex-end;padding-bottom:.1rem;}
.m-pwr{display:flex;align-items:baseline;gap:.2rem;opacity:.8;}
.m-pwr-val{font-size:1.05rem;font-weight:700;-webkit-text-fill-color:#fff;}
.m-pwr-unit{font-size:.72rem;opacity:.75;}
.m-div{border:none;border-top:1px solid rgba(255,255,255,.15);margin:.45rem 0;}
.m-age{font-size:.73rem;opacity:.48;margin-top:.35rem;}
.m-age-warn{color:#ffb300;opacity:1;font-weight:600;}
.m-net-lbl{font-size:.78rem;opacity:.55;}
.sc{background:#fff;border-radius:14px;border:1px solid #d0d8f0;padding:.9rem 1.1rem;width:100%;max-width:400px;display:flex;flex-direction:column;gap:.75rem;}
.sc-top{display:flex;align-items:center;gap:.75rem;}
.dot{width:13px;height:13px;border-radius:50%;flex-shrink:0;}
.dg{background:#4caf50;box-shadow:0 0 6px #4caf5066;}
.do{background:#ff9800;box-shadow:0 0 6px #ff980066;}
.dr{background:#f44336;box-shadow:0 0 6px #f4433666;}
.st strong{display:block;font-size:.92rem;margin-bottom:.1rem;}
.st small{font-size:.77rem;color:#666;}
.br{display:flex;gap:.7rem;width:100%;}
.btn{flex:1;display:flex;align-items:center;justify-content:center;gap:.35rem;padding:.75rem .4rem;border-radius:10px;font-size:.85rem;font-weight:700;text-decoration:none;border:2px solid #1a3799;color:#1a3799;background:#f0f2f7;text-align:center;line-height:1.3;}
.btn:hover{background:#1a3799;color:#fff;}
.lc{color:#666;font-size:.85rem;text-decoration:none;border:1px solid #d0d8f0;border-radius:8px;padding:.4rem 1rem;background:#fff;}
.lc:hover{color:#1a3799;}
.grafana-btn{display:flex;align-items:center;justify-content:center;gap:.5rem;width:100%;max-width:400px;padding:.85rem 1rem;border-radius:14px;background:#374151;color:#fff;font-size:.88rem;font-weight:700;text-decoration:none;white-space:nowrap;box-shadow:0 2px 8px rgba(0,0,0,.15);}
.grafana-btn:hover{background:#1e293b;}
footer{display:flex;flex-direction:column;align-items:center;gap:.4rem;margin-top:.5rem;padding-top:.5rem;}
footer a{color:#999;font-size:.78rem;text-decoration:none;}
footer a:hover{color:#1a3799;}
.footer-love{font-size:.78rem;color:#aaa;}
.wifi-card{background:#fff3cd;border:1px solid #ffc107;border-radius:14px;padding:1rem 1.1rem;width:100%;max-width:400px;display:flex;flex-direction:column;gap:.6rem;}
.wifi-card h3{font-size:.92rem;font-weight:700;color:#856404;margin:0;}
.wifi-scan-btn{display:flex;align-items:center;justify-content:center;gap:.4rem;padding:.75rem 1rem;border-radius:10px;background:linear-gradient(135deg,#ffc94d,#f9a825);color:#3d2b00;font-size:.92rem;font-weight:700;text-decoration:none;text-align:center;box-shadow:0 2px 8px rgba(249,168,37,.4);}
.wifi-scan-btn:hover{background:linear-gradient(135deg,#f9a825,#e69500);color:#3d2b00;}
.wifi-or{display:flex;align-items:center;gap:.6rem;font-size:.78rem;color:#856404;opacity:.75;}
.wifi-or::before,.wifi-or::after{content:'';flex:1;height:1px;background:#ffc107;opacity:.6;}
.wifi-form{display:flex;flex-direction:column;gap:.5rem;}
.wifi-form input[type=text],.wifi-form input[type=password]{width:100%;padding:.55rem .75rem;border-radius:8px;border:1px solid #ccc;font-size:.88rem;background:#fff;}
.wifi-form button{padding:.65rem 1rem;border-radius:8px;background:#1a3799;color:#fff;font-size:.88rem;font-weight:700;border:none;cursor:pointer;}
.wifi-form button:hover{background:#142b7a;}
</style>
</head>
<body>
<div class="logo">&#9889; SmartMeterLite</div>
)rawliteral";

  // Leistung bestimmen
  // Beide Richtungen (1.7.0 + 2.7.0) bekannt → an kWh-Zeilen hängen.
  // Sonst (nur eine Richtung oder nur 16.7.0) → standalone net-Zeile.
  bool hasPower   = false;
  bool netOnly    = false;
  int32_t importW = 0, exportW = 0, netW = 0;
  if (hasReading) {
    if (LastMeterValue.power_import > 0 && LastMeterValue.power_export > 0) {
      importW  = (int32_t)LastMeterValue.power_import;
      exportW  = (int32_t)LastMeterValue.power_export;
      hasPower = true;
    } else if (LastMeterValue.net_power != 0) {
      netW     = LastMeterValue.net_power;
      netOnly  = true;
      hasPower = true;
    } else if (LastMeterValue.power_import > 0) {
      netW     = (int32_t)LastMeterValue.power_import;
      netOnly  = true;
      hasPower = true;
    } else if (LastMeterValue.power_export > 0) {
      netW     = -(int32_t)LastMeterValue.power_export;
      netOnly  = true;
      hasPower = true;
    }
  }

  auto pwrStr = [](int32_t w) -> String {
    char buf[24];
    if (w >= 1000) snprintf(buf, sizeof(buf), "%.2f&thinsp;kW", w / 1000.0f);
    else           snprintf(buf, sizeof(buf), "%d&thinsp;W", (int)w);
    return String(buf);
  };

  // Meter reading card
  s += "<div class='meter-card'>"
       "<div class='m-lbl'>Z&#228;hlerstand</div>";

  // 1.8.0 — Bezug (↓) + Import-Leistung rechts
  if (hasReading) {
    char valBuf[32];
    uint32_t v = LastMeterValue.meter_value_180;
    snprintf(valBuf, sizeof(valBuf), "%lu,%04lu", (unsigned long)(v / 10000), (unsigned long)(v % 10000));
    s += "<div class='m-row'><div class='m-row-l'>"
         "<span class='m-arr'>&#8595;</span><span class='m-val' id='val180'>" + String(valBuf) + "</span>"
         "<span class='m-unit'>kWh</span></div>";
    if (!netOnly && importW > 0)
      s += "<div class='m-pwr'><span class='m-pwr-val' id='pwr-import'>" + pwrStr(importW) + "</span></div>";
    s += "</div>";
  } else {
    s += "<div class='m-age' style='margin:.6rem 0;'>Warte auf Telegramm&#8239;&#8230;</div>";
    if (lastByteTime > 0)
      s += "<div class='m-age-warn' style='margin-top:.4rem;font-size:.75rem;'>"
           "&#9888; Bytes empfangen, aber kein g&uuml;ltiges Telegramm &mdash; "
           "<a href='/serialScan' style='color:#ffb300;'>Baud/Parity pr&uuml;fen</a>"
           "</div>";
  }

  // Nettoleistung zwischen 1.8.0 und 2.8.0
  if (hasPower) {
    int32_t calcNet = netOnly ? netW : (importW - exportW);
    int32_t absP    = calcNet < 0 ? -calcNet : calcNet;
    const char* arr = calcNet >= 0 ? "&#8595;" : "&#8593;";
    const char* lbl = calcNet >= 0 ? "Netzbezug" : "Netzeinspeisung";
    s += "<hr class='m-div'><div class='m-row'><div class='m-row-l'>"
         "<span class='m-arr' id='net-arr'>" + String(arr) + "</span>"
         "<span class='m-pwr-val' id='net-val'>" + pwrStr(absP) + "</span>"
         "</div>"
         "<span class='m-net-lbl' id='net-lbl'>" + String(lbl) + "</span></div>";
  }

  // 2.8.0 — Einspeisung (↑) + Export-Leistung rechts, nur wenn > 0
  if (hasReading && LastMeterValue.meter_value_280 > 0) {
    char val2Buf[32];
    uint32_t v2 = LastMeterValue.meter_value_280;
    snprintf(val2Buf, sizeof(val2Buf), "%lu,%04lu", (unsigned long)(v2 / 10000), (unsigned long)(v2 % 10000));
    s += "<hr class='m-div'><div class='m-row'><div class='m-row-l'>"
         "<span class='m-arr'>&#8593;</span>"
         "<span class='m-val2' id='val280'>" + String(val2Buf) + "</span>"
         "<span class='m-unit'>kWh</span></div>";
    if (!netOnly && exportW > 0)
      s += "<div class='m-pwr'><span class='m-pwr-val' id='pwr-export'>" + pwrStr(exportW) + "</span></div>";
    s += "</div>";
  }

  if (hasReading) {
    bool recentBytes = lastByteTime > 0 && (millis() - lastByteTime) < 30000;
    if (ageS >= kNoTelegramThresholdS && recentBytes)
      s += "<div class='m-age m-age-warn' id='m-age'>&#9888; Empfange Bytes, kann Telegramm nicht lesen &mdash; "
           "<a href='/serialScan' style='color:#ffb300;'>Baud/Parity pr&uuml;fen</a></div>";
    else if (ageS >= kNoTelegramThresholdS)
      s += "<div class='m-age m-age-warn' id='m-age'>&#9888; Kein Telegramm seit " + String(ageS) + "&thinsp;s</div>";
    else
      s += "<div class='m-age' id='m-age'>Letzter Wert vor " + String(ageS) + "&thinsp;s</div>";
  }
  s += "</div>";

  // Time sync card — no readings are stored until the system time is valid
  bool timeSynced = Time_isSynced();
  if (!timeSynced) {
    s += "<div class='sc' id='sc-time'><div class='sc-top'><div class='dot do'></div><div class='st'>"
         "<strong>Warte auf Zeitsynchronisation</strong>"
         "<small>Ohne g&uuml;ltige Uhrzeit werden keine Messwerte gespeichert. Daf&uuml;r ist eine Internetverbindung n&ouml;tig.</small>"
         "</div></div></div>";
  }

  // WiFi setup card — only shown in AP mode
  if (isApMode) {
    s += R"rawliteral(<div class='wifi-card' id='wifi-card'>
<h3>&#128246; Kein WLAN verbunden &ndash; Netzwerk einrichten</h3>
<a class='wifi-scan-btn' href='/wifiScan'>&#128246; Verf&uuml;gbare WLANs anzeigen</a>
<div class='wifi-or'><span>oder manuell eingeben</span></div>
<form class='wifi-form' action='/wifiSetup' method='POST'>
<input type='text'     name='ssid'     placeholder='WLAN-Name (SSID)'    autocomplete='off' autocorrect='off' autocapitalize='none' spellcheck='false'>
<input type='password' name='password' placeholder='WLAN-Passwort'       autocomplete='current-password'>
<button type='submit'>Verbinden</button>
</form>
</div>)rawliteral";
  }

  // PIN / INF status
  s += "<div class='sc' id='sc-pin'>";
  if (!hasReading) {
    s += "<div class='sc-top'><div class='dot do'></div><div class='st'><strong>Kein Messwert</strong>"
         "<small>Noch kein Telegramm empfangen.</small></div></div>"
         "<div class='br'>"
         "<a class='btn' href='/PinAssistant'>&#128274; PIN Assistant</a>"
         "<a class='btn' href='/PinAssistantDeluxe'>&#128274; PIN Assistant Deluxe</a>"
         "</div>";
  } else if (ageS >= kNoTelegramThresholdS) {
    s += "<div class='sc-top'><div class='dot do'></div><div class='st'><strong>Kein Telegramm empfangen</strong>"
         "<small>Lesekopf getrennt oder Verbindungsproblem?</small></div></div>";
  } else if (hasPinPrecision) {
    s += "<div class='sc-top'><div class='dot dg'></div><div class='st'><strong>PIN eingegeben &#8211; INF aktiv</strong>"
         "<small>Messwerte mit Nachkommastellen.</small></div></div>";
  } else {
    s += "<div class='sc-top'><div class='dot do'></div><div class='st'><strong>PIN nicht eingegeben oder INF aus</strong>"
         "<small>Bitte PIN eingeben und INF auf ON setzen.</small></div></div>"
         "<div class='br'>"
         "<a class='btn' href='/PinAssistant'>&#128274; PIN Assistant</a>"
         "<a class='btn' href='/PinAssistantDeluxe'>&#128274; PIN Assistant Deluxe</a>"
         "</div>";
  }
  s += "</div>";

  // Backend status
  s += "<div class='sc' id='sc-backend'>";
  if (!wifi_connected)
    s += "<div class='sc-top'><div class='dot dr'></div><div class='st'><strong>Kein Backend-Kontakt</strong>"
         "<small>Kein WLAN &ndash; Backend nicht erreichbar.</small></div></div>";
  else if (!backendCalled)
    s += "<div class='sc-top'><div class='dot do'></div><div class='st'><strong>Backend noch nicht kontaktiert</strong>"
         "<small>Noch kein Backend-Call durchgef&#252;hrt.</small></div></div>";
  else if (backendOk)
    s += "<div class='sc-top'><div class='dot dg'></div><div class='st'><strong>Mit Backend verbunden</strong>"
         "<small>Letzter Call vor " + String(backAgoMin) + " min.</small></div></div>";
  else
    s += "<div class='sc-top'><div class='dot dr'></div><div class='st'><strong>Backend-Fehler</strong>"
         "<small>Letzter Backend-Call fehlgeschlagen.</small></div></div>"
         "<div class='br'><a class='btn' href='/testBackendConnection'>Verbindung testen</a></div>";
  s += "</div>";

  s += R"rawliteral(<a class='grafana-btn' href='https://portal.smartmeterlite.de' target='_blank' rel='noopener'>&#128200; portal.smartmeterlite.de &rarr;</a>
<a class='lc' href='/sysinfo'>&#9881;&#65039; Konfiguration &amp; Details</a>
<footer>
<a href='https://smartmeterlite.de' target='_blank' rel='noopener'>smartmeterlite.de</a>
<a href='https://www.linkedin.com/in/laurin-vierrath/' target='_blank' rel='noopener'>&#128039; From Laurin with Love</a>
</footer>
</body></html>)rawliteral";
  // Inject live-update script — needsReload/had280 are set from current server state
  s += "<script>var needsReload=";
  s += hasReading ? "false" : "true";
  s += ";var had280=";
  s += (hasReading && LastMeterValue.meter_value_280 > 0) ? "true" : "false";
  s += ";var timeSynced=";
  s += timeSynced ? "true" : "false";
  s += R"rawliteral(;
function _set(id,txt){var e=document.getElementById(id);if(e)e.textContent=txt;}
function _pwr(w){var a=Math.abs(w);return a>=1000?(a/1000).toFixed(2)+' kW':a+' W';}
function _meter(v){return Math.floor(v/10000)+','+String(v%10000).padStart(4,'0');}
function _live(d){
  if(needsReload&&d.meter_value_180>0){location.reload();return;}
  if(!had280&&d.meter_value_280>0){location.reload();return;}
  if(timeSynced!==d.time_synced){location.reload();return;}
  _set('val180',_meter(d.meter_value_180));
  if(d.meter_value_280>0)_set('val280',_meter(d.meter_value_280));
  var imp=d.power_import,exp=d.power_export,net=d.net_power;
  if(imp>0&&exp>0){
    _set('pwr-import',_pwr(imp));_set('pwr-export',_pwr(exp));
    var calc=imp-exp;
    _set('net-arr',calc>=0?'↓':'↑');
    _set('net-val',_pwr(Math.abs(calc)));
    _set('net-lbl',calc>=0?'Netzbezug':'Netzeinspeisung');
  }else if(net!==0){
    _set('net-arr',net>=0?'↓':'↑');
    _set('net-val',_pwr(net));
    _set('net-lbl',net>=0?'Netzbezug':'Netzeinspeisung');
  }else if(imp>0){
    _set('net-arr','↓');_set('net-val',_pwr(imp));_set('net-lbl','Netzbezug');
  }else if(exp>0){
    _set('net-arr','↑');_set('net-val',_pwr(exp));_set('net-lbl','Netzeinspeisung');
  }
  var ageS=d.age_s;
  var el=document.getElementById('m-age');
  if(el){
    if(ageS>=30&&d.last_byte_age_s<30){el.className='m-age m-age-warn';el.innerHTML='&#9888; Empfange Bytes, kann Telegramm nicht lesen &mdash; <a href=\'\/serialScan\' style=\'color:#ffb300;\'>Baud\/Parity prüfen<\/a>';}
    else if(ageS>=30){el.className='m-age m-age-warn';el.textContent='⚠ Kein Telegramm seit '+ageS+' s';}
    else{el.className='m-age';el.textContent='Letzter Wert vor '+ageS+' s';}
  }
  // PIN / meter status
  var hasR=d.meter_value_180>0,hasPP=hasR&&(d.meter_value_180%10000)!==0;
  var pe=document.getElementById('sc-pin');
  if(pe){var btns="<div class='br'><a class='btn' href='/PinAssistant'>&#128274; PIN Assistant</a><a class='btn' href='/PinAssistantDeluxe'>&#128274; PIN Assistant Deluxe</a></div>";var ph;
    if(!hasR)ph="<div class='sc-top'><div class='dot do'></div><div class='st'><strong>Kein Messwert</strong><small>Noch kein Telegramm empfangen.</small></div></div>"+btns;
    else if(ageS>=30)ph="<div class='sc-top'><div class='dot do'></div><div class='st'><strong>Kein Telegramm empfangen</strong><small>Lesekopf getrennt oder Verbindungsproblem?</small></div></div>";
    else if(hasPP)ph="<div class='sc-top'><div class='dot dg'></div><div class='st'><strong>PIN eingegeben &#8211; INF aktiv</strong><small>Messwerte mit Nachkommastellen.</small></div></div>";
    else ph="<div class='sc-top'><div class='dot do'></div><div class='st'><strong>PIN nicht eingegeben oder INF aus</strong><small>Bitte PIN eingeben und INF auf ON setzen.</small></div></div>"+btns;
    pe.innerHTML=ph;}
  // Backend / WiFi status
  var be=document.getElementById('sc-backend');
  if(be){var bh;
    if(!d.wifi_connected)bh="<div class='sc-top'><div class='dot dr'></div><div class='st'><strong>Kein Backend-Kontakt</strong><small>Kein WLAN &#8211; Backend nicht erreichbar.</small></div></div>";
    else if(!d.backend_called)bh="<div class='sc-top'><div class='dot do'></div><div class='st'><strong>Backend noch nicht kontaktiert</strong><small>Noch kein Backend-Call durchgef&#252;hrt.</small></div></div>";
    else if(d.backend_ok)bh="<div class='sc-top'><div class='dot dg'></div><div class='st'><strong>Mit Backend verbunden</strong><small>Letzter Call vor "+d.backend_ago_min+" min.</small></div></div>";
    else bh="<div class='sc-top'><div class='dot dr'></div><div class='st'><strong>Backend-Fehler</strong><small>Letzter Backend-Call fehlgeschlagen.</small></div></div><div class='br'><a class='btn' href='/testBackendConnection'>Verbindung testen</a></div>";
    be.innerHTML=bh;}
  if(d.wifi_connected&&document.getElementById('wifi-card'))location.reload();
}
setInterval(function(){fetch('/showLastMeterValue').then(function(r){return r.json();}).then(_live).catch(function(){});},2000);
</script>)rawliteral";

  server.send(200, "text/html", s);
}
