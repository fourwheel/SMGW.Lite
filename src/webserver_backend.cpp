// Backend certificate (upload, load) and the backend connection test page.
#include "webserver_backend.h"
#include "app_globals.h"
#include "backend_client.h"
#include "html_style.h"
#include "log_buffer.h"
#include "debug_log.h"
#include "ota_pull.h"
#include "time_utils.h"
#include "certs/isrg_root_x1.h"
#include <SPIFFS.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

char FullCert[2000];

void Webserver_SetCert()
{
  // Read directly from SPIFFS so the textarea is empty when no custom cert is stored.
  // Empty textarea = bundled ISRG Root X1 fallback is active.
  String stored = "";
  File file = SPIFFS.open("/cert.pem", FILE_READ);
  if (file && file.size() > 0)
  {
    stored = file.readString();
    file.close();
  }

  String page;
  page.reserve(2000);
  page += R"rawliteral(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>SmartMeterLite &ndash; SSL Certificate</title>)rawliteral";
  page += HTML_STYLE_MODERN;
  page += R"rawliteral(</head>
<body>
<div class="logo">&#9889; SmartMeterLite</div>
<a class="back" href="/sysinfo">&#8592; Zur&uuml;ck</a>
<div class="card">
  <div class="card-title">SSL Certificate</div>
  <form action="/upload" method="POST">
    <textarea name="cert" rows="12">)rawliteral";
  page += stored;
  page += R"rawliteral(</textarea>
    <p class="hint">Leave empty to use the onboard ISRG Root X1 cert (valid until 2035).</p>
    <div class="btns" style="margin-top:.7rem;">
      <button class="btn" type="submit">Save</button>
      <a class="btn btn-s" href="/sysinfo">Cancel</a>
    </div>
  </form>
</div>
</body></html>)rawliteral";

  server.send(200, "text/html", page);
}

void Webserver_TestBackendConnection()
{
  size_t certLen = strlen(FullCert);
  String certStart = String(FullCert).substring(0, 30);
  certStart.replace("<", "&lt;");
  String certEnd = certLen > 30 ? String(FullCert).substring(certLen - 30) : "";
  certEnd.replace("<", "&lt;");

  time_t now = (time_t)Time_getEpochTime();
  char timeBuf[32];
  struct tm* ti = gmtime(&now);
  strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S UTC", ti);

  String page;
  page.reserve(2500);
  page += R"rawliteral(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>SmartMeterLite &ndash; Backend Test</title>)rawliteral";
  page += HTML_STYLE_MODERN;
  page += R"rawliteral(</head>
<body>
<div class="logo">&#9889; SmartMeterLite</div>
<a class="back" href="/sysinfo">&#8592; Zur&uuml;ck</a>
<div class="card">
  <div class="card-title">Backend Connection Test</div>)rawliteral";
  page += "<div class='kv'><span class='kl'>Cert length</span>" + String(certLen) + "</div>";
  page += "<div class='kv'><span class='kl'>Cert start</span><code>" + certStart + "</code></div>";
  page += "<div class='kv'><span class='kl'>Cert end</span><code>" + certEnd + "</code></div>";
  page += "<div class='kv'><span class='kl'>Device time</span><code>" + String(timeBuf) + "</code> (epoch " + String((unsigned long)now) + ")</div>";
  page += R"rawliteral(
  <div id="result">
    <div class="kv"><span class="kl">Host</span><span class="warn">&#9679; Teste&hellip;</span></div>
  </div>
</div>
<script>
fetch('/testBackendConnectionRun')
  .then(function(r){return r.json();})
  .then(function(d){
    var h='';
    if(!d.host_ok){
      h+="<div class='kv last'><span class='kl'>Host</span><span class='fail'>&#10060; Nicht erreichbar</span></div>";
    } else {
      h+="<div class='kv'><span class='kl'>Host</span><span class='ok'>&#9989; Erreichbar</span></div>";
      h+="<div class='kv'><span class='kl'>Zertifikat</span>"+(d.cert_ok?"<span class='ok'>&#9989; G&uuml;ltig</span>":"<span class='fail'>&#10060; Ung&uuml;ltig</span>")+"</div>";
      h+="<div class='kv last'><span class='kl'>ID &amp; Token</span>"+(d.auth_ok?"<span class='ok'>&#9989; G&uuml;ltig</span>":"<span class='fail'>&#10060; Ung&uuml;ltig oder Timeout</span>")+"</div>";
    }
    document.getElementById('result').innerHTML=h;
  })
  .catch(function(){
    document.getElementById('result').innerHTML="<div class='kv last'><span class='kl'>Fehler</span><span class='fail'>Keine Antwort vom Ger&auml;t</span></div>";
  });
</script>
</body></html>)rawliteral";

  server.send(200, "text/html", page);
}

void Webserver_TestBackendConnectionRun()
{
  bool host_ok = false;
  bool cert_ok = false;
  bool auth_ok = false;

  WiFiClientSecure client;
  client.setHandshakeTimeout(10); // seconds; the default of 120 s would block loop() for minutes
  client.setCACert(FullCert);

  if (client.connect(backend_host.c_str(), 443, 5000))
  {
    host_ok = true;
    cert_ok = true;
  }
  else
  {
    client.stop();
    client.setInsecure();
    if (client.connect(backend_host.c_str(), 443, 5000))
      host_ok = true;
  }

  if (host_ok)
  {
    String url = String(backend_path) + "?backend_test=true&ID=" + String(backend_ID);
    client.print(String("GET ") + url + " HTTP/1.1\r\n" +
                 "Host: " + String(backend_host) + "\r\n" +
                 "X-Auth-Token: " + String(backend_token) + "\r\n" +
                 "Connection: close\r\n\r\n");
    // ID & token only count as valid if the backend confirms them in the body
    // ({"ok":true,"id":<backend_ID>}) — a bare HTTP 200 may come from a
    // misconfigured server that never ran the backend script.
    String body;
    if (Webclient_read_response(client, body, 5000) == 200)
    {
      JsonDocument doc;
      auth_ok = deserializeJson(doc, body) == DeserializationError::Ok &&
                (doc["ok"] | false) && strcmp(doc["id"] | "", backend_ID) == 0;
    }
    client.stop();
  }

  String json = "{\"host_ok\":";
  json += host_ok ? "true" : "false";
  json += ",\"cert_ok\":";
  json += cert_ok ? "true" : "false";
  json += ",\"auth_ok\":";
  json += auth_ok ? "true" : "false";
  json += "}";
  server.send(200, "application/json", json);
}

void Webserver_HandleCertUpload()
{
  if (server.hasArg("cert"))
  {
    String cert = server.arg("cert");
    cert.trim();

    if (cert.length() == 0)
    {
      // Empty submission — remove SPIFFS file so the bundled fallback cert is used.
      SPIFFS.remove("/cert.pem");
      Log_Add(8002);
      Webserver_LocationHrefsysinfo();
      return;
    }

    File file = SPIFFS.open("/cert.pem", FILE_WRITE);
    if (file)
    {
      file.println(cert);
      file.close();
      Log_Add(8002);
      Webserver_LocationHrefsysinfo();
    }
    else
    {
      Log_Add(8003);
      Webserver_LocationHrefsysinfo();
      server.send(500, "text/plain", "Cannot Open File!");
    }
  }
  else
  {
    Log_Add(8004);
    Webserver_LocationHrefsysinfo();
  }
}

void Webclient_loadCertToChar()
{
  File file = SPIFFS.open("/cert.pem", FILE_READ);
  if (file && file.size() > 0)
  {
    size_t size = min((size_t)file.size(), sizeof(FullCert) - 1);
    file.readBytes(FullCert, size);
    FullCert[size] = '\0';
    file.close();
    return;
  }
  // No cert in SPIFFS — fall back to bundled ISRG Root X1
  strncpy(FullCert, ISRG_ROOT_X1, sizeof(FullCert) - 1);
  FullCert[sizeof(FullCert) - 1] = '\0';
  Log_AddWithoutTransmit(8001);
}
