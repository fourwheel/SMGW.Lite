#include "ota_pull.h"
#include "version.h"
#include "app_globals.h"
#include "log_buffer.h"
#include "debug_log.h"
#include <WiFiClientSecure.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>
#include <mbedtls/md.h>
#include <Preferences.h>

extern void Webclient_send_log_to_backend();


static const uint32_t FW_CONNECT_TIMEOUT_MS    = 15000;
static const uint32_t FW_READ_TIMEOUT_MS       = 30000;
static const int      FW_MAX_MANIFEST_BYTES    = 512;
static const int      FW_MAX_BINARY_CHUNK      = 1024;
static const uint32_t FW_ROLLBACK_COOLDOWN_MS  = 15UL * 60 * 1000;

static WiFiClientSecure* fw_open_client()
{
    WiFiClientSecure* client = new WiFiClientSecure();
    if (!client) return nullptr;
    client->setTimeout(FW_READ_TIMEOUT_MS / 1000);
    if (UseSslCert_object.isChecked())
        client->setCACert(FullCert);
    else
        client->setInsecure();
    return client;
}

static bool fw_send_get(WiFiClientSecure& client, const String& path)
{
    // HTTP/1.0 prevents chunked transfer encoding from the server,
    // which would corrupt the body reader.
    client.print(String("GET ") + path + " HTTP/1.0\r\n"
                 "Host: " + backend_host + "\r\n"
                 "Connection: close\r\n\r\n");

    unsigned long deadline = millis() + FW_READ_TIMEOUT_MS;
    while (millis() < deadline) {
        if (client.available()) {
            String line = client.readStringUntil('\n');
            if (line.indexOf(" 200 ") >= 0) return true;
            if (line.startsWith("HTTP/"))   return false; // non-200
        } else if (!client.connected()) {
            break;
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    return false;
}

static void fw_skip_headers(WiFiClientSecure& client)
{
    // Do not use client.connected() as the outer loop condition —
    // with HTTP/1.0 the server closes the connection immediately after
    // the response, so connected() may be false while data is still buffered.
    unsigned long deadline = millis() + FW_READ_TIMEOUT_MS;
    while (millis() < deadline) {
        if (client.available()) {
            String line = client.readStringUntil('\n');
            if (line == "\r" || line.isEmpty()) break;
        } else if (!client.connected()) {
            break;
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

static bool fw_fetch_manifest(String& version_out, String& filename_out,
                               String& sha256_out, size_t& size_out)
{
    String base = backend_path.substring(0, backend_path.lastIndexOf('/') + 1);
    String path = base + "fwupdate/" + String(backend_ID) + "/manifest.json";

    WiFiClientSecure* client = fw_open_client();
    if (!client) return false;

    if (!client->connect(backend_host.c_str(), 443, FW_CONNECT_TIMEOUT_MS)) {
        Log_AddEntry(6001);
        client->stop(); delete client;
        return false;
    }
    if (!fw_send_get(*client, path)) {
        Log_AddEntry(6012);
        client->stop(); delete client;
        return false;
    }
    fw_skip_headers(*client);

    char body[FW_MAX_MANIFEST_BYTES + 1];
    int  bodyLen = 0;
    unsigned long deadline = millis() + FW_READ_TIMEOUT_MS;
    while (bodyLen < FW_MAX_MANIFEST_BYTES && millis() < deadline) {
        if (client->available()) {
            body[bodyLen++] = (char)client->read();
        } else if (!client->connected()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (!client->available()) break;
        }
    }
    body[bodyLen] = '\0';
    client->stop(); delete client;

    bool ok = false;
    JsonDocument doc;
    if (deserializeJson(doc, body) == DeserializationError::Ok) {
        const char* v  = doc["version"]  | "";
        const char* f  = doc["filename"] | "";
        const char* s  = doc["sha256"]   | "";
        size_out       = doc["size"]      | (size_t)UPDATE_SIZE_UNKNOWN;

        if (strlen(v) > 0 && strlen(f) > 0 && strlen(s) == 64) {
            version_out  = String(v);
            filename_out = String(f);
            sha256_out   = String(s);
            sha256_out.toLowerCase();
            ok = true;
        }
    }
    if (!ok) Log_AddEntry(6013);
    return ok;
}

static bool fw_download_and_flash(const String& filename,
                                   const String& expected_sha256,
                                   size_t        expected_size)
{
    String base = backend_path.substring(0, backend_path.lastIndexOf('/') + 1);
    String path = base + "fwupdate/" + String(backend_ID) + "/" + filename;

    WiFiClientSecure* client = fw_open_client();
    if (!client) return false;

    bool ok = false;
    if (!client->connect(backend_host.c_str(), 443, FW_CONNECT_TIMEOUT_MS)) {
        client->stop();
        delete client;
        return false;
    }
    if (!fw_send_get(*client, path)) {
        client->stop();
        delete client;
        return false;
    }
    fw_skip_headers(*client);

    if (!Update.begin(expected_size, U_FLASH)) {
        Log_AddEntry(6004);
        client->stop();
        delete client;
        return false;
    }

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
    mbedtls_md_starts(&ctx);

    uint8_t  chunk[FW_MAX_BINARY_CHUNK];
    unsigned long deadline = millis() + FW_READ_TIMEOUT_MS;

    while (millis() < deadline) {
        int available = client->available();
        if (available > 0) {
            deadline = millis() + FW_READ_TIMEOUT_MS; // reset on data
            int toRead = min(available, FW_MAX_BINARY_CHUNK);
            int bytes  = client->read(chunk, toRead);
            if (bytes > 0) {
                mbedtls_md_update(&ctx, chunk, bytes);
                if (Update.write(chunk, bytes) != (size_t)bytes) {
                    Log_AddEntry(6005);
                    Update.abort();
                    goto cleanup;
                }
            }
        } else if (!client->connected()) {
            break;
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    {
        uint8_t hash[32];
        mbedtls_md_finish(&ctx, hash);

        char computed[65];
        for (int i = 0; i < 32; i++)
            snprintf(computed + i * 2, 3, "%02x", hash[i]);
        computed[64] = '\0';

        if (expected_sha256 != String(computed)) {
            DLOGLN("OTA: SHA256 mismatch");
            Log_AddEntry(6006);
            Update.abort();
            goto cleanup;
        }

        if (!Update.end(true)) {
            Log_AddEntry(6007);
            goto cleanup;
        }

        Log_AddEntry(6008);
        ok = true;
    }

cleanup:
    mbedtls_md_free(&ctx);
    client->stop();
    delete client;
    return ok;
}


void OtaPull_init()
{
    Preferences prefs;
    prefs.begin("ota", true);
    bool pending = prefs.getBool("pending", false);
    prefs.end();
    if (!pending) return;

    Log_AddEntry(6009);
    DLOGLN("OTA: post-update boot — validating firmware");

    WiFiClientSecure* client = fw_open_client();
    bool reached = false;

    if (client && client->connect(backend_host.c_str(), 443, FW_CONNECT_TIMEOUT_MS)) {
        String path = backend_path + "?ID=" + String(backend_ID) + "&backend_test=true";
        client->print(String("GET ") + path + " HTTP/1.0\r\n"
                      "Host: " + backend_host + "\r\n"
                      "X-Auth-Token: " + String(backend_token) + "\r\n"
                      "Connection: close\r\n\r\n");
        unsigned long deadline = millis() + FW_READ_TIMEOUT_MS;
        while (millis() < deadline) {
            if (client->available()) {
                String line = client->readStringUntil('\n');
                if (line.indexOf(" 200 ") >= 0) { reached = true; break; }
                if (line.startsWith("HTTP/"))    { break; }
            } else if (!client->connected()) {
                break;
            }
        }
    }
    if (client) { client->stop(); delete client; }

    prefs.begin("ota", false);
    prefs.remove("pending");
    prefs.end();

    if (reached) {
        Log_AddEntry(6010);
        DLOGLN("OTA: firmware validated");
    } else {
        Log_AddEntry(6011);
        DLOGLN("OTA: validation failed — rolling back");
        Preferences prefs;
        prefs.begin("ota", false);
        prefs.putBool("rollback", true);
        prefs.end();
        Webclient_send_log_to_backend();
        const esp_partition_t* prev = esp_ota_get_next_update_partition(NULL);
        if (prev) esp_ota_set_boot_partition(prev);
        delay(200);
        esp_restart();
    }
}

void OtaPull_check()
{
    if (!wifi_connected)       { Log_AddEntry(6016); return; }
    if (ota_active)            { Log_AddEntry(6017); return; }
    if (strlen(backend_ID)==0) { Log_AddEntry(6018); return; }
    if (backend_host.isEmpty()) { Log_AddEntry(6019); return; }

    static bool     rollback_checked  = false;
    static uint32_t ota_blocked_until = 0;
    if (!rollback_checked) {
        rollback_checked = true;
        Preferences prefs;
        prefs.begin("ota", false);
        bool was_rollback = prefs.getBool("rollback", false);
        prefs.remove("rollback");
        prefs.end();
        if (was_rollback) {
            ota_blocked_until = millis() + FW_ROLLBACK_COOLDOWN_MS;
            Log_AddEntry(6020);
        }
    }
    if (millis() < ota_blocked_until) return;

    Log_AddEntry(6000);
    ota_active = true;

    if (!xSemaphoreTake(Sema_Backend, pdMS_TO_TICKS(30000))) {
        ota_active = false;
        return;
    }

    String version, filename, sha256;
    size_t size;

    if (!fw_fetch_manifest(version, filename, sha256, size)) {
        xSemaphoreGive(Sema_Backend);
        ota_active = false;
        return;
    }

    DLOGF("OTA: server=%s current=%s\n", version.c_str(), FIRMWARE_VERSION);

    if (version == String(FIRMWARE_VERSION)) {
        Log_AddEntry(6002);
        xSemaphoreGive(Sema_Backend);
        ota_active = false;
        return;
    }

    Log_AddEntry(6003);

    bool flashed = fw_download_and_flash(filename, sha256, size);
    xSemaphoreGive(Sema_Backend);

    if (!flashed) {
        ota_active = false;
        return;
    }

    Webclient_send_log_to_backend();

    Preferences prefs;
    prefs.begin("ota", false);
    prefs.putBool("pending", true);
    prefs.end();

    delay(500);
    esp_restart();
}
