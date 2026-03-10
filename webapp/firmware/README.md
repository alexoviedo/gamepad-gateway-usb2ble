# Firmware feed root

This directory is populated by `scripts/generate_firmware_manifest.py` and the GitHub Actions release workflow.

Expected structure:

```text
firmware/
  stable/latest.json
  beta/latest.json
  releases/<version>/manifest.json
  releases/<version>/bootloader.bin
  releases/<version>/partition-table.bin
  releases/<version>/hotas-bridge.bin
```

Keep the directory checked in so the static host path is predictable, but expect the actual versioned release contents to be generated during packaging.
