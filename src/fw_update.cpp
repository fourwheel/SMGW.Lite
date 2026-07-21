#include "fw_update.h"
#include "app_globals.h"
#include "log_buffer.h"
#include "debug_log.h"
#include <WiFiClientSecure.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>
#include <mbedtls/md.h>

#define LOG_OTA_CHECK_START    6000
#define LOG_OTA_MANIFEST_FAIL  6001
#define LOG_OTA_UP_TO_DATE     6002
#define LOG_OTA_UPDATE_START   6003
#define LOG_OTA_BEGIN_FAIL     6004
#define LOG_OTA_WRITE_FAIL     6005
#define LOG_OTA_SHA256_FAIL    6006
#define LOG_OTA_END_FAIL       6007
#define LOG_OTA_FLASH_OK       6008
#define LOG_OTA_VALIDATING     6009
#define LOG_OTA_VALIDATE_OK    6010
#define LOG_OTA_VALIDATE_FAIL  6011

static const uint32_t FW_CONNECT_TIMEOUT_MS = 15000;
static const uint32_t FW_READ_TIMEOUT_MS    = 30000;
static const int      FW_MAX_MANIFEST_BYTES = 512;
static const int      FW_MAX_BINARY_CHUNK   = 1024;

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
    client.print(String("GET ") + path + " HTTP/1.1\r\n"
                 "Host: " + backend_host + "\r\n"
                 "Connection: close\r\n\r\n");

    unsigned long deadline = millis() + FW_READ_TIMEOUT_MS;
    while (client.connected() && millis() < deadline) {
        if (client.available()) {
            String line = client.readStringUntil('\n');
            if (line.startsWith("HTTP/1.1 200")) return true;
            if (line.startsWith("HTTP/1.1"))     return false; // non-200
        }
    }
    return false;
}

static void fw_skip_headers(WiFiClientSecure& client)
{
    unsigned long deadline = millis() + FW_READ_TIMEOUT_MS;
    while (client.connected() && millis() < deadline) {
        if (client.available()) {
            String line = client.readStringUntil('\n');
            if (line == "\r" || line.isEmpty()) break;
        }
    }
}

static bool fw_fetch_manifest(String& version_out, String& filename_out,
                               String& sha256_out, size_t& size_out)
{
    String path = "/fwupdate/" + String(backend_ID) + "/manifest.json";

    WiFiClientSecure* client = fw_open_client();
    if (!client) return false;

    bool ok = false;
    if (client->connect(backend_host.c_str(), 443, FW_CONNECT_TIMEOUT_MS)) {
        if (fw_send_get(*client, path)) {
            fw_skip_headers(*client);

            char body[FW_MAX_MANIFEST_BYTES + 1];
            int  bodyLen = 0;
            unsigned long deadline = millis() + FW_READ_TIMEOUT_MS;
            while (bodyLen < FW_MAX_MANIFEST_BYTES && millis() < deadline) {
                if (client->available()) {
                    body[bodyLen++] = (char)client->read();
                } else if (!client->connected()) {
                    break;
                }
            }
            body[bodyLen] = '\0';

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
        }
    }
    client->stop();
    delete client;
    return ok;
}

static bool fw_download_and_flash(const String& filename,
                                   const String& expected_sha256,
                                   size_t        expected_size)
{
    String path = "/fwupdate/" + String(backend_ID) + "/" + filename;

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
        Log_AddEntry(LOG_OTA_BEGIN_FAIL);
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
                    Log_AddEntry(LOG_OTA_WRITE_FAIL);
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
            Log_AddEntry(LOG_OTA_SHA256_FAIL);
            Update.abort();
            goto cleanup;
        }

        if (!Update.end(true)) {
            Log_AddEntry(LOG_OTA_END_FAIL);
            goto cleanup;
        }

        Log_AddEntry(LOG_OTA_FLASH_OK);
        ok = true;
    }

cleanup:
    mbedtls_md_free(&ctx);
    client->stop();
    delete client;
    return ok;
}

void FwUpdate_init()
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return;

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return;

    Log_AddEntry(LOG_OTA_VALIDATING);
    DLOGLN("OTA: post-update boot — validating firmware");

    WiFiClientSecure* client = fw_open_client();
    bool reached = false;

    if (client && client->connect(backend_host.c_str(), 443, FW_CONNECT_TIMEOUT_MS)) {
        String path = backend_path + "?ID=" + String(backend_ID) + "&backend_test=true";
        reached = fw_send_get(*client, path);
    }
    if (client) { client->stop(); delete client; }

    if (reached) {
        esp_ota_mark_app_valid_cancel_rollback();
        Log_AddEntry(LOG_OTA_VALIDATE_OK);
        DLOGLN("OTA: firmware validated");
    } else {
        Log_AddEntry(LOG_OTA_VALIDATE_FAIL);
        DLOGLN("OTA: validation failed — rolling back");
        delay(200);
        esp_restart();
        // Bootloader rolls back to previous OTA slot on this restart.
    }
}

void FwUpdate_check()
{
    if (!wifi_connected || ota_active || strlen(backend_ID) == 0
        || backend_host.isEmpty()) return;

    Log_AddEntry(LOG_OTA_CHECK_START);
    ota_active = true;

    String version, filename, sha256;
    size_t size;

    if (!fw_fetch_manifest(version, filename, sha256, size)) {
        Log_AddEntry(LOG_OTA_MANIFEST_FAIL);
        ota_active = false;
        return;
    }

    DLOGF("OTA: server=%s current=%s\n", version.c_str(), FIRMWARE_VERSION);

    if (version == String(FIRMWARE_VERSION)) {
        Log_AddEntry(LOG_OTA_UP_TO_DATE);
        ota_active = false;
        return;
    }

    Log_AddEntry(LOG_OTA_UPDATE_START);

    if (!xSemaphoreTake(Sema_Backend, pdMS_TO_TICKS(30000))) {
        ota_active = false;
        return;
    }

    bool flashed = fw_download_and_flash(filename, sha256, size);
    xSemaphoreGive(Sema_Backend);

    if (!flashed) {
        ota_active = false;
        return;
    }

    delay(500);
    esp_restart();
}
