# Firmware Deployment — Device ID: ABC

## Folder structure

```
backend/fwupdate/ABC/
├── manifest.json    # read by the device; triggers update when version differs
└── firmware.bin     # the binary to flash
```

## Deploy a new firmware version

**1. Build the binary (PlatformIO)**
```bash
pio run -e esp32-nodemcu
# output: .pio/build/esp32-nodemcu/firmware.bin
```

**2. Compute the SHA-256 hash**
```bash
sha256sum .pio/build/esp32-nodemcu/firmware.bin
```

**3. Update `manifest.json`**

| Field      | Value                                             |
|------------|---------------------------------------------------|
| `version`  | new version string — must match `FIRMWARE_VERSION` in `main.cpp` |
| `filename` | binary filename on the server, e.g. `firmware.bin`|
| `sha256`   | 64-char lowercase hex from step 2                 |
| `size`     | byte count of the binary                          |

**4. Upload both files to the server**
```bash
scp .pio/build/esp32-nodemcu/firmware.bin  server:/backend/fwupdate/ABC/firmware.bin
scp manifest.json                           server:/backend/fwupdate/ABC/manifest.json
```

All devices with `backend_ID = ABC` will pick up the update on their next
backend call (typically within minutes), because the backend returns
`ota_check: true` whenever `manifest.json` exists. A 24 h fallback timer
triggers a check even if the backend hint is never received.

## To hold a device on its current version

Do not update `manifest.json`. The device compares the `version` field
against its running firmware and skips the download if they match.

## To roll back manually

Set `manifest.json` back to the previous version and upload the old binary.
The device will flash it on the next backend call.
