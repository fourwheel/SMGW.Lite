#pragma once
#include <Arduino.h>

enum class TelegramProtocol { UNKNOWN, SML, IEC };
extern TelegramProtocol last_detected_protocol;

String Telegram_protocol_to_string(TelegramProtocol p);
