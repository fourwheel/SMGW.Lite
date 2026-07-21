#include "webserver_main.h"
#include "app_globals.h"
#include "serial_scan.h"
#include "log_buffer.h"
#include "html_style.h"
#include "debug_log.h"
#include "webserver_data.h"
#include <IotWebConf.h>
#include <IotWebConfUsing.h>
#include <WiFi.h>
#include <Update.h>

// Forward declarations for functions defined in main.cpp
void Param_configSaved();
void Webserver_HandleRoot();
void Webserver_HandleSysInfo();
void Webserver_SetCert();
void Webserver_TestBackendConnection();
void Webserver_TestBackendConnectionRun();
// Optical handlers — defined in webserver_optical.cpp
void Webserver_Flashlight();
void Webserver_PinAssistantDeluxe();
void Webserver_FlashPulse();
void Webserver_FlashLongPulse();
void Webserver_HandleCertUpload();
void Webclient_loadCertToChar();

// ---------------------------------------------------------------------------
// IotWebConf HTML format provider — customises the /config page appearance
// ---------------------------------------------------------------------------
class SmartMeterHtmlFormatProvider : public iotwebconf::HtmlFormatProvider
{
protected:
  String getStyleInner() override {
    return
      "*,*::before,*::after{box-sizing:border-box;margin:0;padding:0;}"
      "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
        "background:#f0f2f7;min-height:100vh;display:flex;flex-direction:column;"
        "align-items:center;padding:1.5rem 1rem 3rem;gap:.8rem;color:#1a1a1a;}"
      ".logo{font-size:1.4rem;font-weight:800;color:#1a3799;letter-spacing:-.02em;}"
      ".back{color:#1a3799;font-size:.85rem;text-decoration:none;width:100%;max-width:500px;}"
      ".back:hover{text-decoration:underline;}"
      ".card{background:#fff;border-radius:14px;border:1px solid #d0d8f0;"
        "padding:1.1rem 1.3rem;width:100%;max-width:500px;}"
      "div{padding:0;}"
      "input,select{padding:.55rem .7rem;border:1px solid #d0d8f0;border-radius:8px;"
        "font-size:.88rem;width:100%;background:#fff;margin-top:.3rem;box-sizing:border-box;}"
      "input:focus,select:focus{outline:2px solid #1a3799;border-color:#1a3799;}"
      "input[type=checkbox]{width:auto;margin:8px 6px;transform:scale(1.4);}"
      "label{font-size:.84rem;font-weight:500;color:#444;display:block;margin-top:.8rem;}"
      "fieldset{border:1px solid #d0d8f0;border-radius:10px;padding:.8rem 1rem 1rem;"
        "margin-bottom:.8rem;}"
      "legend{font-size:.9rem;font-weight:700;color:#1a3799;padding:0 .4rem;}"
      "button{padding:.68rem;border-radius:8px;background:#1a3799;color:#fff;"
        "font-size:.88rem;font-weight:700;border:none;cursor:pointer;width:100%;"
        "margin-top:1rem;min-height:44px;}"
      "button:hover{background:#142b7a;}"
      ".de{background:#fff3f3;border:1px solid #ffcccc;border-radius:6px;padding:.4rem;"
        "margin-top:.3rem;}"
      ".em{font-size:.78rem;color:#c62828;}"
      ".c{text-align:center;}"
      "a{color:#1a3799;text-decoration:none;}"
      "a:hover{text-decoration:underline;}";
  }
  String getBodyInner() override {
    return "<div class='logo'>&#9889; SmartMeterLite</div>"
           "<a class='back' href='/sysinfo'>&#8592; Zur&uuml;ck</a>"
           "<div class='card'>";
  }
  String getFormEnd() override {
    return "<button type='submit'>Speichern</button></form>";
  }
  String getFormSaved() override {
    return "<div style='font-size:.9rem;color:#2e7d32;font-weight:600;padding:.5rem 0;'>"
           "&#9989; Konfiguration gespeichert &ndash; <a href='/'>Zur Startseite</a></div>";
  }
  String getEnd() override { return "</div></body></html>"; }
  String getHeadExtension() override {
    return "<link rel='icon' type='image/svg+xml' href='/favicon.ico'>"
           "<script>document.title='SmartMeterLite – Konfiguration';</script>";
  }
};

static SmartMeterHtmlFormatProvider customHtmlFormatProvider;

// ---------------------------------------------------------------------------
// Param_setup — register all IotWebConf groups and parameters
// ---------------------------------------------------------------------------
void Param_setup()
{
  groupTelegram.addItem(&activate_IEC_Parser_object);
  groupBackend.addItem(&backend_endpoint_object);
  groupBackend.addItem(&backend_ID_object);
  groupBackend.addItem(&backend_token_object);
  groupTaf.addItem(&taf7_b_object);
  groupTaf.addItem(&taf7_param_object);
  groupTaf.addItem(&taf14_b_object);
  groupTaf.addItem(&taf14_param_object);
  groupBackend.addItem(&backend_call_minute_object);
  groupTelegram.addItem(&Meter_Value_Buffer_Size_object);
  groupSys.addItem(&led_blink_object);
  groupDebug.addItem(&DebugSetOffline_object);
  groupDebug.addItem(&DebugFromOtherClient_object);
  groupDebug.addItem(&DebugMeterValueFromOtherClientIP_object);

  groupAdditionalMeter.addItem(&mystrom_PV_object);
  groupAdditionalMeter.addItem(&mystrom_PV_IP_object);
  groupAdditionalMeter.addItem(&temperature_object);
  groupAdditionalMeter.addItem(&config_temperature_object);
  groupAdditionalMeter.addItem(&config_solar_object);
  groupAdditionalMeter.addItem(&config_280_object);

  groupBackend.addItem(&UseSslCert_object);

  iotWebConf.setStatusPin(STATUS_PIN);
  iotWebConf.setConfigPin(CONFIG_PIN);
  iotWebConf.addParameterGroup(&groupSys);
  iotWebConf.addParameterGroup(&groupTelegram);
  iotWebConf.addParameterGroup(&groupBackend);
  iotWebConf.addParameterGroup(&groupTaf);
  iotWebConf.addParameterGroup(&groupAdditionalMeter);
  iotWebConf.addParameterGroup(&groupDebug);

  iotWebConf.setConfigSavedCallback(&Param_configSaved);
  iotWebConf.getApTimeoutParameter()->visible = true;

  iotWebConf.skipApStartup();
  iotWebConf.setHtmlFormatProvider(&customHtmlFormatProvider);
  iotWebConf.init();
  iotWebConf.setApTimeoutMs(30000);
  iotWebConf.setWifiConnectionTimeoutMs(90000);
}

// ---------------------------------------------------------------------------
// Webserver_UrlConfig — register all HTTP routes
// ---------------------------------------------------------------------------
void Webserver_UrlConfig()
{
  server.on("/",                    Webserver_HandleRoot);
  server.on("/sysinfo",             Webserver_HandleSysInfo);
  server.on("/showTelegram",        Webserver_ShowTelegram);
  server.on("/showTelegramAnalysis", Webserver_Telegram_Analysis);
  server.on("/showTelegramRaw",     Webserver_ShowTelegram_Raw);
  server.on("/showLastMeterValue",  Webserver_ShowLastMeterValue);

  server.on("/setCert",             Webserver_SetCert);
  server.on("/testBackendConnection",    Webserver_TestBackendConnection);
  server.on("/testBackendConnectionRun", Webserver_TestBackendConnectionRun);
  server.on("/showMeterValues",     Webserver_ShowMeterValues);
  server.on("/showLogBuffer",       Webserver_ShowLogBuffer);
  server.on("/MeterValue_Num2",     Webserver_MeterValue_Num2);

  server.on("/PinAssistant",         Webserver_Flashlight);
  server.on("/PinAssistantDeluxe",   Webserver_PinAssistantDeluxe);
  server.on("/flash",                Webserver_FlashPulse);
  server.on("/flashlong",            Webserver_FlashLongPulse);
  server.on("/upload", [] { Webserver_HandleCertUpload(); Webclient_loadCertToChar(); });
  server.on("/wifiSetup",  HTTP_POST,  Webserver_HandleWifiSetup);
  server.on("/wifiStatus",             Webserver_HandleWifiStatus);

  // Captive portal suppression
  server.on("/hotspot-detect.html",      [] { server.send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"); });
  server.on("/library/test/success.html",[] { server.send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"); });
  server.on("/generate_204",             [] { server.send(204, "text/plain", ""); });
  server.on("/gen_204",                  [] { server.send(204, "text/plain", ""); });
  server.on("/connecttest.txt",          [] { server.send(200, "text/plain", "Microsoft Connect Test"); });
  server.on("/ncsi.txt",                 [] { server.send(200, "text/plain", "Microsoft NCSI"); });
  server.on("/config", [] { iotWebConf.handleConfig(); });
  server.on("/favicon.ico", [] {
    server.send(200, "image/svg+xml",
      "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'>"
      "<rect width='32' height='32' rx='6' fill='#1a3799'/>"
      "<polygon points='19,2 9,18 16,18 13,30 23,14 16,14' fill='#ffffff'/>"
      "</svg>");
  });
  server.on("/serialScan", [] {
    SerialScan_requestScan();
    String page;
    page.reserve(4000);
    page += R"rawliteral(<!DOCTYPE html>
<html lang="en"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Serial Scan</title>
)rawliteral";
    page += HTML_STYLE_SERIAL_SCAN;
    page += R"rawliteral(</head><body>
<div class="logo">&#9889; SMGWLite</div>
<a class="back" href="/sysinfo">&#8592; Back</a>
<div class="card">
<div class="card-title">Baud / Parity Scan &amp; Configuration</div>
<table>
)rawliteral";
    page += SerialScan_buildTableRows();
    page += R"rawliteral(</table>
<div id="result"></div>
<a class="btn" href="/sysinfo" id="back-btn" style="display:none">&#8592; Back</a>
</div>
<script>
var activeIdx=-1;
var timer=setInterval(poll,700);
function updateActive(n,newActive){
  for(var i=0;i<n;i++){
    var r=document.getElementById('r'+i);
    if(r)r.className=(i===newActive?'active-cfg':'');
    var b=document.getElementById('b'+i);
    if(b){
      if(i===newActive){b.textContent='Active';b.classList.add('act');}
      else{b.textContent='Activate';b.classList.remove('act');}
    }
  }
  activeIdx=newActive;
}
function poll(){
  fetch('/serialScanStatus').then(function(r){return r.json()}).then(function(d){
    var n=d.total||12;
    var mask=d.foundMask||0;
    var scanning=d.state==='running';
    var newActive=(d.activeIndex!==undefined)?d.activeIndex:-1;
    if(newActive!==activeIdx)updateActive(n,newActive);
    for(var i=0;i<n;i++){
      var el=document.getElementById('s'+i);
      if(!el)continue;
      var hit=(mask>>i)&1;
      if(hit){
        el.className='found';el.textContent='✓ Frame OK';
      }else if(scanning&&d.currentIndex===i){
        el.className='testing';el.textContent='... testing';
      }else if((scanning&&i<d.currentIndex)||d.state==='done'){
        el.className='fail';el.textContent='No Frame';
      }
    }
    if(d.state==='done'){
      clearInterval(timer);
      document.getElementById('back-btn').style.display='inline-flex';
    }
  }).catch(function(){});
}
function setConfig(idx){
  fetch('/setSerialConfig?idx='+idx).then(function(r){return r.json()}).then(function(d){
    if(d.ok)poll();
  }).catch(function(){});
}
poll();
</script>
</body></html>)rawliteral";
    server.send(200, "text/html", page);
  });
  server.on("/serialScanStatus", [] {
    server.send(200, "application/json", SerialScan_getStatusJson());
  });
  server.on("/setSerialConfig", [] {
    if (!server.hasArg("idx")) { server.send(400, "application/json", "{\"ok\":false}"); return; }
    int idx = server.arg("idx").toInt();
    if (!SerialConfig_setByIndex(idx)) { server.send(400, "application/json", "{\"ok\":false}"); return; }
    server.send(200, "application/json", "{\"ok\":true,\"label\":\"" + SerialScan_activeLabel() + "\"}");
  });
  server.on("/restart", [] { Webserver_LocationHrefsysinfo(5); delay(100); ESP.restart(); });
  server.on("/resetLogBuffer", [] { Webserver_LocationHrefsysinfo(); LogBuffer_reset(); });
  server.on("/StoreMeterValue", [] { Webserver_LocationHrefsysinfo(); Log_AddEntry(1006); MeterValue_trigger_override = true; });
  server.on("/MeterValue_init_Buffer", [] { MeterValue_init_Buffer(); Webserver_LocationHrefsysinfo(); });
  server.on("/sendboth_Task", [] { Webserver_LocationHrefsysinfo(2); Webclient_Send_Meter_Values_to_backend_wrapper(); Webclient_Send_Log_to_backend_wrapper(); });
  server.on("/sendLog_Task", [] { Webserver_LocationHrefsysinfo(2); Webclient_Send_Log_to_backend_wrapper(); });
  server.on("/sendMeterValues_Task", [] { Webserver_LocationHrefsysinfo(2); Webclient_Send_Meter_Values_to_backend_wrapper(); });
  server.on("/setOffline", [] { wifi_connected = false; Webserver_LocationHrefsysinfo(); });
  server.onNotFound([]() { iotWebConf.handleNotFound(); });

  // OTA update handler
  server.on("/update", HTTP_GET, []() {
    String page;
    page.reserve(1200);
    page += R"rawliteral(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>SmartMeterLite &ndash; Firmware Update</title>)rawliteral";
    page += HTML_STYLE_MODERN;
    page += R"rawliteral(</head>
<body>
<div class="logo">&#9889; SmartMeterLite</div>
<a class="back" href="/sysinfo">&#8592; Zur&uuml;ck</a>
<div class="card">
  <div class="card-title">Firmware Update</div>
  <p style="font-size:.84rem;color:#444;margin-bottom:.8rem;">W&auml;hle eine <code>.bin</code>-Datei aus und klicke auf &bdquo;Update starten&ldquo;. Das Ger&auml;t startet nach dem Update automatisch neu.</p>
  <form method="POST" action="/update" enctype="multipart/form-data">
    <input type="file" name="update" accept=".bin" style="font-size:.85rem;margin-bottom:.8rem;display:block;width:100%;">
    <div class="btns">
      <button class="btn" type="submit">Update starten</button>
      <a class="btn btn-s" href="/sysinfo">Abbrechen</a>
    </div>
  </form>
</div>
</body></html>)rawliteral";
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", page);
  });
  server.on("/update", HTTP_POST, []() {
    String result = Update.hasError() ? "Update fehlgeschlagen." : "Update erfolgreich &ndash; Neustart&hellip;";
    String page;
    page.reserve(800);
    page += R"rawliteral(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>SmartMeterLite &ndash; Update</title>)rawliteral";
    page += HTML_STYLE_MODERN;
    page += R"rawliteral(</head>
<body>
<div class="logo">&#9889; SmartMeterLite</div>
<div class="card">
  <div class="card-title">Firmware Update</div>
  <p style="font-size:.95rem;font-weight:600;color:#1a1a1a;">)rawliteral";
    page += result;
    page += R"rawliteral(</p>
</div>
</body></html>)rawliteral";
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", page);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Update Start: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
      else Update.printError(Serial);
    }
  });
}

// ---------------------------------------------------------------------------
// Webserver_HandleWifiSetup — POST /wifiSetup: save credentials, start connect
// ---------------------------------------------------------------------------
void Webserver_HandleWifiSetup()
{
  String ssid     = server.arg("ssid");
  String password = server.arg("password");

  ssid.trim();

  if (ssid.length() == 0) {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
    return;
  }

  strncpy(iotWebConf.getWifiSsidParameter()->valueBuffer,     ssid.c_str(),     IOTWEBCONF_WORD_LEN - 1);
  strncpy(iotWebConf.getWifiPasswordParameter()->valueBuffer, password.c_str(), IOTWEBCONF_WORD_LEN - 1);
  iotWebConf.getWifiSsidParameter()->valueBuffer[IOTWEBCONF_WORD_LEN - 1]     = '\0';
  iotWebConf.getWifiPasswordParameter()->valueBuffer[IOTWEBCONF_WORD_LEN - 1] = '\0';

  iotWebConf.saveConfig();
  redirect_to_sysinfo = false;
  DLOGLN("WiFi-Setup: credentials saved, starting direct connection attempt.");

  iotWebConf.forceApMode(true);
  WiFi.begin(ssid.c_str(), password.c_str());
  g_wifiSetupPending  = true;
  g_apStopAt          = 0;

  String page = R"rawliteral(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>SmartMeterLite &ndash; Verbinde&hellip;</title>
<style>
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0;}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;background:#f0f2f7;min-height:100vh;display:flex;flex-direction:column;align-items:center;justify-content:center;padding:2rem;gap:1rem;color:#1a1a1a;}
.logo{font-size:1.5rem;font-weight:800;color:#1a3799;letter-spacing:-.02em;}
.card{background:#fff;border-radius:16px;border:1px solid #d0d8f0;padding:2rem;width:100%;max-width:380px;display:flex;flex-direction:column;align-items:center;gap:1.1rem;text-align:center;}
.spinner{width:40px;height:40px;border:4px solid #d0d8f0;border-top-color:#1a3799;border-radius:50%;animation:spin 0.9s linear infinite;}
@keyframes spin{to{transform:rotate(360deg)}}
.msg{font-size:.95rem;color:#444;}
.ssid{font-weight:700;color:#1a3799;}
.hint{font-size:.82rem;color:#888;line-height:1.5;}
.err{color:#c0392b;font-size:.88rem;display:none;}
.result{display:none;flex-direction:column;align-items:center;gap:1rem;width:100%;}
.ip-box{background:#f0f2f7;border-radius:10px;padding:.9rem 1.2rem;font-size:1.05rem;color:#333;letter-spacing:.04em;}
.ip-last{font-weight:800;color:#1a3799;font-size:1.2rem;}
.steps{text-align:left;font-size:.84rem;color:#444;line-height:1.8;width:100%;}
.steps li{margin-left:1.1rem;}
.open-btn{display:block;width:100%;padding:.8rem;border-radius:10px;background:#1a3799;color:#fff;font-size:.95rem;font-weight:700;text-decoration:none;text-align:center;}
.open-btn:hover{background:#142b7a;}
</style>
</head>
<body>
<div class="logo">&#9889; SmartMeterLite</div>
<div class="card">
  <div id="connecting">
    <div class="spinner"></div>
    <p class="msg" style="margin-top:1rem;">Pr&uuml;fe Verbindungsdaten f&uuml;r <span class="ssid">)rawliteral" + ssid + R"rawliteral(</span>&hellip;</p>
  </div>
  <div class="result" id="result">
    <p class="msg">&#10003;&nbsp; Verbunden! Deine IP-Adresse:</p>
    <div class="ip-box" id="ip-display"></div>
    <ol class="steps">
      <li>Notiere dir die IP-Adresse &ndash; besonders das letzte Byte (fett).</li>
      <li>Wechsle jetzt mit deinem Ger&auml;t ins WLAN <span class="ssid">)rawliteral" + ssid + R"rawliteral(</span>.</li>
      <li>Klicke dann auf den Button unten.</li>
    </ol>
    <a class="open-btn" id="open-btn" href="#">SmartMeterLite &ouml;ffnen &rarr;</a>
  </div>
  <p class="err" id="err">Verbindung fehlgeschlagen &ndash; SSID oder Passwort pr&uuml;fen und erneut versuchen.</p>
</div>
<script>
var attempts = 0, max = 20;
function poll() {
  fetch('/wifiStatus')
    .then(function(r){ return r.json(); })
    .then(function(d){
      if (d.connected) {
        var parts = d.ip.split('.');
        var last  = parts.pop();
        var prefix = parts.join('.') + '.';
        document.getElementById('ip-display').innerHTML =
          prefix + '<span class="ip-last">' + last + '</span>';
        var url = 'http://' + d.ip + '/';
        document.getElementById('open-btn').href = url;
        document.getElementById('connecting').style.display = 'none';
        document.getElementById('result').style.display     = 'flex';
      } else if (++attempts < max) {
        setTimeout(poll, 2000);
      } else {
        document.getElementById('connecting').style.display = 'none';
        document.getElementById('err').style.display        = 'block';
      }
    })
    .catch(function(){ if (++attempts < max) setTimeout(poll, 2000); });
}
setTimeout(poll, 2000);
</script>
</body></html>)rawliteral";

  server.send(200, "text/html", page);
}

// ---------------------------------------------------------------------------
// Webserver_HandleWifiStatus — GET /wifiStatus: JSON with connected + IP
// ---------------------------------------------------------------------------
void Webserver_HandleWifiStatus()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    String ip = WiFi.localIP().toString();
    if (g_wifiSetupPending)
    {
      g_wifiSetupPending = false;
      g_apStopAt = millis() + 2000;
      DLOGLN("WiFi-Setup: connected, stopping AP in 2 s.");
    }
    server.send(200, "application/json", "{\"connected\":true,\"ip\":\"" + ip + "\"}");
  }
  else
  {
    server.send(200, "application/json", "{\"connected\":false}");
  }
}
