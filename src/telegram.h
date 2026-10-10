#pragma once
#include <Arduino.h>

enum class TelegramProtocol { UNKNOWN, SML, IEC };
extern TelegramProtocol last_detected_protocol;

extern bool          startup_print_done;   // first telegram diagnostics done (boot snapshot waits for it)
extern unsigned long last_telegram_parsed; // millis() of the last parsed telegram, see handle_telegram_watchdog()

String Telegram_protocol_to_string(TelegramProtocol p);
void   telegramTask(void *pvParameters);
void   handle_telegram_watchdog();
void   handle_Telegram_receive();
void   Telegram_ResetReceiveBuffer();
bool   Telegram_parse_SML(uint8_t* buffer, size_t length);
bool   Telegram_parse_IEC(uint8_t* buffer, size_t length);
int32_t MeterValue_get_from_remote();
