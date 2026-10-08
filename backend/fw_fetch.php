<?php
// ---------------------------------------------------------------------------
// Fetches a firmware release from GitHub into FW_RELEASES_DIR (see FW_UPDATE.md)
// Called by hand once the release is published on GitHub:
//   fw_fetch.php?version=<version>&key=<$_fw_fetch_key>[&replace=1]
// The manifests come from the git tag v<version> (manifests/v<version>/<hw>/),
// the binaries from the GitHub release with that tag. Per build target only
// the binary and manifest.json are written, and only if size and SHA-256 of
// the binary match the manifest. A different release that is already present
// is only replaced with replace=1.
// ---------------------------------------------------------------------------
include("../config.php");

const FW_FETCH_REPO      = 'fourwheel/SMGW.Lite';
const FW_FETCH_TARGETS   = ['esp32c3', 'esp32-nodemcu'];
const FW_FETCH_MAX_BYTES = 4 * 1024 * 1024; // app partition is ~1.9 MB

header('Content-Type: text/plain; charset=utf-8');

// Disabled unless credentials.php sets a key of at least 16 characters.
$key = $_GET['key'] ?? '';
if (!isset($_fw_fetch_key) || !is_string($_fw_fetch_key) || strlen($_fw_fetch_key) < 16 ||
    !is_string($key) || !hash_equals($_fw_fetch_key, $key)) {
    http_response_code(403);
    echo "Access denied.\n";
    exit;
}
if (!function_exists('curl_init')) {
    http_response_code(500);
    echo "PHP curl extension missing.\n";
    exit;
}
$version = fw_param('version');
if ($version === null) {
    http_response_code(400);
    echo "Missing or invalid version.\n";
    exit;
}
$replace = ($_GET['replace'] ?? '') === '1';
set_time_limit(300); // two downloads of up to 120 s each

// HTTPS GET (redirects allowed, HTTPS only). Returns the body of a 200
// response, or null on any error or if it is larger than $max bytes.
function fw_fetch_url(string $url, int $max): ?string {
    $ch = curl_init($url);
    curl_setopt_array($ch, [
        CURLOPT_RETURNTRANSFER   => true,
        CURLOPT_FOLLOWLOCATION   => true, // release assets redirect to GitHub's CDN
        CURLOPT_MAXREDIRS        => 5,
        CURLOPT_PROTOCOLS        => CURLPROTO_HTTPS,
        CURLOPT_REDIR_PROTOCOLS  => CURLPROTO_HTTPS,
        CURLOPT_CONNECTTIMEOUT   => 15,
        CURLOPT_TIMEOUT          => 120,
        CURLOPT_USERAGENT        => 'smartmeterlite-fw-fetch',
        CURLOPT_NOPROGRESS       => false,
        CURLOPT_PROGRESSFUNCTION => function ($ch, $total, $now) use ($max) { return $now > $max ? 1 : 0; },
    ]);
    $body   = curl_exec($ch);
    $status = curl_getinfo($ch, CURLINFO_RESPONSE_CODE);
    return (is_string($body) && $status === 200 && strlen($body) <= $max) ? $body : null;
}

// Writes via a temporary file and rename, so no reader sees a partial file.
function fw_fetch_write(string $path, string $data): bool {
    $tmp = "$path.tmp";
    if (file_put_contents($tmp, $data) !== strlen($data)) {
        @unlink($tmp);
        return false;
    }
    return rename($tmp, $path);
}

// Fetches one build target. Returns [ok, message].
function fw_fetch_target(string $version, string $hw, bool $replace): array {
    $tag  = "v$version";
    $json = fw_fetch_url("https://raw.githubusercontent.com/" . FW_FETCH_REPO . "/$tag/manifests/$tag/$hw/manifest.json", 4096);
    if ($json === null) return [false, "no manifest in tag $tag"];

    $m = json_decode($json, true);
    if (!is_array($m)) return [false, "invalid manifest"];
    $file   = $m['filename'] ?? '';
    $sha256 = strtolower((string)($m['sha256'] ?? ''));
    $size   = $m['size'] ?? null;
    if (($m['version'] ?? '') !== $version || $file !== "smartmeterlite_{$tag}_$hw.bin" ||
        !preg_match('/^[0-9a-f]{64}$/', $sha256) || !is_int($size) || $size <= 0 || $size > FW_FETCH_MAX_BYTES) {
        return [false, "invalid manifest"];
    }

    $dir = FW_RELEASES_DIR . "/$tag/$hw";
    $present = fw_release_manifest($version, $hw);
    if ($present !== null && $present['sha256'] === $sha256) return [true, "already present"];
    $existed = is_file("$dir/manifest.json") || is_file("$dir/$file");
    if ($existed && !$replace) {
        return [false, "a different release is present, add replace=1 to replace it"];
    }

    $bin = fw_fetch_url("https://github.com/" . FW_FETCH_REPO . "/releases/download/$tag/$file", FW_FETCH_MAX_BYTES);
    if ($bin === null) return [false, "$file not found in release $tag"];
    if (strlen($bin) !== $size || hash('sha256', $bin) !== $sha256) {
        return [false, "$file does not match the manifest (size/sha256)"];
    }

    if (!is_dir($dir) && !mkdir($dir, 0755, true)) return [false, "cannot create $tag/$hw"];
    // Binary first, manifest last: fw_release_manifest() only accepts a
    // manifest that matches the binary, so devices are never offered a
    // half-written release.
    if (!fw_fetch_write("$dir/$file", $bin) || !fw_fetch_write("$dir/manifest.json", $json)) {
        return [false, "writing $tag/$hw failed"];
    }
    return [true, ($existed ? "replaced" : "fetched") . ", sha256 $sha256, $size bytes"];
}

$all_ok = true;
foreach (FW_FETCH_TARGETS as $hw) {
    [$ok, $msg] = fw_fetch_target($version, $hw, $replace);
    $all_ok = $all_ok && $ok;
    echo "v$version/$hw: " . ($ok ? "OK" : "ERROR") . " - $msg\n";
}
if (!$all_ok) http_response_code(502);
?>
