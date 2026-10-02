<?php

include("credentials.php");

function authenticate(): string {
    global $_link;
    $id    = $_GET['ID']    ?? '';
    $token = $_SERVER['HTTP_X_AUTH_TOKEN'] ?? '';
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
    return $id;
}

// Sends $data as JSON with an explicit Content-Length (prevents chunked transfer
// encoding, which the ESP32 response reader does not decode) and ends the script.
function json_response(array $data, int $status = 200): void {
    http_response_code($status);
    $body = json_encode($data);
    header('Content-Type: application/json');
    header('Content-Length: ' . strlen($body));
    echo $body;
    exit;
}

// Acknowledgement for an uploaded binary payload. The device compares both
// values with what it sent and only discards its buffer if they match, so a
// misconfigured server that answers 200 without running this code cannot make
// the device drop data. Must only be sent after the payload was fully processed.
function payload_ack(string $raw): array {
    return ['bytes' => strlen($raw), 'crc32' => hash('crc32b', $raw)];
}

// ---------------------------------------------------------------------------
// Firmware assignment (firmware >= 1.4.0, see FW_UPDATE.md)
// Both the assignment and the release binaries live next to this file, i.e.
// outside the web root, so neither is publicly readable.
// ---------------------------------------------------------------------------
define('FW_TARGETS_FILE', __DIR__ . '/fw_targets.php');
define('FW_RELEASES_DIR', __DIR__ . '/fw_releases');

// Value of a version/target URL parameter (fw, hw, version), or null if
// absent or invalid. Only these characters can reach file paths and responses.
function fw_param(string $name): ?string {
    $v = $_GET[$name] ?? '';
    return (is_string($v) && preg_match('/^[0-9A-Za-z._-]{1,32}$/', $v)) ? $v : null;
}

// Firmware version assigned to a device: its own entry in fw_targets.php,
// otherwise 'default'. An explicit null entry exempts the device from 'default'.
function fw_target_version(string $id): ?string {
    if (!is_file(FW_TARGETS_FILE)) return null;
    $targets = include FW_TARGETS_FILE;
    if (!is_array($targets)) return null;
    $devices = $targets['devices'] ?? [];
    $v = array_key_exists($id, $devices) ? $devices[$id] : ($targets['default'] ?? null);
    return (is_string($v) && $v !== '') ? $v : null;
}

// Release manifest for fw_releases/v<version>/<hw>/manifest.json (same layout
// as manifests/ in the firmware repo), with 'path' set to the binary. Returns
// null if the release is missing or inconsistent.
function fw_release_manifest(?string $version, ?string $hw): ?array {
    if ($version === null || $hw === null) return null;
    if (!preg_match('/^[0-9A-Za-z._-]{1,32}$/', $version) || !preg_match('/^[0-9A-Za-z_-]{1,32}$/', $hw)) return null;
    $dir  = FW_RELEASES_DIR . "/v$version/$hw";
    $json = @file_get_contents("$dir/manifest.json");
    $m    = $json !== false ? json_decode($json, true) : null;
    if (!is_array($m) || ($m['version'] ?? '') !== $version) return null;
    $file   = basename($m['filename'] ?? '');
    $sha256 = strtolower($m['sha256'] ?? '');
    if ($file === '' || !is_file("$dir/$file") || !preg_match('/^[0-9a-f]{64}$/', $sha256)) return null;
    if ((int)($m['size'] ?? -1) !== filesize("$dir/$file")) return null;
    $m['sha256'] = $sha256;
    $m['path']   = "$dir/$file";
    return $m;
}

// fw_update object for a response, or null if the device runs its assigned
// version. Firmware before 1.4.0 sends no fw/hw and is handled by the old
// per-device fwupdate/<ID>/manifest.json instead.
function fw_update_for(string $id, ?string $fw, ?string $hw): ?array {
    if ($fw === null || $hw === null) return null;
    $target = fw_target_version($id);
    if ($target === null || $target === $fw) return null;
    $m = fw_release_manifest($target, $hw);
    if ($m === null) {
        error_log("fw_update: release v$target/$hw missing or invalid (device $id)");
        return null;
    }
    return ['version' => $target, 'sha256' => $m['sha256'], 'size' => (int)$m['size']];
}

function update_client_endpoint(string $id, ?string $wireframe = null, ?string $fw_version = null, ?string $cfg_version = null): void {
    global $_link;
    $scheme   = (!empty($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off') ? 'https' : 'http';
    $endpoint = $scheme . '://' . ($_SERVER['HTTP_HOST'] ?? 'unknown') . ($_SERVER['SCRIPT_NAME'] ?? '');
    $ip_raw   = isset($_GET['IP']) ? (int)$_GET['IP'] : -1;
    $ip_octet = ($ip_raw >= 0 && $ip_raw <= 255) ? $ip_raw : null;

    $sets   = ["endpoint = ?", "last_reading = NOW()"];
    $types  = "s";
    $params = [$endpoint];

    if ($ip_octet !== null)    { $sets[] = "ip_last_octet = ?"; $types .= "i"; $params[] = $ip_octet; }
    if ($wireframe !== null)   { $sets[] = "wireframe = ?";     $types .= "s"; $params[] = $wireframe; }
    if ($fw_version !== null)  { $sets[] = "fw_version = ?";    $types .= "s"; $params[] = $fw_version; }
    if ($cfg_version !== null) { $sets[] = "cfg_version = ?";   $types .= "s"; $params[] = $cfg_version; }

    $params[] = $id;
    $types   .= "s";

    $stmt = mysqli_prepare($_link, "UPDATE clients SET " . implode(", ", $sets) . " WHERE device_id = ?");
    mysqli_stmt_bind_param($stmt, $types, ...$params);
    mysqli_stmt_execute($stmt);
    mysqli_stmt_close($stmt);
}
