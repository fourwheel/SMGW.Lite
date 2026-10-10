// Backend client: meter value and log uploads (tasks, request, acknowledgement).
#include "backend_client.h"
#include "app_globals.h"
#include "meter_value.h"
#include "log_buffer.h"
#include "debug_log.h"
#include "time_utils.h"
#include "serial_scan.h"
#include "ota_pull.h"
#include "version.h"
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

static TaskHandle_t h_meter_task = NULL;
static TaskHandle_t h_log_task   = NULL;

void Webclient_splitHostAndPath(const String &url, String &host, String &path)
{
  int slashIndex = url.indexOf('/');
  if (slashIndex == -1) { host = url; path = "/"; }
  else { host = url.substring(0, slashIndex); path = url.substring(slashIndex); }
}

String backend_host;
String backend_path;

void Webclient_Send_Meter_Values_to_backend_Task(void *pvParameters)
{
  if (ota_active) { h_meter_task = NULL; vTaskDelete(NULL); return; }
  if (xSemaphoreTake(Sema_Backend, portMAX_DELAY))
  {
    // Timed from here, not from the spawn: waiting for another holder of
    // Sema_Backend is not this task's hang, and every holder is watched
    // itself (the other upload task, or loop() for OTA and web pages).
    g_meter_task_started = millis();
#ifdef TEST_SELF_RECOVERY
    if (g_test_hang_upload) for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
#endif
    if (!ota_active) Webclient_send_meter_values_to_backend();
    xSemaphoreGive(Sema_Backend);
  }
  watermark_meter_buffer = uxTaskGetStackHighWaterMark(NULL);
  g_meter_task_started = 0;
  h_meter_task = NULL;
  vTaskDelete(NULL);
}

void Webclient_Send_Log_to_backend_Task(void *pvParameters)
{
  if (ota_active) { h_log_task = NULL; vTaskDelete(NULL); return; }
  if (xSemaphoreTake(Sema_Backend, portMAX_DELAY))
  {
    g_log_task_started = millis(); // see Webclient_Send_Meter_Values_to_backend_Task()
    if (!ota_active)
    {
      // Cleared right before the buffer is copied: entries logged from now on
      // set it again (Log_Add) and go out with the next upload. Not
      // cleared when skipped for OTA, so the pending log isn't lost.
      b_send_log_to_backend = false;
      Webclient_send_log_to_backend();
    }
    xSemaphoreGive(Sema_Backend);
  }
  watermark_log_buffer = uxTaskGetStackHighWaterMark(NULL);
  g_log_task_started = 0;
  h_log_task = NULL;
  vTaskDelete(NULL);
}

void Webclient_Send_Meter_Values_to_backend_wrapper()
{
  if (h_meter_task != NULL) { DLOGLN("Meter task still running, skipping spawn"); return; }
  xTaskCreate(Webclient_Send_Meter_Values_to_backend_Task, "Send_Meter task", 8192, NULL, 2, &h_meter_task);
}

void Webclient_Send_Log_to_backend_wrapper()
{
  if (h_log_task != NULL) { DLOGLN("Log task still running, skipping spawn"); return; }
  xTaskCreate(Webclient_Send_Log_to_backend_Task, "send log task", 8192, NULL, 2, &h_log_task);
}

// ---------------------------------------------------------------------------
// Webclient_crc32
// CRC-32 (IEEE 802.3), identical to PHP hash('crc32b') and zlib crc32().
// Chainable: start with crc = 0 and pass the previous result for the next block.
// ---------------------------------------------------------------------------
static uint32_t Webclient_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
  crc = ~crc;
  while (len--)
  {
    crc ^= *data++;
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1)));
  }
  return ~crc;
}

// ---------------------------------------------------------------------------
// Webclient_read_response
// Reads a backend response: returns the HTTP status code (0 if none arrived
// in time) and the body. Header lines are offered to the HTTP Date time
// fallback. The body is read byte-wise up to Content-Length (or until the
// server closes), so no read waits for a trailing newline. Chunked transfer
// encoding is not decoded — the backend always sends Content-Length.
// ---------------------------------------------------------------------------
int Webclient_read_response(WiFiClientSecure &client, String &body, unsigned long timeout_ms)
{
  const unsigned int MAX_BODY = 1024; // backend responses are < 300 bytes
  int  status         = 0;
  long content_length = -1;
  bool headers_done   = false;
  body = "";
  unsigned long deadline = millis() + timeout_ms;

  while (millis() < deadline)
  {
    if (headers_done && (body.length() >= MAX_BODY ||
                         (content_length >= 0 && (long)body.length() >= content_length))) break;
    if (!client.available())
    {
      if (!client.connected()) break;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (headers_done) { body += (char)client.read(); continue; }

    String line = client.readStringUntil('\n'); // header lines always end with \r\n
    DLOGLN(line);
    line.trim();
    if (status == 0)
    {
      if (line.startsWith("HTTP/")) status = line.substring(line.indexOf(' ') + 1).toInt();
      continue;
    }
    if (line.isEmpty())
    {
      headers_done = true;
      if (status != 200) break; // error bodies are not evaluated
      continue;
    }
    if (line.substring(0, 15).equalsIgnoreCase("Content-Length:")) content_length = line.substring(15).toInt();
    Time_setFromHttpDate(line); // no-op once NTP is synced or for non-Date lines
  }
  return status;
}

// ---------------------------------------------------------------------------
// Webclient_ack_valid
// True if a backend response acknowledges exactly the payload that was sent:
// "bytes" and "crc32" (see payload_ack() in the backend) must match. A server
// that answers HTTP 200 without running the backend script cannot produce
// this, so the device never discards data on a bare 200.
// ---------------------------------------------------------------------------
static bool Webclient_ack_valid(JsonDocument &doc, size_t bytes, uint32_t crc)
{
  char crc_hex[9];
  snprintf(crc_hex, sizeof(crc_hex), "%08lx", (unsigned long)crc);
  return (doc["bytes"] | -1L) == (long)bytes && strcmp(doc["crc32"] | "", crc_hex) == 0;
}

void Webclient_send_log_to_backend()
{
  if (backend_host.isEmpty()) { DLOGLN("No backend host configured, skipping"); Log_AddWithoutTransmit(1023); return; }
  DLOGLN("Send Log to Backend");
  Log_AddWithoutTransmit(1019);
  WiFiClientSecure client;
  client.setHandshakeTimeout(10); // 10 s SSL handshake timeout
  if (UseSslCert_object.isChecked()) client.setCACert(FullCert);
  else client.setInsecure();

  if (!client.connect(backend_host.c_str(), 443, 10000)) { DLOGLN("Connection to server failed"); Log_Add(4000); call_backend_successfull = false; return; }

  uint8_t *logDataBuffer = (uint8_t *)malloc(LOG_BUFFER_SIZE * sizeof(LogEntry));
  if (!logDataBuffer) { DLOGLN("Log buffer allocation failed"); Log_Add(1002); call_backend_successfull = false; return; }
  // Regular uploads carry only the entries since the last acknowledged upload;
  // /sendLog_Task and /sendboth_Task request the whole ring (g_log_upload_full).
  bool full = g_log_upload_full;
  g_log_upload_full = false;
  uint32_t logUpto;
  size_t logBufferSize = Log_CopyForUpload(logDataBuffer, full, logUpto);

  String logHeader  = "POST " + String(backend_path) + "log.php";
  logHeader += "?ID=" + String(backend_ID) + "&IP=" + String(IPlastOctet);
  logHeader += "&serial=" + SerialScan_activeLabel();
  logHeader += "&fw=" + String(FIRMWARE_VERSION);
  logHeader += "&cfg=" + String(CONFIG_VERSION);
  if (!meter_model.isEmpty()) {
    String encoded = meter_model;
    encoded.replace(" ", "%20");
    logHeader += "&model=" + encoded;
  }
  logHeader += " HTTP/1.1\r\nHost: " + backend_host + "\r\n";
  logHeader += "X-Auth-Token: " + String(backend_token) + "\r\n";
  logHeader += "Content-Type: application/octet-stream\r\n";
  logHeader += "Content-Length: " + String(logBufferSize) + "\r\n";
  logHeader += "Connection: close\r\n\r\n";
  DLOGLN(logHeader);

  client.print(logHeader);
  client.write(logDataBuffer, logBufferSize);
  uint32_t logCrc = Webclient_crc32(0, logDataBuffer, logBufferSize);
  free(logDataBuffer);

  // The log only counts as sent if the backend acknowledges exactly this payload.
  String body;
  int  status = Webclient_read_response(client, body, 15000);
  bool logOk  = false;
  if (status == 200)
  {
    JsonDocument doc;
    logOk = deserializeJson(doc, body) == DeserializationError::Ok &&
            Webclient_ack_valid(doc, logBufferSize, logCrc);
    if (!logOk) Log_Add(4005);
  }
  else Log_Add(4003);

  // No b_send_log_to_backend = false here: the task cleared it before the
  // buffer was copied; an entry logged since then must keep it set.
  if (logOk) { DLOGLN("Log successfully sent"); Log_MarkSent(logUpto); Log_AddWithoutTransmit(1020); last_backend_success = millis(); }
  else b_send_log_to_backend = true;
  call_backend_successfull = logOk;
  client.stop();
}

// ---------------------------------------------------------------------------
// Webclient_send_meter_values_to_backend  (Written by Claude)
// Sends the entire packed ring-buffer to the PHP backend in a single POST.
//
// Wire format declaration:
//   The URL parameter "fields=ts,m180[,temp][,solar][,m280]" tells the backend
//   exactly which fields are present in each binary entry and in what order.
//   This replaces the old separate PV_included / 280_included flags and is
//   self-describing — adding a new field in the future only requires appending
//   a new token here and in the PHP parser.
//
// The buffer is already in the correct wire format, so it is sent directly
// without any intermediate copy.
//
// The buffer is only cleared if the backend acknowledges exactly this payload
// (bytes + CRC32 in the JSON response). The URL also carries fw/hw, so a
// confirmed response may contain an fw_update offer for OtaPull.
// ---------------------------------------------------------------------------
void Webclient_send_meter_values_to_backend()
{
  Log_AddWithoutTransmit(1005);
  Log_AddWithoutTransmit(LOG_VALUE_COUNT_BASE + MeterValue_Num());
  DLOGLN("call_backend_V2");
  last_call_backend = millis();

  if (backend_host.isEmpty()) { DLOGLN("No backend host configured, skipping"); Log_AddWithoutTransmit(1023); call_backend_successfull = true; last_backend_success = millis(); return; }
  // No early return without values: the call (with an empty payload) is also
  // how the device learns about an assigned firmware update and shows up as
  // alive in the backend, even if it receives no telegrams.

  WiFiClientSecure client;
  // connect(..., 10000) below sets the 10 s socket timeout (each TLS write);
  // the TLS handshake has its own limit, 120 s by default.
  client.setHandshakeTimeout(30); // seconds
  if (UseSslCert_object.isChecked()) client.setCACert(FullCert);
  else client.setInsecure();

  if (!client.connect(backend_host.c_str(), 443, 10000)) { DLOGLN("Connection to server failed"); Log_Add(4000); call_backend_successfull = false; return; }

  // Hold the buffer lock from computing the ranges until the buffer is cleared
  // (or the send failed). handle_MeterValue_store() defers new stores meanwhile.
  xSemaphoreTake(Sema_MeterBuffer, portMAX_DELAY);

  // Determine which byte ranges of the buffer actually contain data.
  //
  // Buffer layout:
  //   [0 .. override_i-1]              TAF7  entries (ascending from front)
  //   [override_i .. NON_override_i]   empty gap
  //   [NON_override_i+1 .. Size-1]     TAF14 entries (descending from back)
  //
  // When the buffer is full/overflowed the two pointers have crossed and
  // there is no gap — send the entire buffer as one contiguous block.
  const size_t entrySize = MeterValue_EntrySize();
  size_t taf7_bytes, taf14_offset, taf14_bytes;

  if (meter_value_buffer_full || meter_value_buffer_overflow)
  {
    taf7_bytes   = (size_t)Meter_Value_Buffer_Size * entrySize;
    taf14_offset = 0;
    taf14_bytes  = 0;
  }
  else
  {
    taf7_bytes   = (size_t)meter_value_override_i * entrySize;
    taf14_offset = (size_t)(meter_value_NON_override_i + 1) * entrySize;
    taf14_bytes  = (size_t)(Meter_Value_Buffer_Size - 1 - meter_value_NON_override_i) * entrySize;
  }
  size_t totalPayload = taf7_bytes + taf14_bytes;

  String header  = "POST " + String(backend_path);
  header += "?ID=" + String(backend_ID);
  header += "&chipTemp=" + String(temperatureRead(), 1);
  header += "&uptime=" + String(millis() / 60000);
  header += "&time=" + String(Time_getFormattedTime());
  // Self-describing field manifest — the backend uses this to parse the binary payload.
  // Format: fields=ts,m180[,temp][,solar][,m280]
  header += "&" + MeterValue_BuildFieldsParam();
  header += "&heap=" + String(ESP.getFreeHeap());
  header += "&transmittedValues=" + String(MeterValue_Num());
  header += "&IP=" + String(IPlastOctet);
  header += "&fw=" FIRMWARE_VERSION "&hw=" FIRMWARE_TARGET; // backend answers with fw_update if another version is assigned
  header += " HTTP/1.1\r\n";
  header += "Host: " + backend_host + "\r\n";
  header += "X-Auth-Token: " + String(backend_token) + "\r\n";
  header += "Content-Type: application/octet-stream\r\n";
  header += "Content-Length: " + String(totalPayload) + "\r\n";
  header += "Connection: close\r\n\r\n";

  client.print(header);

  // Send filled ranges in 1 KB chunks so partial TLS writes don't silently
  // drop the tail. WiFiClientSecure::write() may return less than requested.
  // sendRange() logs 4001 and returns false if write() returns 0 (connection
  // lost mid-transfer). Abort without clearing the buffer so data is retried.
  // The CRC32 of everything sent is compared with the backend acknowledgement.
  uint32_t crc = 0;
  auto sendRange = [&](const uint8_t* data, size_t len) -> bool {
    const size_t CHUNK = 1024;
    size_t offset = 0;
    while (offset < len)
    {
      size_t toSend = min(CHUNK, len - offset);
      size_t sent   = client.write(data + offset, toSend);
      if (sent == 0) { DLOGLN("write() returned 0 — aborting"); Log_Add(4001); return false; }
      crc = Webclient_crc32(crc, data + offset, sent);
      offset += sent;
    }
    return true;
  };

  // TAF7 range (front of buffer), then TAF14 range (back of buffer).
  // The backend sorts all entries by timestamp, so the order here doesn't matter.
  if ((taf7_bytes  > 0 && !sendRange(MeterValueBuffer,                taf7_bytes)) ||
      (taf14_bytes > 0 && !sendRange(MeterValueBuffer + taf14_offset, taf14_bytes)))
  {
    xSemaphoreGive(Sema_MeterBuffer);
    client.stop();
    call_backend_successfull = false;
    return;
  }

  // Clear the buffer only if the backend acknowledges exactly this payload —
  // HTTP 200 alone is not enough (a misconfigured server may answer 200
  // without storing anything).
  String body;
  int  status = Webclient_read_response(client, body, 15000);
  bool ok     = false;
  JsonDocument doc;
  if (status == 200)
  {
    ok = deserializeJson(doc, body) == DeserializationError::Ok &&
         Webclient_ack_valid(doc, totalPayload, crc);
    if (!ok) Log_Add(4004);
  }
  else Log_Add(4002);

  if (ok)
  {
    DLOGLN("MeterValues successfully sent");
    MeterValues_clear_Buffer();
    last_call_backend    = millis();
    last_backend_success = millis();
    Log_AddWithoutTransmit(1021);
  }
  xSemaphoreGive(Sema_MeterBuffer);

  // Only a confirmed response may offer a firmware update. Every caller of
  // this function holds Sema_Backend, as OtaPull_setOffer() requires.
  if (ok) OtaPull_setOffer(doc["fw_update"]);

  call_backend_successfull = ok;
  client.stop();
}
