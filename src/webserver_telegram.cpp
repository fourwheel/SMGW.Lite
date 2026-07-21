#include "webserver_telegram.h"
#include "app_globals.h"
#include "meter_value.h"
#include "log_buffer.h"
#include "html_style.h"
#include "debug_log.h"
#include "time_utils.h"
#include <ArduinoJson.h>

// ---------------------------------------------------------------------------
// SML CRC16 (X25): initial 0xFFFF, poly 0x8408 (reversed 0x1021), final XOR 0xFFFF
// ---------------------------------------------------------------------------
static uint16_t calculateSML_CRC16(uint8_t* data, size_t len)
{
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 0x0001) crc = (crc >> 1) ^ 0x8408;
      else crc >>= 1;
    }
  }
  return crc ^ 0xFFFF;
}

// Searches buffer for OBIS code and returns an HTML table row with the value.
static String getObisRow(uint8_t* buffer, int prefix, int suffix, uint8_t* code, const char* label, const char* unit, float scaler)
{
  for (size_t i = (size_t)prefix; i < (size_t)suffix - 12; i++) {
    if (memcmp(&buffer[i], code, 6) == 0) {
      for (int j = i + 6; j < i + 25; j++) {
        if (buffer[j] == 0x52) {
          uint8_t typeByte = buffer[j + 2];
          int vLen   = (typeByte & 0x0F) - 1;
          int vStart = j + 3;
          int32_t raw = 0;
          for (int k = 0; k < vLen; k++) raw = (raw << 8) | buffer[vStart + k];
          if (vLen <= 4 && (buffer[vStart] & 0x80) && (strcmp(unit, "W") == 0)) {
            if (vLen == 1) raw -= 256;
            else if (vLen == 2) raw -= 65536;
          }
          return "<tr><td>" + String(label) + "</td>" +
                 "<td>" + String(vStart) + "</td>" +
                 "<td>" + String(vLen) + " Bytes</td>" +
                 "<td><strong>" + String(raw * scaler, 1) + " " + unit + "</strong></td></tr>";
        }
      }
    }
  }
  return "<tr><td>" + String(label) + "</td><td>&#x2013;</td><td>&#x2013;</td><td>n/a</td></tr>";
}

// ---------------------------------------------------------------------------
// Telegram analysis helpers — file-local, called only from analyzeTelegram()
// ---------------------------------------------------------------------------

// "Parsed Values" card common to both IEC and SML.
static String buildCommonSection(uint8_t* buffer, size_t length)
{
  String s = "<div class='card'><div class='card-title'>Parsed Values";
  if (LastMeterValue.timestamp > 0) {
    uint32_t ageS = (uint32_t)(Time_getEpochTime() - LastMeterValue.timestamp);
    s += "<small style='font-weight:400;color:#888;'>&mdash; last telegram: " + String(ageS) + " s ago</small>";
  }
  s += "</div>";

  if (LastMeterValue.timestamp == 0) {
    s += "<p style='font-size:.84rem;color:#888;font-style:italic;'>No telegram received yet.</p></div>";
    return s;
  }

  bool isIEC = (length > 0 && buffer[0] == '/');

  s += "<p style='font-size:.84rem;font-weight:600;color:#1a3799;margin-bottom:.3rem;'>Meter Identification</p>";
  s += "<div class='tbl'><table><tr><th>Field</th><th>Value</th></tr>";

  if (isIEC) {
    static char firstLine[128];
    const char* src = (const char*)buffer;
    const char* eol = strstr(src, "\r\n");
    size_t rawLen = eol ? (size_t)(eol - src) : 0;
    size_t lineLen = rawLen < sizeof(firstLine) - 1 ? rawLen : sizeof(firstLine) - 1;
    strncpy(firstLine, src, lineLen);
    firstLine[lineLen] = '\0';

    char mfr[4]    = "n/a";
    char ident[64] = "n/a";
    if (lineLen >= 4) {
      strncpy(mfr, firstLine + 1, 3); mfr[3] = '\0';
      if (lineLen > 5) {
        size_t identLen = lineLen - 5 < sizeof(ident) - 1 ? lineLen - 5 : sizeof(ident) - 1;
        strncpy(ident, firstLine + 5, identLen);
        ident[identLen] = '\0';
      }
    }
    if (meter_model.isEmpty()) meter_model = String(mfr) + " " + String(ident);
    s += "<tr><td>Protocol</td><td>IEC 62056-21</td></tr>";
    s += "<tr><td>Manufacturer Code</td><td><strong>" + String(mfr)   + "</strong></td></tr>";
    s += "<tr><td>Meter Identifier</td><td><strong>"  + String(ident) + "</strong></td></tr>";
    s += "<tr><td>Payload Length</td><td>" + String(length) + " Bytes</td></tr>";
  } else {
    int px = -1, sx = -1;
    for (size_t i = 0; i < length - 4; i++)
      if (buffer[i]==0x1b && buffer[i+1]==0x1b && buffer[i+2]==0x1b && buffer[i+3]==0x1b) { px=i; break; }
    if (px != -1)
      for (size_t i = px; i < length - 5; i++)
        if (buffer[i]==0x1b && buffer[i+1]==0x1b && buffer[i+2]==0x1b && buffer[i+3]==0x1b && buffer[i+4]==0x1a) { sx=i; break; }

    String meterId = "n/a";
    if (px != -1 && sx != -1) {
      uint8_t obis960[] = {0x01, 0x00, 0x60, 0x01, 0x00, 0xff};
      for (int i = px; i < sx - 12; i++) {
        if (memcmp(&buffer[i], obis960, 6) == 0) {
          for (int j = i + 6; j < i + 40 && j < sx; j++) {
            uint8_t tl = buffer[j];
            if ((tl & 0xF0) == 0x00 && (tl & 0x0F) > 1) {
              int len = (tl & 0x0F) - 1;
              if (j + len >= sx) break;
              meterId = "";
              for (int k = 0; k < len - 2; k++) {
                uint8_t a = buffer[j+1+k], b = buffer[j+2+k], c = buffer[j+3+k];
                if (a >= 'A' && a <= 'Z' && b >= 'A' && b <= 'Z' && c >= 'A' && c <= 'Z') {
                  meterId = String((char)a) + String((char)b) + String((char)c);
                  break;
                }
              }
              if (meterId.isEmpty()) {
                char hex[3];
                for (int k = 0; k < len; k++) {
                  sprintf(hex, "%02X", buffer[j+1+k]);
                  if (k) meterId += ' ';
                  meterId += hex;
                }
              }
              break;
            }
          }
          break;
        }
      }
    }
    if (meter_model.isEmpty() && meterId != "n/a") meter_model = meterId;
    s += "<tr><td>Protocol</td><td>SML</td></tr>";
    s += "<tr><td>Meter Serial (96.1.0)</td><td><strong>" + meterId + "</strong></td></tr>";
    if (px != -1 && sx != -1) {
      int crcLen = (sx + 6) - px;
      s += "<tr><td>Payload Length</td><td>" + String(crcLen + 2) + " Bytes</td></tr>";
    }
  }
  s += "</table></div>";

  s += "<p style='font-size:.84rem;font-weight:600;color:#1a3799;margin:.7rem 0 .3rem;'>Energy Counters</p>";
  s += "<div class='tbl'><table><tr><th>OBIS</th><th>Value</th></tr>";
  s += "<tr><td>Consumption (1.8.0)</td><td><strong>" +
       String(LastMeterValue.meter_value_180 * 0.1f, 1) + " Wh</strong></td></tr>";
  s += "<tr><td>Delivery (2.8.0)</td><td><strong>" +
       String(LastMeterValue.meter_value_280 * 0.1f, 1) + " Wh</strong></td></tr>";
  s += "</table></div>";

  s += "</div>";
  return s;
}

static String analyzeSML_section(uint8_t* buffer, size_t length)
{
  String s = "<div class='card'><div class='card-title'>SML Raw Analysis</div>";

  int px = -1;
  for (size_t i = 0; i < (int)length - 4; i++)
    if (buffer[i]==0x1b && buffer[i+1]==0x1b && buffer[i+2]==0x1b && buffer[i+3]==0x1b) { px=i; break; }
  int sx = -1;
  if (px != -1)
    for (size_t i = px; i < (int)length - 5; i++)
      if (buffer[i]==0x1b && buffer[i+1]==0x1b && buffer[i+2]==0x1b && buffer[i+3]==0x1b && buffer[i+4]==0x1a) { sx=i; break; }

  if (px == -1 || sx == -1)
    return s + "<p class='fail'>&#10060; Telegram incomplete &mdash; Prefix/Suffix missing.</p></div>";

  int      crcLen  = (sx + 6) - px;
  uint16_t compCRC = calculateSML_CRC16(&buffer[px], crcLen);
  uint16_t recvCRC = (buffer[sx + 6] << 8) | buffer[sx + 7];
  bool     crcOk   = (compCRC == recvCRC);

  s += "<p style='font-size:.84rem;font-weight:600;color:#1a3799;margin-bottom:.3rem;'>Telegram Status</p>";
  s += "<div class='tbl'><table><tr><th>Parameter</th><th>Value</th></tr>";
  s += "<tr><td>Index (Start/End)</td><td>" + String(px) + " / " + String(sx) + "</td></tr>";
  s += "<tr><td>Checksum (CRC)</td><td>";
  if (recvCRC == 0)   s += "Meter sent no CRC (0x0000)";
  else if (crcOk)     s += "<span class='ok'>&#9989; 0x" + String(recvCRC, HEX) + " (Valid)</span>";
  else                s += "<span class='fail'>&#10060; 0x" + String(recvCRC, HEX) + " (Invalid &mdash; expected: 0x" + String(compCRC, HEX) + ")</span>";
  s += "</td></tr></table></div>";

  s += "<p style='font-size:.84rem;font-weight:600;color:#1a3799;margin:.7rem 0 .3rem;'>Registers (from raw binary)</p>";
  s += "<div class='tbl'><table><tr><th>OBIS Code</th><th>Index</th><th>Length</th><th>Reading</th></tr>";
  uint8_t obis180[] = {0x01,0x00,0x01,0x08,0x00,0xff};
  uint8_t obis280[] = {0x01,0x00,0x02,0x08,0x00,0xff};
  uint8_t obis170[] = {0x01,0x00,0x01,0x07,0x00,0xff};
  uint8_t obis270[] = {0x01,0x00,0x02,0x07,0x00,0xff};
  uint8_t obis167[] = {0x01,0x00,0x10,0x07,0x00,0xff};
  s += getObisRow(buffer, px, sx, obis180, "Consumption (1.8.0)",  "Wh", 0.1);
  s += getObisRow(buffer, px, sx, obis280, "Delivery (2.8.0)",     "Wh", 0.1);
  s += getObisRow(buffer, px, sx, obis170, "Power Import (1.7.0)", "W",  1.0);
  s += getObisRow(buffer, px, sx, obis270, "Power Export (2.7.0)", "W",  1.0);
  s += getObisRow(buffer, px, sx, obis167, "Net Power (16.7.0)",   "W",  1.0);
  s += "</table></div></div>";
  return s;
}

static String analyzeIEC_section(uint8_t* buffer, size_t length)
{
  String s = "<div class='card'><div class='card-title'>IEC 62056-21 Raw Analysis</div>";

  static char telegram_str[TELEGRAM_LENGTH + 1];
  size_t copy_len = (length <= TELEGRAM_LENGTH) ? length : TELEGRAM_LENGTH;
  memcpy(telegram_str, buffer, copy_len);
  telegram_str[copy_len] = '\0';

  if (telegram_str[0] != '/')
    return s + "<p class='fail'>&#10060; No IEC telegram in buffer.</p></div>";

  auto parseObis = [&](const char* label, float* out) -> bool {
    const char *p = strstr(telegram_str, label);
    if (!p) return false;
    const char *op = strchr(p, '(');
    const char *st = op ? strchr(op, '*') : nullptr;
    if (!op || !st || op >= st) return false;
    char buf[20]; size_t l = st - op - 1;
    if (l >= sizeof(buf)) return false;
    strncpy(buf, op + 1, l); buf[l] = '\0';
    for (int i = 0; buf[i]; i++) if (buf[i] == ',') buf[i] = '.';
    *out = atof(buf);
    return true;
  };

  struct { const char* label; const char* name; const char* unit; } regs[] = {
    { "1-0:1.8.0",  "Consumption (1.8.0)",  "kWh" },
    { "1-0:2.8.0",  "Delivery (2.8.0)",     "kWh" },
    { "1-0:1.7.0",  "Power Import (1.7.0)", "kW"  },
    { "1-0:2.7.0",  "Power Export (2.7.0)", "kW"  },
    { "1-0:16.7.0", "Net Power (16.7.0)",   "kW"  },
  };

  s += "<p style='font-size:.84rem;font-weight:600;color:#1a3799;margin-bottom:.3rem;'>Registers (from telegram text)</p>";
  s += "<div class='tbl'><table><tr><th>OBIS Code</th><th>Reading</th></tr>";
  for (auto& r : regs) {
    float val = 0.0f;
    if (parseObis(r.label, &val))
      s += "<tr><td>" + String(r.name) + "</td><td><strong>" + String(val, 3) + " " + r.unit + "</strong></td></tr>";
    else
      s += "<tr><td>" + String(r.name) + "</td><td style='color:#aaa;'>n/a</td></tr>";
  }
  s += "</table></div></div>";
  return s;
}

static String analyzeTelegram(uint8_t* buffer, size_t length)
{
  String s;
  s.reserve(4000);
  s += R"rawliteral(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>SmartMeterLite &ndash; Telegram Analysis</title>)rawliteral";
  s += HTML_STYLE_MODERN;
  s += R"rawliteral(</head>
<body>
<div class="logo">&#9889; SmartMeterLite</div>
<a class="back" href="/sysinfo">&#8592; Zur&uuml;ck</a>
)rawliteral";
  s += buildCommonSection(buffer, length);
  if (LastMeterValue.timestamp > 0) {
    if (length > 0 && buffer[0] == '/')
      s += analyzeIEC_section(buffer, length);
    else
      s += analyzeSML_section(buffer, length);
  }
  s += "</body></html>";
  return s;
}

// ---------------------------------------------------------------------------
// Public route handlers
// ---------------------------------------------------------------------------

void Webserver_Telegram_Analysis()
{
  server.send(200, "text/html", analyzeTelegram(telegram_receive_buffer, TELEGRAM_LENGTH));
}

void Webserver_MeterValue_Num2()
{
  String s = "<html><head><title>SMGWLite - Alternative Amount Meter Value</title> " + String(HTML_STYLE) + "</head><body>" + String(MeterValue_Num2()) + "</body></html>";
  server.send(200, "text/html", s);
}

// Renders the packed ring-buffer as an HTML table (chunked streaming).
void Webserver_ShowMeterValues()
{
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  String s;
  s.reserve(1000);
  s = R"rawliteral(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>SmartMeterLite &ndash; Meter Values</title>)rawliteral";
  s += HTML_STYLE_MODERN;
  s += R"rawliteral(</head>
<body>
<div class="logo">&#9889; SmartMeterLite</div>
<a class="back" href="/sysinfo">&#8592; Zur&uuml;ck</a>
<div class="card" style="max-width:800px;">
<div class="card-title">Meter Values</div>
<p class="hint">Entry size: )rawliteral";
  s += String(MeterValue_EntrySize()) + " bytes | ";
  s += MeterValue_BuildFieldsParam();
  s += R"rawliteral(</p>
<table><tr><th>Index</th><th>Count</th><th>Timestamp</th><th>Unix</th><th>Consumption (1.8.0)</th>)rawliteral";
  if (config_temperature_enabled) s += "<th>Temperature</th>";
  if (config_solar_enabled)       s += "<th>myStrom (solar)</th>";
  if (config_obis280_enabled)     s += "<th>Infeed (2.8.0)</th>";
  s += "</tr>";
  server.sendContent(s);

  int count = 1;
  bool first = true;
  for (int m = 0; m < Meter_Value_Buffer_Size; m++)
  {
    uint32_t ts, m180, temp, solar, m280;
    MeterValue_read(m, ts, m180, temp, solar, m280);
    if (ts == 0 && m180 == 0) {
      if (first) { first = false; server.sendContent("<tr><td>-----</td></tr>"); }
      continue;
    }
    String row = "<tr><td>" + String(m) + "</td><td>" + String(count++) + "</td>";
    row += "<td>" + String(Time_formatTimestamp(ts)) + "</td><td>" + String(ts) + "</td>";
    row += "<td>" + String(m180) + "</td>";
    if (config_temperature_enabled) row += "<td>" + String(temp)  + "</td>";
    if (config_solar_enabled)       row += "<td>" + String(solar) + "</td>";
    if (config_obis280_enabled)     row += "<td>" + String(m280)  + "</td>";
    row += "</tr>";
    server.sendContent(row);
  }

  server.sendContent("</table></div></body></html>");
  server.sendContent("");
}

void Webserver_ShowLogBuffer()
{
  server.send(200, "text/html", Log_BufferToString());
}

void Webserver_ShowTelegram_Raw()
{
  String s;
  s.reserve(TELEGRAM_LENGTH * 12 + 1024);
  s = R"rawliteral(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>SmartMeterLite &ndash; Telegram Raw</title>)rawliteral";
  s += HTML_STYLE_MODERN;
  s += R"rawliteral(</head>
<body>
<div class="logo">&#9889; SmartMeterLite</div>
<a class="back" href="/sysinfo">&#8592; Zur&uuml;ck</a>
<div class="card">
<div class="card-title">Receive Buffer &ndash; Decimal</div>
<textarea rows="8">)rawliteral";
  for (int i = 0; i < TELEGRAM_LENGTH; i++) { if (i > 0) s += " "; s += String(telegram_receive_buffer[i]); }
  s += R"rawliteral(</textarea>
</div>
<div class="card">
<div class="card-title">Receive Buffer &ndash; Hex</div>
<textarea rows="8">)rawliteral";
  for (int i = 0; i < TELEGRAM_LENGTH; i++) { if (i > 0) s += " "; s += String(telegram_receive_buffer[i], HEX); }
  s += R"rawliteral(</textarea>
</div>
<div class="card">
<div class="card-title">Receive Buffer &ndash; ASCII</div>
<textarea rows="8">)rawliteral";
  for (int i = 0; i < TELEGRAM_LENGTH; i++)
  {
    char c = (char)telegram_receive_buffer[i];
    s += (isPrintable(c) || c == '\n' || c == '\r') ? String(c) : ".";
  }
  s += R"rawliteral(</textarea>
</div>
</body></html>)rawliteral";
  server.send(200, "text/html", s);
}

void Webserver_ShowTelegram()
{
  if (iotWebConf.handleCaptivePortal()) return;

  String s;
  s.reserve(TELEGRAM_LENGTH * 45 + 1500);
  s = R"rawliteral(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>SmartMeterLite &ndash; Telegram</title>)rawliteral";
  s += HTML_STYLE_MODERN;
  s += R"rawliteral(</head>
<body>
<div class="logo">&#9889; SmartMeterLite</div>
<a class="back" href="/sysinfo">&#8592; Zur&uuml;ck</a>
<div class="card" style="max-width:420px;">
<div class="card-title">Telegram</div>
<div class="kv"><span class="kl">Last byte received</span>)rawliteral";
  s += String(millis() - lastByteTime) + " ms ago";
  s += R"rawliteral(</div>
<div class="kv last"><span class="kl">Last complete telegram</span>)rawliteral";
  if (LastMeterValue.timestamp > 0)
    s += Time_formatTimestamp(LastMeterValue.timestamp) + " (" + String(Time_getEpochTime() - LastMeterValue.timestamp) + " s ago)";
  else
    s += "&#x2013;";
  s += R"rawliteral(</div>
</div>
<div class="card" style="max-width:420px;">
<div class="card-title">Receive Buffer (Hex)</div>
<div class="tbl"><table><tr><th>Index</th><th>Byte</th></tr>)rawliteral";

  String color;
  int signature_7101 = 9999;
  int k = 0;
  for (int i = 0; i < TELEGRAM_LENGTH; i++)
  {
    if (i >= k && i < TELEGRAM_LENGTH - 5 &&
        telegram_receive_buffer[i-k]   == 7 && telegram_receive_buffer[i+1-k] == 1 &&
        telegram_receive_buffer[i+2-k] == 0 && telegram_receive_buffer[i+3-k] == 1 &&
        telegram_receive_buffer[i+4-k] == 8)
    {
      color = "bgcolor=959018"; signature_7101 = i; if (k < 5) k++;
    }
    else if (i > signature_7101 && telegram_receive_buffer[i] == 0x77)
    {
      signature_7101 = 9999; color = "bgcolor=959018";
    }
    else color = "";
    s += "<tr><td>" + String(i) + "</td><td " + String(color) + ">" + String(telegram_receive_buffer[i], HEX) + "</td></tr>";
  }
  s += R"rawliteral(</table></div>
</div>
</body></html>)rawliteral";
  server.send(200, "text/html", s);
}

void Webserver_ShowLastMeterValue()
{
  JsonDocument jsonDoc;
  jsonDoc["meter_value_180"] = LastMeterValue.meter_value_180;
  jsonDoc["timestamp"]       = LastMeterValue.timestamp;
  jsonDoc["temperature"]     = LastMeterValue.temperature;
  jsonDoc["solar"]           = LastMeterValue.solar;
  jsonDoc["meter_value_280"] = LastMeterValue.meter_value_280;
  jsonDoc["power_import"]    = LastMeterValue.power_import;
  jsonDoc["power_export"]    = LastMeterValue.power_export;
  jsonDoc["net_power"]       = LastMeterValue.net_power;
  jsonDoc["last_byte_age_s"]  = lastByteTime > 0 ? (unsigned long)(millis() - lastByteTime) / 1000 : 9999;
  jsonDoc["wifi_connected"]   = wifi_connected;
  jsonDoc["backend_called"]   = last_call_backend > 0;
  jsonDoc["backend_ok"]       = call_backend_successfull;
  jsonDoc["backend_ago_min"]  = (last_call_backend > 0) ? (millis() - last_call_backend) / 60000UL : 0UL;

  String jsonResponse;
  serializeJson(jsonDoc, jsonResponse);
  server.send(200, "application/json", jsonResponse);
}
