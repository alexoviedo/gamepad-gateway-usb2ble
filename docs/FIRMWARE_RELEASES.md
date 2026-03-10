# Browser Firmware Flashing and Release Publishing

This project now supports a browser-based firmware flashing flow for end users who do **not** want to build ESP-IDF locally.

## Overview

Two distribution surfaces are used together:

1. **GitHub Releases**
   - canonical history
   - release notes
   - raw binaries for power users
   - checksums

2. **Static host feed**
   - browser-friendly `latest.json` channel feeds
   - release-scoped manifests for the web flasher
   - optional stable/beta channels

The browser UI reads a small channel feed such as:

```text
/webapp/firmware/stable/latest.json
```

That feed points to a versioned manifest such as:

```text
/webapp/firmware/releases/v1.2.3/manifest.json
```

## Browser UI

The web app includes a dedicated page at:

```text
webapp/flash.html
```

It provides:

- release feed selection (stable / beta)
- custom manifest URL loading for manual validation
- direct download links and checksums
- a browser-based install button using an ESP web flasher component

The main Config Console (`webapp/index.html`) now includes a quick-launch firmware section linking to the full flasher.

## Manifest generation

The script below stages the three flash artifacts, writes an ESP-style manifest for browser flashing, and updates the channel feed:

```bash
python3 scripts/generate_firmware_manifest.py \
  --version v1.2.3 \
  --channel stable \
  --build 86b48f5 \
  --release-url "https://github.com/OWNER/REPO/releases/tag/v1.2.3" \
  --published-at "2026-03-09T23:59:00Z" \
  --release-notes "Validation-scene release." \
  --web-root webapp/firmware \
  --bootloader build/bootloader/bootloader.bin \
  --partition-table build/partition_table/partition-table.bin \
  --app-binary build/hotas_ble.bin
```

Outputs:

- `webapp/firmware/releases/<version>/bootloader.bin`
- `webapp/firmware/releases/<version>/partition-table.bin`
- `webapp/firmware/releases/<version>/hotas-bridge.bin`
- `webapp/firmware/releases/<version>/manifest.json`
- `webapp/firmware/releases/<version>/SHA256SUMS.txt`
- `webapp/firmware/<channel>/latest.json`

## GitHub Actions workflow

`.github/workflows/release-firmware.yml` does the following:

1. installs ESP-IDF
2. builds the firmware
3. generates the web-flashing manifests
4. stages both GitHub Release assets and static-host files
5. uploads packaged artifacts
6. publishes release assets when triggered by a tag

## Static hosting

The workflow produces `dist/web-root`, which can be deployed to a static host.

Suggested structure:

```text
firmware/
  stable/latest.json
  beta/latest.json
  releases/v1.2.3/manifest.json
  releases/v1.2.3/bootloader.bin
  releases/v1.2.3/partition-table.bin
  releases/v1.2.3/hotas-bridge.bin
```

## End-user flashing flow

1. User opens the site in desktop Chrome or Edge.
2. User navigates to the firmware flasher.
3. User plugs in the ESP32-S3 over USB.
4. User selects the published stable manifest.
5. User flashes the firmware directly from the browser.

Mobile flashing is intentionally treated as best-effort. Desktop remains the primary supported path.
