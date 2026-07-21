#include "serial_scan.h"
#include "log_buffer.h"
#include "app_globals.h"
#include <Preferences.h>
#include "soc/uart_reg.h"

// Forward declaration — Telegram_ResetReceiveBuffer stays in main.cpp
void Telegram_ResetReceiveBuffer();
// Forward declarations for private helpers used before their definitions
static int SerialScan_activeIndexPrivate();
String SerialScan_activeLabel();

// ---------------------------------------------------------------------------
// Scan table — all candidate baud/parity combinations
// ---------------------------------------------------------------------------
const SerialScanEntry SERIAL_SCAN_TABLE[] = {
    {  9600, SERIAL_8N1,  "9600-8N1"  },
    {  9600, SERIAL_8E1,  "9600-8E1"  },
    {  9600, SERIAL_7E1,  "9600-7E1"  },
    {  2400, SERIAL_7E1,  "2400-7E1"  },
    {  2400, SERIAL_8N1,  "2400-8N1"  },
    {  4800, SERIAL_8N1,  "4800-8N1"  },
    {  4800, SERIAL_7E1,  "4800-7E1"  },
    {  1200, SERIAL_7E1,  "1200-7E1"  },
    {  1200, SERIAL_8N1,  "1200-8N1"  },
    {   300, SERIAL_7E1,   "300-7E1"  },
    { 19200, SERIAL_8N1, "19200-8N1"  },
    { 38400, SERIAL_8N1, "38400-8N1"  },
};
const int SERIAL_SCAN_TABLE_SIZE = (int)(sizeof(SERIAL_SCAN_TABLE) / sizeof(SERIAL_SCAN_TABLE[0]));

// ---------------------------------------------------------------------------
// Private state
// ---------------------------------------------------------------------------
enum class ScanState : uint8_t { IDLE, RUNNING, DONE };

static uint32_t          active_baud_rate        = 9600;
static uint32_t          active_uart_config       = SERIAL_8N1;
static int               serial_config_saved_idx  = -1;
static volatile ScanState scan_state              = ScanState::IDLE;
static volatile int       scan_current            = -1;
static volatile int       scan_found              = -1;
static volatile uint32_t  scan_found_mask         = 0;
static volatile bool      serial_scan_requested   = false;

// ---------------------------------------------------------------------------
// Public accessors
// ---------------------------------------------------------------------------
uint32_t SerialScan_getActiveBaud()   { return active_baud_rate; }
uint32_t SerialScan_getActiveConfig() { return active_uart_config; }

void SerialScan_requestScan()
{
    scan_state            = ScanState::IDLE;
    scan_found            = -1;
    scan_found_mask       = 0;
    scan_current          = -1;
    serial_scan_requested = true;
}

bool SerialScan_consumePending()
{
    if (!serial_scan_requested) return false;
    serial_scan_requested = false;
    return true;
}

bool SerialScan_isRunning() { return scan_state == ScanState::RUNNING; }

String SerialScan_getStatusJson()
{
    String json = "{\"state\":\"";
    switch (scan_state) {
        case ScanState::RUNNING: json += "running"; break;
        case ScanState::DONE:    json += "done";    break;
        default:                 json += "idle";    break;
    }
    json += "\",\"currentIndex\":" + String((int)scan_current);
    json += ",\"foundIndex\":"     + String((int)scan_found);
    json += ",\"foundMask\":"      + String((unsigned int)scan_found_mask);
    json += ",\"total\":"          + String(SERIAL_SCAN_TABLE_SIZE);
    json += ",\"activeIndex\":"    + String(SerialScan_activeIndexPrivate());
    json += ",\"activeLabel\":\""  + SerialScan_activeLabel() + "\"}";
    return json;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------
static int SerialScan_activeIndexPrivate()
{
    for (int i = 0; i < SERIAL_SCAN_TABLE_SIZE; i++)
        if (SERIAL_SCAN_TABLE[i].baudRate    == active_baud_rate &&
            SERIAL_SCAN_TABLE[i].uartConfig  == active_uart_config)
            return i;
    return -1;
}

static String SerialScan_label(uint32_t baud, uint32_t cfg)
{
    for (int i = 0; i < SERIAL_SCAN_TABLE_SIZE; i++)
        if (SERIAL_SCAN_TABLE[i].baudRate == baud && SERIAL_SCAN_TABLE[i].uartConfig == cfg)
            return String(SERIAL_SCAN_TABLE[i].label);
    return String(baud) + "-?";
}

static bool SerialScan_looks_valid(const uint8_t* buf, size_t len)
{
    if (len < (size_t)SERIAL_SCAN_MIN_BYTES) return false;
    bool hasStart = false;
    size_t px = 0;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == 0x1b && buf[i+1] == 0x1b && buf[i+2] == 0x1b && buf[i+3] == 0x1b) {
            if (!hasStart) { hasStart = true; px = i; i += 3; continue; }
            if (i + 4 < len && buf[i+4] == 0x1a && (i - px) >= 40) return true;
        }
    }
    if (buf[0] == '/') {
        int printable = 0;
        for (size_t i = 1; i < len && i < 32; i++)
            if (buf[i] >= 0x20 && buf[i] <= 0x7E) printable++;
        if (printable >= 4) return true;
    }
    return false;
}

static void SerialConfig_save(int idx)
{
    serial_config_saved_idx = idx;
    Preferences prefs;
    if (prefs.begin("smgw", false)) {
        prefs.putInt("serialCfg", idx);
        prefs.end();
    }
}

// ---------------------------------------------------------------------------
// Public functions
// ---------------------------------------------------------------------------
String SerialScan_buildTableRows()
{
    String rows;
    rows.reserve(SERIAL_SCAN_TABLE_SIZE * 130);
    int activeIdx = SerialScan_activeIndexPrivate();
    for (int i = 0; i < SERIAL_SCAN_TABLE_SIZE; i++) {
        bool isActive = (i == activeIdx);
        rows += "<tr id=\"r";
        rows += i;
        rows += (isActive ? "\" class=\"active-cfg\">" : "\">");
        rows += "<td>";
        rows += SERIAL_SCAN_TABLE[i].label;
        rows += "</td><td id=\"s";
        rows += i;
        rows += "\" class=\"pending\">&#8203;</td><td><button class=\"set-btn";
        rows += (isActive ? " act\" id=\"b" : "\" id=\"b");
        rows += i;
        rows += "\" onclick=\"setConfig(";
        rows += i;
        rows += (isActive ? ")\">Active</button></td></tr>\n" : ")\">Activate</button></td></tr>\n");
    }
    return rows;
}

String SerialScan_activeLabel()
{
    return SerialScan_label(active_baud_rate, active_uart_config);
}

bool SerialConfig_setByIndex(int idx)
{
    if (idx < 0 || idx >= SERIAL_SCAN_TABLE_SIZE) return false;
    const SerialScanEntry& e = SERIAL_SCAN_TABLE[idx];
    active_baud_rate   = e.baudRate;
    active_uart_config = e.uartConfig;
    mySerial.end();
    mySerial.begin(e.baudRate, e.uartConfig, RX_PIN, TX_PIN);
    SerialConfig_save(idx);
    Log_AddEntry(3012);
    return true;
}

void SerialConfig_load()
{
    Preferences prefs;
    if (!prefs.begin("smgw", true)) return;
    int idx = prefs.getInt("serialCfg", -1);
    prefs.end();
    if (idx < 0 || idx >= SERIAL_SCAN_TABLE_SIZE) return;
    serial_config_saved_idx = idx;
    const SerialScanEntry& e = SERIAL_SCAN_TABLE[idx];
    active_baud_rate   = e.baudRate;
    active_uart_config = e.uartConfig;
    mySerial.end();
    mySerial.begin(e.baudRate, e.uartConfig, RX_PIN, TX_PIN);
}

void SerialScan_run()
{
    static uint8_t scan_buf[512];

    const uint32_t saved_baud   = active_baud_rate;
    const uint32_t saved_config = active_uart_config;
    const int      saved_idx    = serial_config_saved_idx;

    scan_state      = ScanState::RUNNING;
    scan_found      = -1;
    scan_found_mask = 0;

    for (int i = 0; i < SERIAL_SCAN_TABLE_SIZE; i++) {
        scan_current = i;
        const SerialScanEntry& e = SERIAL_SCAN_TABLE[i];

        mySerial.end();
        vTaskDelay(pdMS_TO_TICKS(60));
        mySerial.begin(e.baudRate, e.uartConfig, RX_PIN, TX_PIN);

        vTaskDelay(pdMS_TO_TICKS(120));
        while (mySerial.available()) mySerial.read();
        // Disable the parity-error interrupt so the IDF UART ISR cannot clear UART_INT_RAW
        // bit 2 while we are collecting.
        CLEAR_PERI_REG_MASK(UART_INT_ENA_REG(1), 1UL << 2);
        WRITE_PERI_REG(UART_INT_CLR_REG(1), 1UL << 2);

        size_t idx        = 0;
        unsigned long start    = millis();
        unsigned long lastByte = 0;

        while (millis() - start < (unsigned long)SERIAL_SCAN_TIMEOUT_MS) {
            while (mySerial.available()) {
                uint8_t b = mySerial.read();
                lastByte = millis();
                if (idx < sizeof(scan_buf)) scan_buf[idx++] = b;
            }
            if (idx >= (size_t)SERIAL_SCAN_MIN_BYTES && lastByte > 0 && millis() - lastByte > 200)
                break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        bool hadParityError = (READ_PERI_REG(UART_INT_RAW_REG(1)) & (1UL << 2)) != 0;
        SET_PERI_REG_MASK(UART_INT_ENA_REG(1), 1UL << 2);

        if (SerialScan_looks_valid(scan_buf, idx) && !hadParityError) {
            scan_found_mask |= (1UL << i);
            if (scan_found == -1) scan_found = i;
        }
    }

    if (serial_config_saved_idx != saved_idx) {
        mySerial.end();
        mySerial.begin(active_baud_rate, active_uart_config, RX_PIN, TX_PIN);
    } else {
        active_baud_rate        = saved_baud;
        active_uart_config      = saved_config;
        serial_config_saved_idx = saved_idx;
        mySerial.end();
        mySerial.begin(saved_baud, saved_config, RX_PIN, TX_PIN);
    }
    Log_AddEntry(scan_found >= 0 ? 3010 : 3011);

    scan_state   = ScanState::DONE;
    scan_current = -1;
    Telegram_ResetReceiveBuffer();
}
