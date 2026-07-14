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
