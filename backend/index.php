<?php
// Uncomment the lines below for verbose error output during development.
// Never enable these on a production server.
// ini_set('display_errors', 1);
// ini_set('display_startup_errors', 1);
// error_reporting(E_ALL);

include("valid_clients.php");

// ---------------------------------------------------------------------------
// Authentication
// The client ID is always passed as a URL parameter.
// The token can be passed as a URL parameter or as the X-Auth-Token header.
// Sending it as a header ("token=header") is preferred because URL parameters
// may appear in server access logs.
// ---------------------------------------------------------------------------
$id    = $_GET['ID']    ?? '';
$token = $_GET['token'] ?? '';

if ($token === "header") {
    $token = $_SERVER['HTTP_X_AUTH_TOKEN'] ?? '';
}

// Reject unknown or invalid clients immediately
if (!isset($valid_clients[$id]) || !hash_equals($valid_clients[$id], $token)) {
    http_response_code(403);
    echo "Access denied.";
    exit;
}

// ---------------------------------------------------------------------------
// Early-exit for connectivity tests
// The ESP32 calls this endpoint with backend_test=true on startup and from
// the "Test Backend Connection" page to verify reachability and credentials.
// ---------------------------------------------------------------------------
if (isset($_GET['backend_test']) && $_GET['backend_test'] === "true") {
    http_response_code(200);
    exit;
}

// ---------------------------------------------------------------------------
// Field manifest parser  (Written by Claude)
//
// New clients send a self-describing "fields=" URL parameter that lists every
// field present in each binary entry in order:
//
//   fields=ts,m180[,temp][,solar][,m280]
//
// This replaces the old PV_included / 280_included boolean flags and makes
// the protocol forward-compatible: adding a new field only requires adding a
// new token here, without touching the entry-size logic.
//
// Backward compatibility: old clients that do not send "fields=" are handled
// by the legacy fallback block below, which reconstructs the field list from
// the old PV_included and 280_included parameters.
// ---------------------------------------------------------------------------

// Known field tokens and their byte sizes
$known_fields = [
    'ts'    => 4,   // Unix timestamp (uint32_t)
    'm180'  => 4,   // OBIS 1.8.0 consumption counter (uint32_t, unit: 0.1 Wh)
    'temp'  => 4,   // temperature * 100 (uint32_t, e.g. 2150 = 21.50 degC)
    'solar' => 4,   // PV / MyStrom energy counter (uint32_t)
    'm280'  => 4,   // OBIS 2.8.0 feed-in counter (uint32_t, unit: 0.1 Wh)
];

if (isset($_GET['fields']) && $_GET['fields'] !== '') {
    // --- New protocol: parse the self-describing field manifest ---
    $field_tokens = explode(',', $_GET['fields']);
    $active_fields = [];
    foreach ($field_tokens as $token) {
        $token = trim($token);
        if (isset($known_fields[$token])) {
            $active_fields[$token] = $known_fields[$token];
        }
        // Unknown tokens are silently ignored for forward-compatibility.
        // If a future firmware adds a new field, old backends simply skip it —
        // UNLESS it changes the byte size of existing fields, which it won't.
    }
} else {
    // --- Legacy fallback: reconstruct field list from old boolean flags ---
    // Clients that pre-date the "fields=" parameter send separate flags.
    // ts and m180 are always present in the old protocol.
    $active_fields = ['ts' => 4, 'm180' => 4];

    // Old clients always stored temperature; include it in the legacy path.
    // Note: if an old client sent PV_included without 280_included, the old
    // backend had a bug (it hardcoded offset 16 for 280). This is fixed here.
    $active_fields['temp'] = 4;

    if (isset($_GET['PV_included'])  && $_GET['PV_included']  === "true") {
        $active_fields['solar'] = 4;
    }
    if (isset($_GET['280_included']) && $_GET['280_included'] === "true") {
        $active_fields['m280'] = 4;
    }
}

// Calculate entry size in bytes from the active field list
$entrySize = array_sum($active_fields);

// ---------------------------------------------------------------------------
// Read and validate the raw binary POST body.
// The ESP32 sends the entire packed ring-buffer in one request, including
// uninitialised (zero-filled) slots. Those are filtered out below.
// ---------------------------------------------------------------------------
$rawData = file_get_contents('php://input');
$rawLen  = strlen($rawData);

// Reject payloads whose length is not an exact multiple of the entry size.
// A mismatch means the field manifest in the URL does not match the firmware,
// or the request body was truncated.
if ($rawLen === 0 || ($rawLen % $entrySize) !== 0) {
    http_response_code(400);
    $fields_str = implode(',', array_keys($active_fields));
    echo "Invalid data length: $rawLen bytes, entry size $entrySize (fields: $fields_str).";
    exit;
}

$dataCount = $rawLen / $entrySize;

// ---------------------------------------------------------------------------
// Parse binary entries  (Written by Claude)
//
// Each entry is read field by field using the ordered $active_fields map.
// The running byte offset advances by each field's size, so the parser is
// correct for any combination of enabled fields without hardcoded offsets.
// ---------------------------------------------------------------------------
$entries = [];

for ($i = 0; $i < $dataCount; $i++) {

    $o = $i * $entrySize; // byte offset of this entry in the raw buffer
    $parsed = [];

    foreach ($active_fields as $field_name => $field_size) {
        // "V" = unsigned 32-bit little-endian, matching uint32_t on the ESP32
        $parsed[$field_name] = unpack("V", substr($rawData, $o, $field_size))[1];
        $o += $field_size;
    }

    // Assign parsed fields to named variables with safe defaults
    $timestamp   = $parsed['ts']    ?? 0;
    $meter       = $parsed['m180']  ?? 0;
    $temperature = $parsed['temp']  ?? 0;
    $meter_solar = $parsed['solar'] ?? null;
    $obis280     = $parsed['m280']  ?? null;

    // 0xFFFFFFFF is the client sentinel for "no valid solar reading available"
    // (e.g. MyStrom unreachable). Treat it as null so the DB receives NULL.
    if ($meter_solar === 4294967295) $meter_solar = null;

    // Skip uninitialised / empty slots.
    // The ESP32 zeroes the entire buffer on init and after a successful send,
    // so meter_value_180 == 0 reliably identifies an unused slot.
    if ($meter === 0) continue;

    $entries[] = [
        "timestamp"   => $timestamp,
        "meter"       => $meter,
        "temperature" => $temperature,
        "meter_solar" => $meter_solar,
        "obis280"     => $obis280,
    ];
}

// ---------------------------------------------------------------------------
// Sort entries by client timestamp ascending before inserting.
// The ring-buffer on the ESP32 is not guaranteed to be in chronological
// order: TAF7 (override) entries fill from index 0 upward while TAF14
// (non-override) entries fill from the last index downward, so the two
// groups can interleave in the raw payload.
// ---------------------------------------------------------------------------
usort($entries, fn($a, $b) => $a["timestamp"] <=> $b["timestamp"]);

// ---------------------------------------------------------------------------
// Optional file logging (disabled by default)  
// Set $enable_file_log = true and adjust $log_id to enable CSV logging
// for a specific client ID. Useful for debugging a single device without
// touching the database. Files are written to the log/ subdirectory.
// CSV columns: timestamp;meter;temperature;meter_solar;obis280
// ---------------------------------------------------------------------------
$enable_file_log = false; // set to true to enable
$log_id          = "BF1"; // only log entries from this client ID

if ($enable_file_log && $id === $log_id && count($entries) > 0) {
    $filename = date("y-m-d-H-i-s") . "-" . $id . ".txt";
    $file = fopen("log/" . $filename, "w");
    if ($file !== false) {
        foreach ($entries as $entry) {
            $line = implode(";", [
                $entry["timestamp"],
                $entry["meter"],
                $entry["temperature"] ?? "",
                $entry["meter_solar"] ?? "",
                $entry["obis280"]     ?? "",
            ]) . "\n";
            fwrite($file, $line);
        }
        fclose($file);
    }
}

// Respond 200 immediately so the ESP32 knows the transfer succeeded and can
// clear its local buffer, even if the DB inserts below take a moment.
http_response_code(200);
echo "Data received. Fields: " . implode(',', array_keys($active_fields)) . ". Entries: " . count($entries) . ".\n";

// ---------------------------------------------------------------------------
// Database: fetch the last known state for this client
// ---------------------------------------------------------------------------
include("../config.php");

$prev = ["timestamp" => 0, "meter" => 0];

// Use a prepared statement to prevent SQL injection on the $id field.
// The original code built the query with string concatenation, which allowed
// an attacker with a valid token to manipulate the query via the ID parameter.
$stmt = mysqli_prepare($_link,
    "SELECT `meter_value`, `timestamp_client`
     FROM `sml_v1`
     WHERE `id` = ?
     ORDER BY `timestamp_client` DESC
     LIMIT 1"
);
mysqli_stmt_bind_param($stmt, "s", $id);
mysqli_stmt_execute($stmt);
$result = mysqli_stmt_get_result($stmt);
if ($row = mysqli_fetch_array($result)) {
    $prev["meter"]     = $row['meter_value'];
    $prev["timestamp"] = $row['timestamp_client'];
}
mysqli_stmt_close($stmt);

// ---------------------------------------------------------------------------
// Database: insert valid entries
// The INSERT statement is prepared once and executed for each valid entry
// to avoid re-parsing overhead and to keep the code safe from injection.
// ---------------------------------------------------------------------------
$insert = mysqli_prepare($_link,
    "INSERT INTO `sml_v1`
        (`i`, `id`, `timestamp_server2`, `timestamp_client`,
         `meter_value`, `meter_value_PV`, `temperature`, `obis280`)
     VALUES
        (NULL, ?, ?, ?, ?, ?, ?, ?)"
);

$current_time = time(); // single server timestamp for all inserts in this batch
$inserted  = 0;
$diag_rows = []; // collects all entries with validation result for diagnostic log

foreach ($entries as $item) {

    echo $item["timestamp"] . " " . $item["meter"] . " " . ($item["meter_solar"] ?? "null") . "\n";

    $rejection = null;

    // Entries older than the last known DB entry arrived out of order —
    // typically a TAF14 reading buffered before a TAF7 entry that was already
    // inserted in a previous backend call (e.g. connection dropped after the
    // server responded 200 but before the ESP received it).
    // Skip without advancing $prev so the next newer entry validates correctly.
    if ($item["timestamp"] < $prev["timestamp"]) {
        $rejection = "older_than_db_prev";
    }
    // Skip duplicate timestamps — the client may retry a failed send
    elseif ($item["timestamp"] == $prev["timestamp"]) {
        $rejection = "duplicate_timestamp";
    }

    if ($rejection === null) {
        // Compute average power between consecutive readings.
        // meter values are in 0.1 Wh units; timestamps are Unix seconds.
        $delta_energy = ($item["meter"] - $prev["meter"]) / 10.0;           // Wh
        $delta_time   = ($item["timestamp"] - $prev["timestamp"]) / 3600.0; // hours
        $power        = ($delta_time > 0) ? ($delta_energy / $delta_time) : 0;

        if ($prev["meter"] > $item["meter"]) {
            $rejection = "meter_rollback(prev=" . $prev["meter"] . ")";
        } elseif ($power < 0 || $power > 40000) {
            $rejection = "power_out_of_range(" . round($power) . "W)";
        }
    } else {
        $power = 0;
        $delta_energy = 0;
        $delta_time   = 0;
    }

    // Record row for diagnostic log regardless of outcome
    $diag_rows[] = [
        'ts'        => $item["timestamp"],
        'ts_fmt'    => date("Y-m-d H:i:s", $item["timestamp"]),
        'meter'     => $item["meter"],
        'solar'     => $item["meter_solar"] ?? "",
        'temp'      => isset($item["temperature"]) ? round($item["temperature"] / 100.0, 2) : "",
        'power_w'   => round($power),
        'prev_ts'   => $prev["timestamp"],
        'prev_m'    => $prev["meter"],
        'status'    => $rejection ?? "OK",
    ];

    if ($rejection !== null) {
        // do not advance prev, do not insert
        continue;
    }

    // Advance the "previous" pointer so the next entry is validated against
    // this one, not the one loaded from the database
    $prev["timestamp"] = $item["timestamp"];
    $prev["meter"]     = $item["meter"];

    // Clamp meter value to zero just in case (should never be negative here)
    $meter_val = max(0, $item["meter"]);

    // Temperature is stored as integer * 100 on the ESP32 (e.g. 2150 = 21.50 degC)
    $temp_val = isset($item["temperature"]) ? ($item["temperature"] / 100.0) : 0.0;

    // Default optional fields to 0 when they were not included in this payload
    $solar_val   = $item["meter_solar"] ?? 0;
    $obis280_val = $item["obis280"]     ?? 0;

    mysqli_stmt_bind_param($insert, "siidddd",
        $id,
        $current_time,
        $item["timestamp"],
        $meter_val,
        $solar_val,
        $temp_val,
        $obis280_val
    );
    mysqli_stmt_execute($insert);
    $inserted++;
}

mysqli_stmt_close($insert);
echo $inserted . " values inserted.";

// ---------------------------------------------------------------------------
// Diagnostic log — written whenever at least one entry was rejected.
// File: log/YYYY-MM-DD-HH-ii-ss-{ID}-diag.tsv
// Columns: timestamp_utc | ts_unix | meter | solar | temp_c | power_w |
//          prev_ts | prev_meter | status
// ---------------------------------------------------------------------------
$rejected_count = count($diag_rows) - $inserted;
if ($rejected_count > 0) {
    $diag_file = "log/" . date("y-m-d-H-i-s") . "-" . preg_replace('/[^A-Za-z0-9_-]/', '', $id) . "-diag.tsv";
    $fh = fopen($diag_file, "w");
    if ($fh) {
        fwrite($fh, "timestamp_utc\t\tts_unix\t\tmeter\t\tsolar\t\ttemp_c\tpower_w\tprev_ts\t\tprev_meter\tstatus\n");
        fwrite($fh, str_repeat("-", 120) . "\n");
        foreach ($diag_rows as $r) {
            $mark = ($r['status'] !== "OK") ? ">>> " : "    ";
            fwrite($fh, sprintf("%s%-20s\t%d\t\t%d\t\t%s\t\t%s\t%d\t%d\t\t%d\t\t%s\n",
                $mark,
                $r['ts_fmt'],
                $r['ts'],
                $r['meter'],
                $r['solar'],
                $r['temp'],
                $r['power_w'],
                $r['prev_ts'],
                $r['prev_m'],
                $r['status']
            ));
        }
        fwrite($fh, "\nSummary: " . count($diag_rows) . " received, $inserted inserted, $rejected_count rejected.\n");
        fclose($fh);
    }
}
?>
