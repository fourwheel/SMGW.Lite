#include "webserver_main.h"
#include "app_globals.h"
#include "serial_scan.h"
#include "log_buffer.h"
#include "html_style.h"
#include "debug_log.h"
#include "webserver_data.h"
#include "ota_pull.h"
#include "version.h"
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

// Credentials from an in-progress /wifiSetup attempt, held in RAM only until
// the connection is confirmed to work — see Webserver_HandleWifiSetup().
static String g_pendingWifiSsid;
static String g_pendingWifiPassword;

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
  // groupTelegram.addItem(&dynTaf_enabled_object); // dynTaf disabled
  groupBackend.addItem(&backend_endpoint_object);
  groupBackend.addItem(&backend_ID_object);
  groupBackend.addItem(&backend_token_object);
  groupTaf.addItem(&taf7_b_object);
  groupTaf.addItem(&taf7_param_object);
  groupTaf.addItem(&taf14_b_object);
  groupTaf.addItem(&taf14_param_object);
  // tafdyn params intentionally NOT registered — adding them would shift the NVS
  // layout and force re-configuration of deployed devices. Values are hardcoded.
  groupBackend.addItem(&backend_call_minute_object);
  groupTelegram.addItem(&Meter_Value_Buffer_Size_object);
  groupSys.addItem(&led_blink_object);
  groupDebug.addItem(&DebugSetOffline_object);
  groupDebug.addItem(&DebugFromOtherClient_object);
  groupDebug.addItem(&DebugMeterValueFromOtherClientIP_object);

  groupAdditionalMeter.addItem(&mystrom_PV_object);
  groupAdditionalMeter.addItem(&mystrom_PV_IP_object);
  groupAdditionalMeter.addItem(&temperature_object);
  // Buffer field checkboxes — grouped with the sensors they relate to.
  // Changing these triggers MeterValue_init_Buffer() via Param_configSaved().
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
  server.on("/wifiSetup",       HTTP_POST, Webserver_HandleWifiSetup);
  server.on("/wifiStatus",                Webserver_HandleWifiStatus);
  server.on("/wifiScan",                  Webserver_HandleWifiScan);
  server.on("/wifiScanResults",           Webserver_HandleWifiScanResults);

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
  server.on("/checkRemoteFwUpdate", []() {
    String version;
    bool   fetched = false;
    if (wifi_connected && strlen(backend_ID) > 0 && !backend_host.isEmpty()) {
      ota_active = true;
      if (xSemaphoreTake(Sema_Backend, pdMS_TO_TICKS(30000))) {
        Log_AddEntry(6021);
        fetched = OtaPull_fetchManifestVersion(version);
        xSemaphoreGive(Sema_Backend);
      }
      ota_active            = false;
      g_ota_check_requested = false; // don't auto-install while user is deciding
    }
    if (fetched && version == FIRMWARE_VERSION) Log_AddEntry(6002);

    String page;
    page.reserve(900);
    page += R"rawliteral(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>SmartMeterLite &ndash; Remote FW Update</title>)rawliteral";
    page += HTML_STYLE_MODERN;
    page += R"rawliteral(</head>
<body>
<div class="logo">&#9889; SmartMeterLite</div>
<a class="back" href="/sysinfo">&#8592; Zur&uuml;ck</a>
<div class="card">
<div class="card-title">Remote Firmware Update</div>)rawliteral";

    if (!fetched) {
      page += R"rawliteral(<p>Kein Firmware-Update f&uuml;r dieses Ger&auml;t verf&uuml;gbar.</p>
<div class="btns"><a class="btn btn-s" href="/sysinfo">Zur&uuml;ck</a></div>)rawliteral";
    } else if (version == FIRMWARE_VERSION) {
      page += "<p>Firmware ist aktuell (v" + String(FIRMWARE_VERSION) + ").</p>\n"
              R"rawliteral(<div class="btns"><a class="btn btn-s" href="/sysinfo">Zur&uuml;ck</a></div>)rawliteral";
    } else {
      page += "<p>Version <strong>" + version + "</strong> verf&uuml;gbar"
              " (aktuell: v" + String(FIRMWARE_VERSION) + "). Jetzt installieren?</p>\n"
              R"rawliteral(<div class="btns">
<a class="btn" href="/installRemoteFw">Installieren</a>
<a class="btn btn-s" href="/sysinfo">Abbrechen</a>
</div>)rawliteral";
    }

    page += R"rawliteral(
</div>
</body></html>)rawliteral";
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", page);
  });
  server.on("/installRemoteFw", []() { g_ota_check_requested = true; Webserver_LocationHrefsysinfo(15); });
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
    page.reserve(2600);
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
  <p class="hint" id="fw-hint">W&auml;hle eine <code>.bin</code>-Datei aus und klicke auf &bdquo;Update starten&ldquo;. Das Ger&auml;t startet nach dem Update automatisch neu.</p>
  <input type="file" id="fw-file" accept=".bin" style="font-size:.85rem;margin:.8rem 0;display:block;width:100%;">
  <div class="btns">
    <button class="btn" id="fw-btn" onclick="fwStart()">Update starten</button>
    <a class="btn btn-s" id="fw-cancel" href="/sysinfo">Abbrechen</a>
  </div>
  <div id="fw-status" style="display:none;margin-top:1rem;">
    <div style="background:#f0f2f7;border-radius:8px;height:10px;overflow:hidden;margin-bottom:.5rem;">
      <div id="fw-bar" style="background:#1a3799;height:100%;width:0%;transition:width .2s;"></div>
    </div>
    <p id="fw-msg" style="font-size:.85rem;font-weight:600;"></p>
  </div>
</div>
<script>
function fwSetBusy(busy){
  document.getElementById('fw-btn').disabled=busy;
  document.getElementById('fw-file').disabled=busy;
}
function fwStart(){
  var f=document.getElementById('fw-file').files[0];
  if(!f){alert('Bitte zuerst eine .bin-Datei auswählen.');return;}
  fwSetBusy(true);
  document.getElementById('fw-status').style.display='block';
  var bar=document.getElementById('fw-bar'), msg=document.getElementById('fw-msg');
  msg.className=''; msg.textContent='Hochladen … 0%';
  var fd=new FormData(); fd.append('update', f);
  var xhr=new XMLHttpRequest();
  xhr.open('POST','/update',true);
  xhr.upload.onprogress=function(e){
    if(e.lengthComputable){
      var pct=Math.round(e.loaded/e.total*100);
      bar.style.width=pct+'%';
      msg.textContent='Hochladen … '+pct+'%';
    }
  };
  xhr.onload=function(){
    bar.style.width='100%';
    if(xhr.status===200){
      msg.className='ok';
      msg.textContent='Update erfolgreich – Gerät startet neu …';
      fwWaitForReboot();
    } else {
      msg.className='fail';
      msg.textContent='Update fehlgeschlagen: '+(xhr.responseText||('HTTP '+xhr.status));
      fwSetBusy(false);
    }
  };
  xhr.onerror=function(){
    // The connection can drop while the response is in flight if the
    // device restarts a moment too early - a successful flash is more
    // likely than a network fault, so treat it as tentative success.
    bar.style.width='100%';
    msg.className='ok';
    msg.textContent='Verbindung unterbrochen – Gerät startet vermutlich neu …';
    fwWaitForReboot();
  };
  xhr.send(fd);
}
var fwAttempt=0;
function fwWaitForReboot(){
  document.getElementById('fw-cancel').style.display='none';
  document.getElementById('fw-msg').textContent='Warte auf Geräteneustart …';
  fwAttempt=0;
  setTimeout(fwPollReboot,15000);
}
function fwPollReboot(){
  var msg=document.getElementById('fw-msg');
  fwAttempt++;
  fetch('/sysinfo',{cache:'no-store'}).then(function(r){
    if(!r.ok) throw new Error();
    msg.textContent='Gerät ist wieder online – Weiterleitung …';
    setTimeout(function(){window.location.href='/sysinfo';},800);
  }).catch(function(){
    if(fwAttempt<15){
      msg.textContent='Warte auf Neustart … (Versuch '+fwAttempt+'/15)';
      setTimeout(fwPollReboot,2000);
    } else {
      msg.className='fail';
      msg.textContent='Gerät antwortet nicht. Bitte Status manuell prüfen (z.B. IP im Router).';
    }
  });
}
</script>
</body></html>)rawliteral";
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", page);
  });
  server.on("/update", HTTP_POST, []() {
    bool ok = !Update.hasError();
    server.sendHeader("Connection", "close");
    if (ok) {
      server.send(200, "text/plain", "OK");
    } else {
      server.send(500, "text/plain", Update.errorString());
    }
    // Give the TCP stack a moment to flush the response before the reboot
    // tears down the connection, otherwise the client never sees the result.
    server.client().flush();
    delay(300);
    if (ok) ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Update Start: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { Update.printError(Serial); Log_AddEntry(6101); }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) { Update.printError(Serial); Log_AddEntry(6102); }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) { Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize); Log_AddEntry(6104); }
      else { Update.printError(Serial); Log_AddEntry(6103); }
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
  int    channel  = server.hasArg("channel") ? server.arg("channel").toInt() : 0;

  ssid.trim();

  if (ssid.length() == 0) {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
    return;
  }

  // Hold the credentials in RAM only for now — they're only written to flash
  // (see Webserver_HandleWifiStatus) once the connection is confirmed to
  // work, so a typo'd password can't overwrite a previously working config.
  g_pendingWifiSsid     = ssid;
  g_pendingWifiPassword = password;

  redirect_to_sysinfo = false;
  DLOGLN("WiFi-Setup: starting direct connection attempt (not yet saved).");

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
.err{color:#c0392b;font-size:.88rem;display:none;flex-direction:column;align-items:center;gap:1rem;width:100%;}
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
  <div id="connecting" style="display:flex;flex-direction:column;align-items:center;gap:1.1rem;">
    <div class="spinner"></div>
    <p class="msg">Pr&uuml;fe Verbindungsdaten f&uuml;r <span class="ssid">)rawliteral" + ssid + R"rawliteral(</span>&hellip;</p>
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
  <div class="err" id="err">
    <p id="err-msg">Verbindung fehlgeschlagen &ndash; SSID oder Passwort pr&uuml;fen und erneut versuchen.</p>
    <a class="open-btn" href="/">&#8634; Erneut versuchen</a>
  </div>
</div>
<script>
var attempts = 0, max = 20;
function showError(msg) {
  document.getElementById('connecting').style.display = 'none';
  if (msg) document.getElementById('err-msg').textContent = msg;
  document.getElementById('err').style.display = 'flex';
}
function poll() {
  // The AP can be briefly unreachable while the STA connection attempt is
  // using the shared radio - without an explicit timeout, a single fetch()
  // can hang far longer than the 2s retry interval (up to the browser's own
  // connect timeout, which can be tens of seconds), making this look stuck
  // even though it would eventually move on by itself.
  var ctrl = new AbortController();
  var to = setTimeout(function(){ ctrl.abort(); }, 5000);
  fetch('/wifiStatus', {signal: ctrl.signal})
    .then(function(r){ clearTimeout(to); return r.json(); })
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
      } else if (d.failed) {
        showError();
      } else if (++attempts < max) {
        setTimeout(poll, 2000);
      } else {
        showError();
      }
    })
    .catch(function(){ clearTimeout(to); if (++attempts < max) setTimeout(poll, 2000); });
}
setTimeout(poll, 2000);
</script>
</body></html>)rawliteral";

  server.send(200, "text/html", page);
  // server.send() only queues the response for the TCP stack - it doesn't
  // wait for the client to actually receive it. Give it a moment before the
  // radio activity below (association, possibly a channel scan) competes
  // with that delivery for the shared AP/STA radio, same as the /update
  // handler already does before its own disruptive ESP.restart().
  server.client().flush();
  delay(300);

  // Only start the actual connection attempt after the page has been sent —
  // WiFi.begin() drives real radio activity (association, possibly a channel
  // scan) on the same shared AP/STA radio that's still delivering this very
  // response; starting it earlier was delaying/aborting this request's own
  // TCP connection (ERR_CONNECTION_RESET), the same issue /wifiScan had.
  iotWebConf.forceApMode(true);
  // Passing the known channel (from a prior /wifiScan) skips the multi-channel
  // scan that WiFi.begin() would otherwise do to locate the SSID - that scan
  // hops the shared AP+STA radio across channels and briefly drops the AP the
  // client (phone/laptop) is connected to. Falls back to auto-scan (channel 0)
  // for the manual-entry form on the home page, which doesn't know the channel.
  if (channel > 0) WiFi.begin(ssid.c_str(), password.c_str(), channel);
  else              WiFi.begin(ssid.c_str(), password.c_str());
  g_wifiSetupPending   = true;
  g_wifiSetupFailed    = false;
  g_wifiSetupStartedAt = millis();
  g_apStopAt           = 0;
}

// ---------------------------------------------------------------------------
// Webserver_ConfirmWifiSetupSuccess — commit a confirmed-working /wifiSetup
// attempt: persist the pending credentials to flash and clear the setup
// state. Called from the success path of Webserver_HandleWifiStatus() once
// the client polls again after connecting, and from
// Webserver_CheckWifiSetupFallback() if the client never polls again after
// the device actually connected (closed tab, backgrounded app, etc.).
// ---------------------------------------------------------------------------
void Webserver_ConfirmWifiSetupSuccess()
{
  g_wifiSetupPending = false;
  g_apStopAt = millis() + 2000;

  // Only now that the credentials are confirmed to work are they written
  // to flash — see the comment in Webserver_HandleWifiSetup().
  strncpy(iotWebConf.getWifiSsidParameter()->valueBuffer,     g_pendingWifiSsid.c_str(),     IOTWEBCONF_WORD_LEN - 1);
  strncpy(iotWebConf.getWifiPasswordParameter()->valueBuffer, g_pendingWifiPassword.c_str(), IOTWEBCONF_WORD_LEN - 1);
  iotWebConf.getWifiSsidParameter()->valueBuffer[IOTWEBCONF_WORD_LEN - 1]     = '\0';
  iotWebConf.getWifiPasswordParameter()->valueBuffer[IOTWEBCONF_WORD_LEN - 1] = '\0';
  iotWebConf.saveConfig();
  g_pendingWifiPassword = "";

  Log_AddEntry(7004);
  DLOGLN("WiFi-Setup: connected, credentials saved, stopping AP in 2 s.");
}

// ---------------------------------------------------------------------------
// Webserver_ClearPendingWifiCredentials — drop a no-longer-needed candidate
// password from RAM. Called from every /wifiSetup failure path (never on
// success — Webserver_ConfirmWifiSetupSuccess() clears it itself after use).
// ---------------------------------------------------------------------------
void Webserver_ClearPendingWifiCredentials()
{
  g_pendingWifiSsid     = "";
  g_pendingWifiPassword = "";
}

// ---------------------------------------------------------------------------
// Webserver_HandleWifiStatus — GET /wifiStatus: JSON with connected + IP
// ---------------------------------------------------------------------------
void Webserver_HandleWifiStatus()
{
  // g_wifiSetupFailed is a sticky verdict for the current attempt, set either
  // here or by Webserver_CheckWifiSetupFallback() from the main loop. It must
  // be checked before WiFi.status(), not just in the not-connected path below:
  // the case where the STA falls back to the previously-saved network leaves
  // WiFi.status() == WL_CONNECTED (the old network still works), so without
  // this early check a poll arriving after the loop-driven fallback check
  // already ran would fall through to the WL_CONNECTED branch below and
  // wrongly report success.
  if (g_wifiSetupFailed)
  {
    server.send(200, "application/json", "{\"connected\":false,\"failed\":true}");
    return;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    // WL_CONNECTED alone isn't proof that THIS attempt succeeded: if the new
    // credentials fail, the STA can fall back to a previously-saved network
    // (still on flash, since we haven't overwritten it yet) and report
    // WL_CONNECTED for that instead. Mistaking that for success would both
    // show the wrong network as "connected" and overwrite the old, working
    // config with the new, never-actually-verified one.
    if (g_wifiSetupPending && WiFi.SSID() != g_pendingWifiSsid)
    {
      g_wifiSetupPending = false;
      g_wifiSetupFailed  = true;
      Log_AddEntry(7003);
      DLOGLN("WiFi-Setup: connected to a different network than requested (previously-saved config) - treating as failed.");
      Webserver_ClearPendingWifiCredentials();
      server.send(200, "application/json", "{\"connected\":false,\"failed\":true}");
      return;
    }

    String ip = WiFi.localIP().toString();
    if (g_wifiSetupPending) Webserver_ConfirmWifiSetupSuccess();
    server.send(200, "application/json", "{\"connected\":true,\"ip\":\"" + ip + "\"}");
    return;
  }

  // Not connected and not (yet) marked failed — the actual failure detection
  // + WiFi.disconnect() happens unconditionally in handle_wifi_setup_lifecycle()
  // (main loop), not here — this request may itself be delayed while the STA
  // connection attempt is starving the AP's radio time, so it must not be the
  // only place that aborts the attempt.
  server.send(200, "application/json", "{\"connected\":false}");
}

// ---------------------------------------------------------------------------
// Webserver_CheckWifiSetupFallback — called every loop() iteration (via
// handle_wifi_setup_lifecycle()) while a /wifiSetup attempt is connected but
// not yet confirmed.
//
// The normal path is Webserver_HandleWifiStatus(): the browser polls
// /wifiStatus, sees the connection, and that request persists the
// credentials / clears the pending state. If the client never polls again
// after the device actually connects — tab closed, app backgrounded, browser
// navigated away — that confirmation never happens. Without this fallback
// the new credentials would then silently never reach flash and be lost on
// the next reboot, even though the device is connected right now; and the
// forced AP would keep running alongside the STA connection forever, since
// nothing else clears g_wifiSetupPending in this case (IotWebConf's own AP
// timeout doesn't help either — forceApMode(true) makes it stay in AP mode
// unconditionally).
//
// Uses the same SSID check as Webserver_HandleWifiStatus() so a connection
// that fell back to the old saved network is never mistaken for success.
// ---------------------------------------------------------------------------
void Webserver_CheckWifiSetupFallback()
{
  if (!g_wifiSetupPending || WiFi.status() != WL_CONNECTED) return;
  if (millis() - g_wifiSetupStartedAt < 60000) return; // give /wifiStatus a fair chance first

  if (WiFi.SSID() == g_pendingWifiSsid)
  {
    DLOGLN("WiFi-Setup: connected but client never confirmed - saving credentials now.");
    Webserver_ConfirmWifiSetupSuccess();
  }
  else
  {
    // Connected, but to the old, previously-saved network, not the requested
    // one - nothing new to persist, just stop blocking the pending state forever.
    g_wifiSetupPending = false;
    g_wifiSetupFailed  = true;
    Log_AddEntry(7003);
    DLOGLN("WiFi-Setup: connected to previously-saved network, client never confirmed - marking failed.");
    Webserver_ClearPendingWifiCredentials();
  }
}

// ---------------------------------------------------------------------------
// Webserver_HandleWifiScan — GET /wifiScan: network list page with async poll
// ---------------------------------------------------------------------------
void Webserver_HandleWifiScan()
{
  String page;
  page.reserve(5500);
  page += R"rawliteral(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>SmartMeterLite &ndash; WLAN-Netzwerke</title>)rawliteral";
  page += HTML_STYLE_MODERN;
  page += R"rawliteral(<style>
@keyframes spin{to{transform:rotate(360deg)}}
.spin-sm{display:inline-block;width:13px;height:13px;border:2px solid #d0d8f0;border-top-color:#1a3799;border-radius:50%;animation:spin 0.9s linear infinite;vertical-align:middle;margin-right:.45rem;}
</style>)rawliteral";
  page += R"rawliteral(</head>
<body>
<div class="logo">&#9889; SmartMeterLite</div>
<a class="back" href="/">&#8592; Zur&uuml;ck</a>
<div class="card">
<div class="card-title">Verf&uuml;gbare WLAN-Netzwerke</div>
<div id="scan-status" style="color:#888;font-size:.9rem;margin-bottom:.8rem;"><span class="spin-sm"></span>Suche nach Netzwerken&hellip;</div>
<div id="networks"></div>
<div class="btns" id="rescan-row" style="display:none;margin-top:.6rem;">
<button class="btn btn-s" onclick="doRescan()">&#128260; Erneut suchen</button>
</div>
</div>
<script>
var t;
function bars(r){
  var s=(r>=-55?5:r>=-65?4:r>=-75?3:r>=-85?2:1);
  var col=(r>=-65?'#2e7d32':r>=-80?'#f9a825':'#c62828');
  var b='';
  for(var i=0;i<5;i++)b+='<span style="opacity:'+(i<s?'1':'.18')+'">&#9646;</span>';
  return'<span style="color:'+col+';letter-spacing:-1px;font-size:1.05rem;">'+b+'</span>';
}
function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');}
function toggle(i){
  var f=document.getElementById('f'+i);
  if(!f)return;
  var vis=f.style.display!=='none';
  f.style.display=vis?'none':'block';
  if(!vis){var p=document.getElementById('pw'+i);if(p)p.focus();}
}
function render(nets){
  if(!nets||nets.length===0){
    document.getElementById('scan-status').textContent='Keine Netzwerke gefunden.';
    document.getElementById('rescan-row').style.display='flex';
    return;
  }
  document.getElementById('scan-status').textContent=nets.length+' Netzwerk'+(nets.length!==1?'e':'')+' gefunden.';
  nets=nets.slice().sort(function(a,b){return b.rssi-a.rssi;});
  var h='';
  for(var i=0;i<nets.length;i++){
    var n=nets[i],se=esc(n.ssid);
    h+='<div style="border:1px solid #dde3f0;border-radius:8px;margin-bottom:.45rem;overflow:hidden;">'
      +'<div style="display:flex;align-items:center;justify-content:space-between;padding:.55rem .8rem;background:#f5f7fb;">'
      +'<span style="font-weight:600;font-size:.93rem;">'+(n.enc?'&#128274;&thinsp;':'&#128275;&thinsp;')+se+'</span>'
      +'<span style="display:flex;align-items:center;gap:.5rem;">'+bars(n.rssi)
      +'<button class="btn btn-s" style="font-size:.8rem;padding:.22rem .6rem;min-height:0;" onclick="toggle('+i+')">Verbinden &#9660;</button>'
      +'</span></div>'
      +'<div id="f'+i+'" style="display:none;padding:.65rem .8rem;background:#fff;border-top:1px solid #eee;">'
      +'<form action="/wifiSetup" method="POST">'
      +'<input type="hidden" name="channel" value="'+n.channel+'">'
      +'<label style="font-size:.8rem;color:#555;display:block;margin-bottom:.15rem;">SSID</label>'
      +'<input name="ssid" type="text" value="'+se+'" readonly style="width:100%;box-sizing:border-box;padding:.38rem .55rem;border:1px solid #ccc;border-radius:6px;font-size:.9rem;margin-bottom:.45rem;background:#f5f5f5;color:#555;">'
      +'<label style="font-size:.8rem;color:#555;display:block;margin-bottom:.15rem;">Passwort</label>'
      +'<input id="pw'+i+'" name="password" type="password" placeholder="WLAN-Passwort" autocomplete="current-password" style="width:100%;box-sizing:border-box;padding:.38rem .55rem;border:1px solid #ccc;border-radius:6px;font-size:.9rem;margin-bottom:.55rem;">'
      +'<div class="btns"><button class="btn" type="submit">Verbinden</button>'
      +'<button class="btn btn-s" type="button" onclick="toggle('+i+')">Abbrechen</button></div>'
      +'</form></div></div>';
  }
  document.getElementById('networks').innerHTML=h;
  document.getElementById('rescan-row').style.display='flex';
}
function doRescan(){
  document.getElementById('scan-status').innerHTML='<span class="spin-sm"></span>Suche nach Netzwerken…';
  document.getElementById('networks').innerHTML='';
  document.getElementById('rescan-row').style.display='none';
  clearInterval(t);
  fetch('/wifiScanResults?rescan=1').then(function(){t=setInterval(poll,800);});
}
function poll(){
  fetch('/wifiScanResults').then(function(r){return r.json();}).then(function(d){
    if(d.state==='scanning')return;
    clearInterval(t);
    render(d.networks);
  }).catch(function(){});
}
poll();
t=setInterval(poll,800);
</script>
</body></html>)rawliteral";
  server.send(200, "text/html", page);

  // Start scanning only after the page has been sent — WiFi.scanNetworks()
  // hops the shared AP/STA radio across channels immediately, which was
  // delaying delivery of this very page (including the waiting spinner)
  // until the scan had already progressed.
  WiFi.scanNetworks(true);
}

// ---------------------------------------------------------------------------
// Webserver_HandleWifiScanResults — GET /wifiScanResults: JSON scan results
// ---------------------------------------------------------------------------
void Webserver_HandleWifiScanResults()
{
  if (server.hasArg("rescan")) {
    WiFi.scanNetworks(true);
    server.send(200, "application/json", "{\"state\":\"scanning\"}");
    return;
  }

  int n = WiFi.scanComplete();
  if (n < 0) {
    if (n == WIFI_SCAN_FAILED) WiFi.scanNetworks(true);
    server.send(200, "application/json", "{\"state\":\"scanning\"}");
    return;
  }

  String json = "{\"state\":\"done\",\"networks\":[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ',';
    String ssid = WiFi.SSID(i);
    ssid.replace("\\", "\\\\");
    ssid.replace("\"", "\\\"");
    json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i))
         + ",\"channel\":" + String(WiFi.channel(i))
         + ",\"enc\":" + (WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  json += "]}";
  WiFi.scanDelete();
  server.send(200, "application/json", json);
}
