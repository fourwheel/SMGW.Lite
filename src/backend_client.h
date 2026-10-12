#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>

void Webclient_send_log_to_backend();
void Webclient_Send_Log_to_backend_Task(void *pvParameters);
void Webclient_Send_Meter_Values_to_backend_Task(void *pvParameters);
int  Webclient_read_response(WiFiClientSecure &client, String &body, unsigned long timeout_ms);
