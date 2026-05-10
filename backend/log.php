<?php
// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------
include("../config.php");

$id    = $_GET['ID']    ?? '';
$token = $_GET['token'] ?? '';

if ($token === "header") {
    $token = $_SERVER['HTTP_X_AUTH_TOKEN'] ?? '';
}

// Look up the stored SHA-256 token hash for this client ID.
$stmt = mysqli_prepare($_link, "SELECT token FROM clients WHERE device_id = ? LIMIT 1");
mysqli_stmt_bind_param($stmt, "s", $id);
mysqli_stmt_execute($stmt);
$result = mysqli_stmt_get_result($stmt);
$row    = mysqli_fetch_assoc($result);
mysqli_stmt_close($stmt);

if (!$row || !hash_equals($row['token'], hash('sha256', $token))) {
    http_response_code(403);
    echo "Access denied.";
    exit;
}

// ---------------------------------------------------------------------------
// Status code descriptions
// Kept in sync with Log_StatusCodeToString() in main.cpp.
// Codes < 1000: number of meter values transmitted (handled separately below).
// ---------------------------------------------------------------------------
function getStatusDescription(int $statusCode): string
{
    // Codes below 1000 carry a dynamic value (number of transmitted meter values)
    if ($statusCode >= 0 && $statusCode < 1000) {
        return "# values transmitted: $statusCode";
    }

    switch ($statusCode) {
        // --- System ---
        case 1001: return "setup()";
        case 1002: return "Memory allocation failed";
        case 1003: return "Config saved";
        case 1004: return "Buffer layout changed, re-initialising";

        // --- Backend calls ---
        case 1005: return "call_backend()";
        case 1012: return "call backend trigger";
        case 1019: return "Sending Log";
        case 1020: return "Sending Log successful";
        case 1021: return "call_backend successful";

        // --- TAF triggers ---
        case 1006: return "TAF 6 meter reading trigger";
        case 1010: return "TAF 7 meter reading trigger";
        case 1011: return "TAF 14 meter reading trigger";
        case 1014: return "TAF 7-900s meter reading trigger";
        case 1018: return "Dynamic TAF trigger";
        case 1022: return "TAF 14 trigger not possible, buffer full";
        case 1023: return "No backend host configured, skipping";

        // --- Meter value buffer ---
        case 1013: return "MeterValues_clear_Buffer()";
        case 1015: return "Not enough heap to store value";
        case 1016: return "Buffer full, cannot store non-override value";
        case 1017: return "Meter value stored";
        case 1206: return "Buffer full, cannot store non-override value";

        // --- Meter value validation ---
        case 1200: return "Meter value <= 0";
        case 1201: return "Current meter value = previous meter value";
        case 1203: return "Suffix must not be 0";
        case 1204: return "Prefix/suffix not correct";
        case 1205: return "Error: buffer size exceeded";

        // --- WiFi ---
        case 1008: return "WiFi returned";
        case 1009: return "WiFi lost";
        case 7000: return "Stopping WiFi, backend call unsuccessful";
        case 7001: return "Restarting WiFi";

        // --- Telegram / serial ---
        case 3000: return "Complete telegram received";
        case 3001: return "Telegram buffer overflow";
        case 3002: return "Telegram timeout";
        case 3003: return "Protocol detected: SML";
        case 3004: return "Protocol detected: IEC 62056-21";

        // --- Backend connection ---
        case 4000: return "Connection to server failed (certificate?)";
        case 4001: return "Error transmitting Buffer Chunk";

        // --- MyStrom / PV ---
        case 5000: return "myStrom: connection failed";
        case 5001: return "myStrom: failed to connect";
        case 5002: return "myStrom: deserializeJson() failed";

        // --- SPIFFS / certificates ---
        case 8000: return "SPIFFS not mounted";
        case 8001: return "Error reading cert file";
        case 8002: return "Cert saved";
        case 8003: return "Error writing cert file";
        case 8004: return "No cert received";

        default:   return "Unknown status code ($statusCode)";
    }
}

// ---------------------------------------------------------------------------
// Parse binary log buffer
// Matches the LogEntry struct in main.cpp:
//   unsigned long timestamp  (4 bytes, little-endian unsigned)
//   unsigned long uptime     (4 bytes, little-endian unsigned)
//   int           statusCode (4 bytes, little-endian signed)
// ---------------------------------------------------------------------------
// Reject oversized payloads before reading them into memory.
$contentLength = (int)($_SERVER['CONTENT_LENGTH'] ?? 0);
if ($contentLength > 36864) {
    http_response_code(413);
    echo "Payload too large.";
    exit;
}

$inputData    = file_get_contents("php://input");
$logEntrySize = 12; // 3 x 4 bytes
$logEntries   = [];

for ($i = 0; $i + $logEntrySize <= strlen($inputData); $i += $logEntrySize) {
    $entry      = substr($inputData, $i, $logEntrySize);
    $timestamp  = unpack("V", substr($entry, 0, 4))[1]; // unsigned 32-bit little-endian
    $uptime     = unpack("V", substr($entry, 4, 4))[1];
    $statusCode = unpack("l", substr($entry, 8, 4))[1]; // signed 32-bit little-endian

    // Skip uninitialised entries (statusCode == -1 is the sentinel set by LogBuffer_reset())
    if ($statusCode === -1) continue;

    $logEntries[] = [
        'timestamp'   => $timestamp,
        'uptime'      => $uptime,
        'statusCode'  => $statusCode,
        'description' => getStatusDescription($statusCode),
    ];
}

// Sort descending by timestamp; use uptime (ms since boot) as tiebreaker
// for entries that share the same second.
usort($logEntries, function($a, $b) {
    if ($a['timestamp'] !== $b['timestamp'])
        return $b['timestamp'] <=> $a['timestamp'];
    return $b['uptime'] <=> $a['uptime'];
});

// ---------------------------------------------------------------------------
// Update client metadata
// ---------------------------------------------------------------------------
$scheme       = (!empty($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off') ? 'https' : 'http';
$endpoint_str = $scheme . '://' . ($_SERVER['HTTP_HOST'] ?? 'unknown') . ($_SERVER['SCRIPT_NAME'] ?? '/log.php');
$stmt = mysqli_prepare($_link,
    "UPDATE clients SET endpoint = ?, last_reading = NOW() WHERE device_id = ?"
);
mysqli_stmt_bind_param($stmt, "ss", $endpoint_str, $id);
mysqli_stmt_execute($stmt);
mysqli_stmt_close($stmt);

http_response_code(200);
echo "Log received (" . count($logEntries) . " entries).";
?>
