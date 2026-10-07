# Firmware updates (firmware >= 1.4.0)

Which firmware a device should run is assigned centrally on the server. Every
backend call of a device reports its running version (`fw`) and build target
(`hw`, e.g. `esp32c3` or `esp32-nodemcu`). If the assigned version differs,
the response contains an `fw_update` object; the device then downloads the
binary via `fw_download.php`, checks the SHA-256, flashes it and reboots.
Devices also call the backend when they have no stored values, so an
assignment reaches every device within one backend interval.

Devices with firmware up to 1.3.12 do not report `fw`/`hw` and keep using the
old per-device folder `fwupdate/<ID>/` (see `fwupdate/ABC/DEPLOY.md`). Update
them once that way to 1.4.0 or later; from then on only the central assignment
applies.

## Server layout

Both live next to `config.php`, i.e. outside the web root, so neither the
assignment nor the binaries are publicly readable:

```
config.php
fw_targets.php                      # assignment (copy of fw_targets.php.TEMPLATE)
fw_releases/
└── v1.4.0/
    ├── esp32c3/
    │   ├── manifest.json
    │   └── smartmeterlite_v1.4.0_esp32c3.bin
    └── esp32-nodemcu/
        ├── manifest.json
        └── smartmeterlite_v1.4.0_esp32-nodemcu.bin
<web root>/
├── index.php
├── log.php
└── fw_download.php                 # authenticated download (X-Auth-Token)
```

`fw_releases/` has the same layout as `manifests/` in the firmware repo, so a
release folder can be uploaded as is (the `.bin` files are not in git; they are
attached to the GitHub release).

## Assigning a version

`fw_targets.php`:

```php
return [
    'default' => '1.4.0',        // all devices without their own entry; null = no update
    'devices' => [
        'BF1' => '1.4.1',        // this device gets 1.4.1
        'ABC' => null,           // this device is exempt from 'default'
    ],
];
```

- A device updates whenever its running version differs from the assigned
  one, so assigning an older version downgrades it.
- Every assigned version needs `fw_releases/v<version>/<hw>/` for the build
  target of the device. If it is missing or inconsistent (version, sha256 or
  size do not match the file), no update is offered and the backend writes a
  line to the PHP error log.
- After the update the device checks the backend (`backend_test`) once per
  minute (`6027` on failure). If it hasn't succeeded within 15 min after boot
  (`6011`), or the new firmware restarts before that, e.g. a crash (`6028`,
  firmware >= 1.4.3), the device boots the previous firmware again.
- A version that fails its post-update check on the device is rolled back and
  never installed automatically again on that device (log `6025`); it can
  still be installed from the "Check Remote FW Update" page.

## Acknowledgement of uploads

`index.php` and `log.php` answer with `bytes` and `crc32` of the received
payload. Firmware >= 1.4.0 only discards its meter value buffer (or marks its
log as sent) if both match what it sent, so a misconfigured server that answers
HTTP 200 without running the backend cannot make devices drop data. Inserts run
in a transaction; on a database error the backend answers 500 without an
acknowledgement and the device resends the data on the next call.

`backend_test` answers `{"ok":true,"id":"<ID>"}` (plus `fw_update` if
assigned). The device's connection test and its post-update check require this
body, not just HTTP 200.
