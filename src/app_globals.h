#pragma once
// Shared extern declarations for globals defined in main.cpp.
// Include this header in any .cpp file that needs access to these globals.

#include <Arduino.h>
#include <IotWebConf.h>
#include <IotWebConfUsing.h>
#include <WiFi.h>
#include <HardwareSerial.h>

// Hardware
extern WebServer      server;
extern DNSServer      dnsServer;
extern IotWebConf     iotWebConf;
extern HardwareSerial mySerial;

// Hardware defines (from platformio.ini build flags: RX_PIN, TX_PIN)
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif
#define STATUS_PIN LED_BUILTIN
// When CONFIG_PIN is pulled to ground on startup, IotWebConf builds its AP with
// the initial password (e.g. in case of a lost password).
#define CONFIG_PIN 5

// Telegram receive buffer
#define TELEGRAM_LENGTH 1024
extern uint8_t        telegram_receive_buffer[];
extern unsigned long  lastByteTime;
extern unsigned long  last_call_backend;
extern String         meter_model;

// WiFi / AP state
extern bool          wifi_connected;
extern bool          g_wifiSetupPending;
extern bool          g_wifiSetupFailed;
extern unsigned long g_wifiSetupStartedAt;
extern unsigned long g_apStopAt;
extern bool          redirect_to_sysinfo;

// IotWebConf parameter groups
extern IotWebConfParameterGroup groupTelegram;
extern IotWebConfParameterGroup groupBackend;
extern IotWebConfParameterGroup groupTaf;
extern IotWebConfParameterGroup groupAdditionalMeter;
extern IotWebConfParameterGroup groupSys;
extern IotWebConfParameterGroup groupDebug;

// IotWebConf parameters
extern IotWebConfCheckboxParameter activate_IEC_Parser_object;
extern IotWebConfTextParameter     backend_endpoint_object;
extern IotWebConfCheckboxParameter led_blink_object;
extern IotWebConfTextParameter     backend_ID_object;
extern IotWebConfTextParameter     backend_token_object;
extern IotWebConfCheckboxParameter taf7_b_object;
extern IotWebConfNumberParameter   taf7_param_object;
extern IotWebConfCheckboxParameter taf14_b_object;
extern IotWebConfNumberParameter   taf14_param_object;
extern IotWebConfNumberParameter   backend_call_minute_object;
extern IotWebConfCheckboxParameter mystrom_PV_object;
extern IotWebConfTextParameter     mystrom_PV_IP_object;
extern IotWebConfCheckboxParameter temperature_object;
extern IotWebConfCheckboxParameter UseSslCert_object;
extern IotWebConfCheckboxParameter DebugSetOffline_object;
extern IotWebConfCheckboxParameter DebugFromOtherClient_object;
extern IotWebConfTextParameter     DebugMeterValueFromOtherClientIP_object;
extern IotWebConfNumberParameter   Meter_Value_Buffer_Size_object;
extern IotWebConfCheckboxParameter config_temperature_object;
extern IotWebConfCheckboxParameter config_solar_object;
extern IotWebConfCheckboxParameter config_280_object;

// Config char buffers (raw values read by IotWebConf)
extern char backend_endpoint[];
extern char backend_token[];
extern char backend_ID[];
extern char taf7_param[];
extern char taf14_param[];
extern char backend_call_minute[];
extern char Meter_Value_Buffer_Size_Char[];
extern char mystrom_PV_IP[];
extern char DebugMeterValueFromOtherClientIP[];
extern char config_temperature_char[];
extern char config_solar_char[];
extern char config_280_char[];

// Backend runtime state
extern String         backend_host;
extern String         backend_path;
extern volatile bool  b_send_log_to_backend;
extern volatile bool  g_log_upload_full;
extern bool           call_backend_successfull;
extern unsigned long  last_backend_success;
extern int            IPlastOctet;

// Misc state shown on /sysinfo
extern int          staticDelay;
extern int          current_temperature; // last DS18B20 reading, see handle_temperature()
extern int          watermark_meter_buffer;
extern int          watermark_log_buffer;
extern int          watermark_telegram;
extern const String BUILD_TIMESTAMP;
extern const String BUILD_BRANCH;

// TAF cached params
extern int cached_taf7_param;
extern int cached_taf14_param;
extern int cached_backend_call_minute;

// FreeRTOS
extern SemaphoreHandle_t Sema_Backend;
extern SemaphoreHandle_t Sema_Serial;
extern SemaphoreHandle_t Sema_MeterBuffer; // held by the meter upload from computing the ranges until the buffer is cleared

// Self-recovery, see supervisorTask()
extern volatile unsigned long g_loop_heartbeat;
extern volatile unsigned long g_meter_task_started;
extern volatile unsigned long g_log_task_started;
#ifdef TEST_SELF_RECOVERY
extern volatile bool g_test_hang_upload;
#endif

// Trigger flags
extern bool MeterValue_trigger_override;

// OTA state
extern char          FullCert[];
extern volatile bool ota_active;
extern volatile bool g_ota_check_requested;
extern volatile bool g_ota_manual_install_requested;

// Forward declarations for functions called across modules
void Led_update_Blink();
void MeterValue_init_Buffer();
int  MeterValue_Num();
void Webclient_splitHostAndPath(const String& url, String& host, String& path);
void Webclient_send_meter_values_to_backend();
void Webclient_Send_Meter_Values_to_backend_wrapper();
void Webclient_Send_Log_to_backend_wrapper();
void Webserver_LocationHrefsysinfo(int delay = 0);
void Log_Add(int code);
bool Log_AddWithoutTransmit(int code);
void OtaPull_boot();
void OtaPull_validate();
void OtaPull_check(bool manual);
