// Telegram reception (telegramTask) and the SML / IEC 62056-21 parsers.
#include "telegram.h"
#include "app_globals.h"
#include "meter_value.h"
#include "log_buffer.h"
#include "debug_log.h"
#include "time_utils.h"
#include "serial_scan.h"
#include "soc/uart_reg.h"  // UART_INT_RAW_REG / UART_INT_CLR_REG for parity-error detection
#include <ArduinoJson.h>

// Telegram vars
#define TELEGRAM_TIMEOUT_MS 30                    // timeout for telegram in ms
uint8_t telegram_receive_buffer[TELEGRAM_LENGTH]; // buffer for serial data
size_t telegram_receive_bufferIndex = 0;          // position in serial data buffer
unsigned long lastByteTime = 0;                   // timestamp of last received byte
unsigned long timestamp_telegram;                 // timestamp of telegram

unsigned long last_remote_meter_value = 0;
bool startup_print_done              = false; // one-time diagnostic after first telegram
unsigned long last_telegram_parsed      = 0; // millis() of last successfully parsed telegram; initialised in setup()
unsigned long last_silence_log            = 0; // millis() 3005/3007 were last logged, see handle_telegram_watchdog()

// Finds an OBIS code in an SML buffer and returns the raw integer value
// and its SML scaler byte. The raw value is sign-extended for signed types (0x5x).
// Returns false if the OBIS code is not found or the value type is unsupported.
bool obisExtractRaw(uint8_t* buffer, int px, int sx, uint8_t* code,
                    uint64_t* raw_out, int8_t* scaler_out) {
  for (int i = px; i < sx - 12; i++) {
    if (memcmp(&buffer[i], code, 6) == 0) {
      for (int j = i + 6; j < i + 40 && j < sx; j++) {
        if (buffer[j] == 0x52) {
          int8_t  scaler    = (int8_t)buffer[j + 1];
          uint8_t typeByte  = buffer[j + 2];
          uint8_t typeGroup = typeByte & 0xF0;
          if (typeGroup == 0x50 || typeGroup == 0x60) {
            int vLen   = (typeByte & 0x0F) - 1;
            int vStart = j + 3;
            if (vStart + vLen > sx || vLen <= 0 || vLen > 8) continue;
            uint64_t raw64 = 0;
            for (int k = 0; k < vLen; k++) raw64 = (raw64 << 8) | buffer[vStart + k];
            if (typeGroup == 0x50) {
              uint64_t signBit = 1ULL << (vLen * 8 - 1);
              if (raw64 & signBit)
                raw64 |= ~((signBit << 1) - 1);
            }
            *raw_out    = raw64;
            *scaler_out = scaler;
            return true;
          }
        }
      }
    }
  }
  return false;
}

// Converts a raw SML value to 0.1 Wh (the DB energy storage unit).
// SML scaler s: raw is in 10^s Wh → multiply by 10^(s+1) to reach 10^-1 Wh.
static uint32_t smlToDeciWh(uint64_t raw, int8_t scaler) {
  int8_t adj = scaler + 1;
  if (adj > 0) for (int8_t i = 0; i < adj;  i++) raw *= 10;
  if (adj < 0) for (int8_t i = 0; i > adj; i--) raw /= 10;
  return (uint32_t)raw;
}

// Converts a raw SML value to integer watts.
// SML scaler s: raw is in 10^s W → apply 10^s to reach W.
static int32_t smlToWatt(uint64_t raw_u, int8_t scaler) {
  int64_t raw = (int64_t)raw_u;
  if (scaler > 0) for (int8_t i = 0; i < scaler;  i++) raw *= 10;
  if (scaler < 0) for (int8_t i = 0; i > scaler; i--) raw /= 10;
  return (int32_t)raw;
}

// ---------------------------------------------------------------------------
// Protocol detection and unified parser  (Written by Claude)
//
// Both parsers now share the same signature:
//   bool Telegram_parse_*(uint8_t* buffer, size_t length)
//   Returns: true  = meter value successfully extracted into LastMeterValue
//            false = telegram not recognised or parse error
//
// Protocol detection heuristics:
//   SML: starts with 0x1b 0x1b 0x1b 0x1b (binary escape sequence)
//   IEC: starts with '/' (ASCII 0x2F) — IEC 62056-21 mode C identifier
//
// The detected protocol is stored in last_detected_protocol for display
// in the webserver "Telegram Parse Config" section.
// ---------------------------------------------------------------------------

// Tracks which protocol was last successfully detected

TelegramProtocol last_detected_protocol = TelegramProtocol::UNKNOWN;
TelegramProtocol prev_detected_protocol = TelegramProtocol::UNKNOWN;

// Returns a human-readable string for the detected protocol
String Telegram_protocol_to_string(TelegramProtocol p)
{
  switch (p) {
    case TelegramProtocol::SML: return "SML (detected)";
    case TelegramProtocol::IEC: return "IEC 62056-21 (detected)";
    default:                    return "Unknown (no valid telegram yet)";
  }
}

/**
 * SML parser — extracts OBIS 1.8.0 and optionally 2.8.0 into LastMeterValue.
 * This function is written by Gemini3, signature unified by Claude.
 */
bool Telegram_parse_SML(uint8_t* buffer, size_t length)
{
  // 1. Locate Prefix
  int px = -1;
  for (int i = 0; i < (int)length - 4; i++) {
    if (buffer[i] == 0x1b && buffer[i+1] == 0x1b && buffer[i+2] == 0x1b && buffer[i+3] == 0x1b) { px = i; break; }
  }
  if (px == -1) return false;

  // 2. Locate Suffix
  int sx = -1;
  for (int i = px; i < (int)length - 5; i++) {
    if (buffer[i] == 0x1b && buffer[i+1] == 0x1b && buffer[i+2] == 0x1b && buffer[i+3] == 0x1b && buffer[i+4] == 0x1a) { sx = i; break; }
  }
  if (sx == -1) return false;

  // 3a. Extract meter serial (OBIS 0-0:96.1.0) for meter_model if not yet known
  if (meter_model.isEmpty()) {
    String model; // built locally, assigned once - other tasks read meter_model
    uint8_t obis960[] = {0x01, 0x00, 0x60, 0x01, 0x00, 0xff};
    for (int i = px; i < sx - 12; i++) {
      if (memcmp(&buffer[i], obis960, 6) == 0) {
        for (int j = i + 6; j < i + 40 && j < sx; j++) {
          uint8_t tl = buffer[j];
          if ((tl & 0xF0) == 0x00 && (tl & 0x0F) > 1) {
            int slen = (tl & 0x0F) - 1;
            if (j + slen >= sx) break;
            // Look for 3 consecutive uppercase letters (manufacturer code)
            for (int k = 0; k < slen - 2; k++) {
              uint8_t a = buffer[j+1+k], b = buffer[j+2+k], c = buffer[j+3+k];
              if (a >= 'A' && a <= 'Z' && b >= 'A' && b <= 'Z' && c >= 'A' && c <= 'Z') {
                model = String((char)a) + String((char)b) + String((char)c);
                break;
              }
            }
            if (model.isEmpty()) {
              char hex[3];
              for (int k = 0; k < slen; k++) {
                sprintf(hex, "%02X", buffer[j+1+k]);
                if (k) model += ' ';
                model += hex;
              }
            }
            break;
          }
        }
        break;
      }
    }
    meter_model = model;
  }

  // 3b. Extract OBIS Data
  uint8_t obis180[] = {0x01, 0x00, 0x01, 0x08, 0x00, 0xff};
  uint8_t obis280[] = {0x01, 0x00, 0x02, 0x08, 0x00, 0xff};
  uint8_t obis170[] = {0x01, 0x00, 0x01, 0x07, 0x00, 0xff};
  uint8_t obis270[] = {0x01, 0x00, 0x02, 0x07, 0x00, 0xff};
  uint8_t obis167[] = {0x01, 0x00, 0x10, 0x07, 0x00, 0xff};
  uint32_t temp180 = 0, temp280 = 0, temp170 = 0, temp270 = 0, temp167 = 0;
  uint64_t raw; int8_t sc;
  bool found180 = obisExtractRaw(buffer, px, sx, obis180, &raw, &sc);
  if (found180) temp180 = smlToDeciWh(raw, sc);
  bool found280 = obisExtractRaw(buffer, px, sx, obis280, &raw, &sc);
  if (found280) temp280 = smlToDeciWh(raw, sc);
  if (obisExtractRaw(buffer, px, sx, obis170, &raw, &sc)) temp170 = (uint32_t)smlToWatt(raw, sc);
  if (obisExtractRaw(buffer, px, sx, obis270, &raw, &sc)) temp270 = (uint32_t)smlToWatt(raw, sc);
  if (obisExtractRaw(buffer, px, sx, obis167, &raw, &sc)) temp167 = (uint32_t)smlToWatt(raw, sc);

  // Only update globals if the main consumption register was found.
  // All other fields are optional — some meters don't transmit them.
  //
  // Build the update in a local struct first, then assign to LastMeterValue in
  // one shot. This avoids a SMP race condition: telegramTask runs on Core 0
  // while the webserver / MeterValue_store run on Core 1. The old approach
  // called resetMeterValue() (which zeroed meter_value_180) and then assigned
  // temp180 in a separate step, leaving a brief window where meter_value_180 == 0
  // was visible to the other core — causing the REST API to occasionally return 0.
  if (found180 && temp180 > 0) {
    if (PrevMeterValue.meter_value_180 > 0 && temp180 < PrevMeterValue.meter_value_180)
      Log_Add(1208);
    if (found280 && PrevMeterValue.meter_value_280 > 0 && temp280 < PrevMeterValue.meter_value_280)
      Log_Add(1208);
    MeterValue newVal = {};
    // Preserve solar if MyStrom is active (mirrors resetMeterValue logic)
    if (mystrom_PV_object.isChecked()) newVal.solar = LastMeterValue.solar;
    // Always preserve the externally-sourced temperature reading
    newVal.temperature     = LastMeterValue.temperature;
    newVal.meter_value_180 = temp180;
    newVal.meter_value_280 = found280 ? temp280 : 0;
    newVal.power_import    = temp170;
    newVal.power_export    = temp270;
    newVal.net_power       = (int32_t)temp167;
    newVal.timestamp       = Time_getEpochTime();
    // Single locked assignment: readers never see meter_value_180 at 0 or a
    // mix of the old and the new telegram.
    MeterValue_setLast(newVal);
    return true;
  }
  return false;
}

/**
 * IEC 62056-21 parser — extracts OBIS 1.8.0 and optionally 2.8.0 into LastMeterValue.
 * This function is written by ChatGPT, extended and signature unified by Claude.
 */
bool Telegram_parse_IEC(uint8_t* buffer, size_t length)
{
  static char telegram_str[TELEGRAM_LENGTH + 1];
  size_t copy_len = (length <= TELEGRAM_LENGTH) ? length : TELEGRAM_LENGTH;
  memcpy(telegram_str, buffer, copy_len);
  telegram_str[copy_len] = '\0';

  // Extract meter model from identification line (/<MFR><baud><ident>) if not yet known
  if (meter_model.isEmpty() && telegram_str[0] == '/') {
    String model; // built locally, assigned once - other tasks read meter_model
    const char *eol = strstr(telegram_str, "\r\n");
    size_t lineLen = eol ? (size_t)(eol - telegram_str) : 0;
    if (lineLen >= 4) {
      char mfr[4]; strncpy(mfr, telegram_str + 1, 3); mfr[3] = '\0';
      model = mfr;
      if (lineLen > 5) {
        char ident[64];
        size_t identLen = lineLen - 5 < sizeof(ident) - 1 ? lineLen - 5 : sizeof(ident) - 1;
        strncpy(ident, telegram_str + 5, identLen); ident[identLen] = '\0';
        model += ' '; model += ident;
      }
    }
    meter_model = model;
  }

  // Extract OBIS 1.8.0 (consumption) — required
  const char *obis180 = strstr(telegram_str, "1-0:1.8.0");
  if (!obis180) return false;

  const char *openParen = strchr(obis180, '(');
  const char *star      = (openParen) ? strchr(openParen, '*') : nullptr;
  if (!openParen || !star || openParen > star) return false;

  char valueStr[16];
  size_t len = star - openParen - 1;
  if (len >= sizeof(valueStr)) return false;

  strncpy(valueStr, openParen + 1, len);
  valueStr[len] = '\0';
  for (int i = 0; valueStr[i]; i++) if (valueStr[i] == ',') valueStr[i] = '.';

  float kWh180 = atof(valueStr);
  if (kWh180 <= 0.0f) return false; // implausible value

  // Helper lambda: find OBIS label in IEC text, parse the float value after '('
  auto parseIecObis = [&](const char* label, float* out) -> bool {
    const char *p = strstr(telegram_str, label);
    if (!p) return false;
    const char *op = strchr(p, '(');
    const char *st = op ? strchr(op, '*') : nullptr;
    if (!op || !st || op >= st) return false;
    char buf[20];
    size_t l = st - op - 1;
    if (l >= sizeof(buf)) return false;
    strncpy(buf, op + 1, l);
    buf[l] = '\0';
    for (int i = 0; buf[i]; i++) if (buf[i] == ',') buf[i] = '.';
    *out = atof(buf);
    return true;
  };

  uint32_t new180 = (uint32_t)(kWh180 * 10000.0f);
  if (PrevMeterValue.meter_value_180 > 0 && new180 < PrevMeterValue.meter_value_180)
    Log_Add(1208);

  float v280_raw = 0.0f;
  bool has280 = parseIecObis("1-0:2.8.0", &v280_raw);
  uint32_t new280 = has280 ? (uint32_t)(v280_raw * 10000.0f) : 0;
  if (has280 && PrevMeterValue.meter_value_280 > 0 && new280 < PrevMeterValue.meter_value_280)
    Log_Add(1208);

  float v170 = 0.0f, v270 = 0.0f, v167 = 0.0f;
  parseIecObis("1-0:1.7.0",  &v170);
  parseIecObis("1-0:2.7.0",  &v270);
  parseIecObis("1-0:16.7.0", &v167);

  // Build update in a local struct first, then assign to LastMeterValue in one shot.
  // Avoids the SMP race window where resetMeterValue() zeroes meter_value_180 and the
  // other core sees 0 before the new value is written (same fix as in Telegram_parse_SML).
  MeterValue newVal = {};
  if (mystrom_PV_object.isChecked()) newVal.solar = LastMeterValue.solar;
  newVal.temperature     = LastMeterValue.temperature;
  newVal.meter_value_180 = new180;
  newVal.meter_value_280 = has280 ? new280 : 0;
  newVal.timestamp       = Time_getEpochTime();
  newVal.power_import    = (uint32_t)(v170 * 1000.0f);
  newVal.power_export    = (uint32_t)(v270 * 1000.0f);
  newVal.net_power       = (int32_t)(v167 * 1000.0f);
  MeterValue_setLast(newVal);

  return true;
}

int32_t MeterValue_get_from_remote()
{
  DLOGLN("MeterValue_get_from_remote Connecting...");
  WiFiClient client;
  client.setTimeout(2); // seconds, see myStrom_get_Meter_value()
  if (!client.connect(DebugMeterValueFromOtherClientIP, 80)) { DLOGLN(F("Connection failed")); return -1; }

  DLOGLN(F("Connected!"));
  client.println(F("GET /showLastMeterValue HTTP/1.0"));
  client.print(F("Host: ")); client.println(F(DebugMeterValueFromOtherClientIP));
  client.println(F("Connection: close"));
  client.println();
  DLOGLN(F("Request sent"));

  String fullResponse = "";
  unsigned long startTime = millis();
  while (client.connected() || client.available())
  {
    if (client.available()) fullResponse += (char)client.read();
    else
    {
      if (millis() - startTime > 5000) { DLOGLN(F("Timeout while reading response")); break; }
      delay(10);
    }
  }

  int bodyIndex = fullResponse.indexOf("\r\n\r\n");
  if (bodyIndex == -1) { DLOGLN(F("No HTTP body found")); return -2; }

  String body = fullResponse.substring(bodyIndex + 4);
  body.trim();

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, body);
  if (error) { DLOG(F("deserializeJson() failed: ")); DLOGLN(error.f_str()); return -3; }

  int32_t meter_value_180_i32 = doc["meter_value_180"] | -4;
  int32_t timestamp           = doc["timestamp"] | 0;

  DLOG(F("Meter Value 180: ")); DLOGLN(meter_value_180_i32);
  DLOG(F("Timestamp: "));      DLOGLN(timestamp);

  MeterValue newVal = MeterValue_getLast();
  resetMeterValue(newVal);
  newVal.meter_value_180 = doc["meter_value_180"];
  newVal.timestamp       = doc["timestamp"];
  newVal.temperature     = doc["temperature"];
  newVal.solar           = doc["solar"];
  newVal.meter_value_280 = doc["meter_value_280"];
  MeterValue_setLast(newVal);
  client.stop();
  timestamp_telegram = timestamp;
  return meter_value_180_i32;
}

void Telegram_ResetReceiveBuffer()
{
  telegram_receive_bufferIndex = 0;
}

void handle_Telegram_receive()
{
  if (DebugFromOtherClient_object.isChecked())
  {
    if (last_remote_meter_value + 5000 < millis())
    {
      last_remote_meter_value = millis();
      MeterValue_get_from_remote();
    }
    return;
  }

  while (mySerial.available() > 0)
  {
    uint8_t incomingByte = mySerial.read();
    lastByteTime = millis();
    if (telegram_receive_bufferIndex < TELEGRAM_LENGTH)
      telegram_receive_buffer[telegram_receive_bufferIndex++] = incomingByte;
    else
    {
      Log_Add(3001);
      Telegram_ResetReceiveBuffer();
      continue;
    }
  }

  if (telegram_receive_bufferIndex > 0 && (millis() - lastByteTime > TELEGRAM_TIMEOUT_MS))
  {
    // Try SML parser first. If it finds the SML prefix+suffix, it is
    // authoritative and IEC is not attempted.
    // If SML finds no valid frame, fall through to the IEC parser.
    bool parsed = Telegram_parse_SML(telegram_receive_buffer, telegram_receive_bufferIndex);
    if (parsed)
    {
      last_detected_protocol = TelegramProtocol::SML;
    }
    else
    {
      parsed = Telegram_parse_IEC(telegram_receive_buffer, telegram_receive_bufferIndex);
      if (parsed) last_detected_protocol = TelegramProtocol::IEC;
    }
    if (!parsed)
    {
      Log_Add(3006);
    }
    if (parsed)
    {
      last_telegram_parsed = millis(); // reset watchdog
      last_silence_log     = 0;         // restart alert cycle if meter comes back online
      if (!startup_print_done) {
        startup_print_done = true;
        Serial.printf("\n--- Startup Meter Diagnostic ---\n");
        Serial.printf("Protocol : %s\n", last_detected_protocol == TelegramProtocol::SML ? "SML" : "IEC 62056-21");
        Serial.printf("Meter    : %s\n", meter_model.isEmpty() ? "(unknown)" : meter_model.c_str());
        Serial.printf("1.8.0    : %lu (0.1 Wh)\n", (unsigned long)LastMeterValue.meter_value_180);
        Serial.printf("2.8.0    : %lu (0.1 Wh)\n", (unsigned long)LastMeterValue.meter_value_280);
        Serial.printf("P Import : %lu W\n", (unsigned long)LastMeterValue.power_import);
        Serial.printf("P Export : %lu W\n", (unsigned long)LastMeterValue.power_export);
        Serial.printf("P Net    : %ld W\n", (long)LastMeterValue.net_power);
        Serial.printf("--- End Diagnostic ---\n\n");

      }
    }
    if(last_detected_protocol != prev_detected_protocol){
      if(last_detected_protocol == TelegramProtocol::SML) Log_AddWithoutTransmit(3003);
      else if(last_detected_protocol == TelegramProtocol::IEC) Log_AddWithoutTransmit(3004);

      prev_detected_protocol = last_detected_protocol;
    }
    if (parsed && temperature_object.isChecked() && !mystrom_PV_object.isChecked())
      LastMeterValue.temperature = current_temperature;

    Telegram_ResetReceiveBuffer();
  }
}

void telegramTask(void * pvParameters) {
  for(;;) {
    xSemaphoreTake(Sema_Serial, portMAX_DELAY);
    if (SerialScan_consumePending()) {
      SerialScan_run();
    } else if (!SerialScan_isRunning()) {
      handle_Telegram_receive();
    }
    xSemaphoreGive(Sema_Serial);
    vTaskDelay(pdMS_TO_TICKS(10));
    watermark_telegram = uxTaskGetStackHighWaterMark(NULL);
  }
}

// ---------------------------------------------------------------------------
// handle_telegram_watchdog
// Logs 3007 (no serial data) or 3005 (no valid telegram) if nothing has been
// received for 5 minutes (e.g. optical reader slipped off the meter). Repeats
// every 30 minutes while the meter remains silent. Like any non-routine code,
// the entry marks the log for upload with the next backend call.
// Resets automatically when a new telegram arrives (see handle_Telegram_receive).
// ---------------------------------------------------------------------------
void handle_telegram_watchdog()
{
  if (DebugFromOtherClient_object.isChecked()) return; // no serial telegram expected in remote mode

  bool alertDue = (last_silence_log == 0 || millis() - last_silence_log >= 1800000UL);

  if (millis() - lastByteTime >= 300000UL)
  {
    // No bytes at all on the serial interface — optical reader likely disconnected.
    if (alertDue)
    {
      Log_Add(3007);
      last_silence_log = millis();
    }
    return; // 3005 (parse failure) would be redundant — suppress it
  }

  if (millis() - last_telegram_parsed >= 300000UL)
  {
    // Bytes arriving but no valid telegram parsed for 5 min (baud/parity mismatch?).
    if (alertDue)
    {
      Log_Add(3005);
      last_silence_log = millis();
    }
  }
}
