#include "ota_pull.h"
#include "version.h"
#include "app_globals.h"
#include "log_buffer.h"
#include "debug_log.h"
#include <WiFiClientSecure.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <mbedtls/md.h>
#include <Preferences.h>

extern void Webclient_send_log_to_backend();


static const uint32_t FW_CONNECT_TIMEOUT_MS    = 15000;
static const uint32_t FW_READ_TIMEOUT_MS       = 30000;
static const int      FW_MAX_RESPONSE_BYTES    = 512;
static const int      FW_MAX_BINARY_CHUNK      = 1024;
static const uint32_t FW_ROLLBACK_COOLDOWN_MS  = 15UL * 60 * 1000;
static const uint32_t FW_RETRY_BACKOFF_MS      = 60UL * 60 * 1000;
static const uint32_t FW_HANDSHAKE_TIMEOUT_S   = 30;               // TLS default is 120 s
static const uint32_t FW_DOWNLOAD_MAX_MS       = 4UL * 60 * 1000;  // ~1.2 MB need < 30 s normally
static const uint32_t FW_VALIDATE_RETRY_MS     = 60UL * 1000;      // between post-update validation attempts
static const uint32_t FW_VALIDATE_DEADLINE_MS  = 15UL * 60 * 1000; // since boot; then roll back

// Keep a freshly pulled image in PENDING_VERIFY instead of letting the Arduino
// core confirm it before setup() (initArduino()). The bootloader then boots
// the previous image if the device restarts before OtaPull_validate() has
// confirmed it, so an update that crashes or hangs before or during the
// validation rolls back by itself. Images that are not awaiting validation are
// confirmed in OtaPull_boot().
extern "C" bool verifyRollbackLater() { return true; }

// Post-update validation in progress (set by OtaPull_boot()).
static bool     validating        = false;
static bool     validate_tried    = false;
static uint32_t validate_last_try = 0;

// Update offered by the backend: the fw_update object of the last confirmed
// response (meter values or backend_test). Empty version = nothing assigned.
// Written and read only while Sema_Backend is held.
static String offer_version;
static String offer_sha256;
static size_t offer_size = 0;

// Last version whose download or flash failed — not retried automatically
// within FW_RETRY_BACKOFF_MS, so a broken release is not fetched on every call.
static String   failed_version;
static uint32_t failed_at = 0;

// Version that failed post-update validation and was rolled back (NVS
// "ota"/"bad_ver"). It is never installed automatically again, only via the
// Remote FW Update page, otherwise the device would loop flash -> rollback.
// A freshly flashed version is written here before the restart and removed
// once it is validated, so it is also blocked if the bootloader rolls it back:
// the previous firmware, which wrote it, then finds it already marked. This
// replaces an older bad version; that one may be installed once more.
static String bad_version;
static bool   bad_version_loaded = false;

static const String& fw_bad_version()
{
    if (!bad_version_loaded) {
        Preferences prefs;
        prefs.begin("ota", true);
        bad_version = prefs.getString("bad_ver", "");
        prefs.end();
        bad_version_loaded = true;
    }
    return bad_version;
}

// Version strings end up in a URL — accept only what release names contain.
static bool fw_version_valid(const char* v)
{
    size_t len = strlen(v);
    if (len == 0 || len > 32) return false;
    for (size_t i = 0; i < len; i++)
        if (!isalnum((unsigned char)v[i]) && v[i] != '.' && v[i] != '-' && v[i] != '_') return false;
    return true;
}

static WiFiClientSecure* fw_open_client()
{
    WiFiClientSecure* client = new WiFiClientSecure();
    if (!client) return nullptr;
    client->setTimeout(FW_READ_TIMEOUT_MS / 1000);
    client->setHandshakeTimeout(FW_HANDSHAKE_TIMEOUT_S);
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
                 "X-Auth-Token: " + String(backend_token) + "\r\n"
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

// Calls backend_test with fw/hw and checks the confirmation in the body
// ({"ok":true,"id":<backend_ID>}): a plain HTTP 200 from a misconfigured
// server does not count as a working backend. With take_offer the fw_update
// object of the response becomes the current offer (caller holds Sema_Backend).
static bool fw_backend_test(bool take_offer)
{
    WiFiClientSecure* client = fw_open_client();
    if (!client) return false;

    bool ok = false;
    String path = backend_path + "?ID=" + String(backend_ID) + "&backend_test=true"
                + "&fw=" FIRMWARE_VERSION "&hw=" FIRMWARE_TARGET;
    if (client->connect(backend_host.c_str(), 443, FW_CONNECT_TIMEOUT_MS) &&
        fw_send_get(*client, path))
    {
        fw_skip_headers(*client);

        char body[FW_MAX_RESPONSE_BYTES + 1];
        int  bodyLen = 0;
        unsigned long deadline = millis() + FW_READ_TIMEOUT_MS;
        while (bodyLen < FW_MAX_RESPONSE_BYTES && millis() < deadline) {
            if (client->available()) {
                body[bodyLen++] = (char)client->read();
            } else if (!client->connected()) {
                vTaskDelay(pdMS_TO_TICKS(100));
                if (!client->available()) break;
            }
        }
        body[bodyLen] = '\0';

        JsonDocument doc;
        if (deserializeJson(doc, body) == DeserializationError::Ok &&
            (doc["ok"] | false) && strcmp(doc["id"] | "", backend_ID) == 0)
        {
            ok = true;
            if (take_offer) OtaPull_setOffer(doc["fw_update"]);
        }
    }
    client->stop();
    delete client;
    return ok;
}

static bool fw_download_and_flash(const String& version,
                                   const String& expected_sha256,
                                   size_t        expected_size)
{
    String base = backend_path.substring(0, backend_path.lastIndexOf('/') + 1);
    String path = base + "fw_download.php?ID=" + String(backend_ID)
                + "&version=" + version + "&hw=" FIRMWARE_TARGET;

    WiFiClientSecure* client = fw_open_client();
    if (!client) return false;

    bool ok = false;
    if (!client->connect(backend_host.c_str(), 443, FW_CONNECT_TIMEOUT_MS)) {
        Log_Add(6023);
        client->stop();
        delete client;
        return false;
    }
    if (!fw_send_get(*client, path)) {
        Log_Add(6024);
        client->stop();
        delete client;
        return false;
    }
    fw_skip_headers(*client);

    if (!Update.begin(expected_size, U_FLASH)) {
        Log_Add(6004);
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
    // The deadline only catches a stalled download; the overall cap also ends
    // one that trickles in, since this runs in loop() (see supervisorTask()).
    const unsigned long download_started = millis();

    while (millis() < deadline) {
        if (millis() - download_started > FW_DOWNLOAD_MAX_MS) {
            Log_Add(6026);
            Update.abort();
            goto cleanup;
        }
        int available = client->available();
        if (available > 0) {
            deadline = millis() + FW_READ_TIMEOUT_MS; // reset on data
            int toRead = min(available, FW_MAX_BINARY_CHUNK);
            int bytes  = client->read(chunk, toRead);
            if (bytes > 0) {
                mbedtls_md_update(&ctx, chunk, bytes);
                if (Update.write(chunk, bytes) != (size_t)bytes) {
                    Log_Add(6005);
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
            Log_Add(6006);
            Update.abort();
            goto cleanup;
        }

        if (!Update.end(true)) {
            Log_Add(6007);
            goto cleanup;
        }

        Log_Add(6008);
        ok = true;
    }

cleanup:
    mbedtls_md_free(&ctx);
    client->stop();
    delete client;
    return ok;
}


void OtaPull_setOffer(JsonVariantConst fw_update)
{
    const char* v    = fw_update["version"] | "";
    const char* s    = fw_update["sha256"]  | "";
    size_t      size = fw_update["size"]    | (size_t)0;

    if (fw_update.isNull() || !fw_version_valid(v) || strlen(s) != 64 || size == 0) {
        if (!fw_update.isNull()) Log_Add(6013);
        offer_version = "";
        offer_sha256  = "";
        offer_size    = 0;
        return;
    }

    String version = String(v);
    bool   is_new  = version != offer_version;
    offer_version  = version;
    offer_sha256   = String(s);
    offer_sha256.toLowerCase();
    offer_size     = size;

    if (version == FIRMWARE_VERSION) return;
    if (version == fw_bad_version()) {
        if (is_new) Log_Add(6025); // log once per offer, not on every backend call
        return;
    }
    if (version == failed_version && millis() - failed_at < FW_RETRY_BACKOFF_MS) return;
    g_ota_check_requested = true;
}

bool OtaPull_query(String& version_out)
{
    bool ok = fw_backend_test(true);
    version_out = offer_version;
    return ok;
}

bool OtaPull_isRolledBack(const String& version)
{
    return !version.isEmpty() && version == fw_bad_version();
}

void OtaPull_boot()
{
    Preferences prefs;
    prefs.begin("ota", true);
    bool pending = prefs.getBool("pending", false);
    prefs.end();

    // PENDING_VERIFY: first boot of an OTA image, not confirmed yet (NEW only
    // with a bootloader without rollback support, which keeps that state).
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    bool have_state = esp_ota_get_state_partition(running, &state) == ESP_OK;
    bool unconfirmed = have_state &&
        (state == ESP_OTA_IMG_PENDING_VERIFY || state == ESP_OTA_IMG_NEW);

    if (pending && unconfirmed) {
        validating = true;
        Log_Add(6009);
        DLOGLN("OTA: post-update boot — validating firmware");
        return;
    }

    // Not a pulled update awaiting validation (normal boot, espota or /update
    // upload): confirm the running image, as the Arduino core did before.
    esp_ota_mark_app_valid_cancel_rollback();
    if (!pending) return;

    // pending, but the running image is a confirmed one: the pulled update
    // restarted before it was validated and the bootloader booted this
    // (previous) firmware again. Its version is already in bad_ver.
    prefs.begin("ota", false);
    prefs.remove("pending");
    if (have_state) prefs.putBool("rollback", true); // 15 min cooldown in OtaPull_check()
    prefs.end();
    if (have_state) {
        Log_Add(6028);
        DLOGLN("OTA: new firmware did not get validated — bootloader rolled back");
    }
}

// Validation failed for good: mark this image invalid and boot the previous
// one. bad_ver already holds this version (written before the update restart).
static void fw_rollback()
{
    Log_Add(6011);
    DLOGLN("OTA: validation failed — rolling back");
    Preferences prefs;
    prefs.begin("ota", false);
    prefs.remove("pending");
    prefs.putBool("rollback", true);
    prefs.putString("bad_ver", FIRMWARE_VERSION);
    prefs.end();
    if (wifi_connected && xSemaphoreTake(Sema_Backend, pdMS_TO_TICKS(30000))) {
        Webclient_send_log_to_backend();
        xSemaphoreGive(Sema_Backend);
    }
    esp_ota_mark_app_invalid_rollback_and_reboot();
    // Only returns if that failed (no PENDING_VERIFY state, e.g. a bootloader
    // without rollback support): switch the boot partition directly.
    const esp_partition_t* prev = esp_ota_get_next_update_partition(NULL);
    if (prev) esp_ota_set_boot_partition(prev);
    delay(200);
    esp_restart();
}

void OtaPull_validate()
{
    if (!validating) return;

    // One attempt per minute while WiFi is up. The backend or the network can
    // be briefly unavailable right after the restart; a single failed attempt
    // must not roll back (and block) a working firmware.
    bool due = !validate_tried || millis() - validate_last_try >= FW_VALIDATE_RETRY_MS;
    if (wifi_connected && due && xSemaphoreTake(Sema_Backend, 0) == pdTRUE) {
        validate_tried    = true;
        validate_last_try = millis();
        bool reached = fw_backend_test(false);
        xSemaphoreGive(Sema_Backend);

        if (reached) {
            validating = false;
            // Clear the NVS flags before confirming the image: a restart in
            // between then rolls back to the previous firmware, which simply
            // gets this version offered again.
            Preferences prefs;
            prefs.begin("ota", false);
            prefs.remove("pending");
            if (fw_bad_version() == FIRMWARE_VERSION) {
                prefs.remove("bad_ver");
                bad_version = "";
            }
            prefs.end();
            esp_ota_mark_app_valid_cancel_rollback();
            Log_Add(6010);
            DLOGLN("OTA: firmware validated");
            return;
        }
        Log_Add(6027);
        DLOGLN("OTA: validation attempt failed — retrying");
    }

    // Also without WiFi: a firmware that never gets a connection is not
    // working either. millis() is the time since boot.
    if (millis() >= FW_VALIDATE_DEADLINE_MS) fw_rollback();
}

void OtaPull_check(bool manual)
{
    if (!wifi_connected)       { Log_Add(6016); return; }
    if (ota_active)            { Log_Add(6017); return; }
    if (strlen(backend_ID)==0) { Log_Add(6018); return; }
    if (backend_host.isEmpty()) { Log_Add(6019); return; }
    if (validating)            { Log_AddWithoutTransmit(6029); return; } // confirm the running update first

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
            Log_Add(6020);
        }
    }
    if (millis() < ota_blocked_until) return;

    Log_AddWithoutTransmit(6000);
    ota_active = true;

    if (!xSemaphoreTake(Sema_Backend, pdMS_TO_TICKS(30000))) {
        ota_active = false;
        return;
    }

    String version = offer_version;
    String sha256  = offer_sha256;
    size_t size    = offer_size;

    DLOGF("OTA: offered=%s current=%s\n", version.c_str(), FIRMWARE_VERSION);

    if (version.isEmpty() || version == FIRMWARE_VERSION) {
        Log_AddWithoutTransmit(6002);
        xSemaphoreGive(Sema_Backend);
        ota_active = false;
        return;
    }
    // Automatic checks only start for allowed offers (OtaPull_setOffer); the
    // offer may have changed since, so check again unless the user confirmed.
    if (!manual && (version == fw_bad_version() ||
                    (version == failed_version && millis() - failed_at < FW_RETRY_BACKOFF_MS))) {
        xSemaphoreGive(Sema_Backend);
        ota_active = false;
        return;
    }

    Log_Add(6003);

    bool flashed = fw_download_and_flash(version, sha256, size);
    xSemaphoreGive(Sema_Backend);

    if (!flashed) {
        failed_version = version;
        failed_at      = millis();
        ota_active = false;
        return;
    }

    // The new image boots next (Update.end() switched the boot partition), so
    // set the flags before anything else can restart the device. bad_ver: see
    // bad_version above.
    Preferences prefs;
    prefs.begin("ota", false);
    prefs.putBool("pending", true);
    prefs.putString("bad_ver", version);
    prefs.end();
    bad_version = version;

    Webclient_send_log_to_backend();

    delay(500);
    esp_restart();
}
