<?php
// ---------------------------------------------------------------------------
// Authenticated firmware download (firmware >= 1.4.0)
// The device requests the release named in the fw_update object of a backend
// response: fw_download.php?ID=<id>&version=<version>&hw=<build target>
// with its X-Auth-Token header. The binary is served from FW_RELEASES_DIR,
// which lies outside the web root, so releases are not publicly downloadable.
// ---------------------------------------------------------------------------
include("../config.php");

$id = authenticate();

$m = fw_release_manifest(fw_param('version'), fw_param('hw'));
if ($m === null) {
    http_response_code(404);
    echo "No such release.";
    exit;
}

header('Content-Type: application/octet-stream');
header('Content-Length: ' . filesize($m['path']));
readfile($m['path']);
?>
