<?php
// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------
include("../config.php");

$id = authenticate();
$fw_version  = isset($_GET['fw'])  ? substr($_GET['fw'],  0, 20) : null;
$cfg_version = isset($_GET['cfg']) ? substr($_GET['cfg'], 0, 10) : null;
update_client_endpoint($id, null, $fw_version, $cfg_version);

// ---------------------------------------------------------------------------
// Update meter model if supplied
// ---------------------------------------------------------------------------
if (!empty($_GET['model'])) {
    $model = substr(preg_replace('/[^\x20-\x7E]/', '', $_GET['model']), 0, 64);
    if ($model !== '') {
        global $_link;
        $stmt = mysqli_prepare($_link, "UPDATE clients SET meter_model = ? WHERE device_id = ?");
        mysqli_stmt_bind_param($stmt, "ss", $model, $id);
        mysqli_stmt_execute($stmt);
        mysqli_stmt_close($stmt);
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
$totalSlots   = intdiv(strlen($inputData), $logEntrySize);
$logEntries   = [];

for ($i = 0; $i + $logEntrySize <= strlen($inputData); $i += $logEntrySize) {
    $entry      = substr($inputData, $i, $logEntrySize);
    $timestamp  = unpack("V", substr($entry, 0, 4))[1]; // unsigned 32-bit little-endian
    $uptime     = unpack("V", substr($entry, 4, 4))[1];
    $statusCode = unpack("l", substr($entry, 8, 4))[1]; // signed 32-bit little-endian

    // Skip uninitialised entries (statusCode == -1 is the sentinel set by LogBuffer_reset())
    if ($statusCode === -1) continue;

    $logEntries[] = [
        'physIdx'     => intdiv($i, $logEntrySize),
        'timestamp'   => $timestamp,
        'uptime'      => $uptime,
        'statusCode'  => $statusCode,
    ];
}

// Reconstruct the ring-buffer traversal order without sorting by uptime:
// The entry with the smallest uptime is the oldest — it sits right after the
// write pointer. We use its physical slot as the origin and sort every entry
// by its modular distance from that origin, which reproduces the exact order
// the firmware wrote the entries. Ties in minimum uptime use the lower physIdx
// as origin, which is correct for both the not-yet-full and the wrapped case.
if (!empty($logEntries)) {
    $minUptime = min(array_column($logEntries, 'uptime'));
    $origin    = PHP_INT_MAX;
    foreach ($logEntries as $e) {
        if ($e['uptime'] === $minUptime) {
            $origin = min($origin, $e['physIdx']);
        }
    }
    usort($logEntries, fn($a, $b) =>
        (($a['physIdx'] - $origin + $totalSlots) % $totalSlots)
        <=> (($b['physIdx'] - $origin + $totalSlots) % $totalSlots)
    );
}

// ---------------------------------------------------------------------------
// Persist log entries to the database
// ---------------------------------------------------------------------------

// Delete entries older than 30 days for this device (rolling cleanup)
$stmt = mysqli_prepare($_link,
    "DELETE FROM device_logs WHERE device_id = ? AND received_at < NOW() - INTERVAL 30 DAY");
mysqli_stmt_bind_param($stmt, "s", $id);
mysqli_stmt_execute($stmt);
mysqli_stmt_close($stmt);

// Batch-insert with INSERT IGNORE to skip duplicates (same device + timestamp +
// uptime + status_code) that occur when the device re-sends the same buffer.
$inserted = 0;
if (!empty($logEntries)) {
    $stmt = mysqli_prepare($_link,
        "INSERT IGNORE INTO device_logs (device_id, timestamp_client, uptime_ms, status_code)
         VALUES (?, ?, ?, ?)");
    foreach ($logEntries as $entry) {
        mysqli_stmt_bind_param($stmt, "siii",
            $id,
            $entry['timestamp'],
            $entry['uptime'],
            $entry['statusCode']
        );
        mysqli_stmt_execute($stmt);
        $inserted += mysqli_stmt_affected_rows($stmt);
    }
    mysqli_stmt_close($stmt);
}

http_response_code(200);
echo "Log received (" . count($logEntries) . " entries, $inserted new).";
?>
