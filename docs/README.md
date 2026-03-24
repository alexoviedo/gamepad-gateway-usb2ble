# Docs Index

## Purpose
Top-level index for the AI-optimized project knowledge base. This directory is organized around symmetrical documentation of firmware, webapp, interfaces, and CI/CD.

Primary entry points:
- [../../ARCHITECTURE.md](../ARCHITECTURE.md)
- [../../AUDIT_REPORT.md](../AUDIT_REPORT.md)
- [architecture/SYSTEM_OVERVIEW.md](architecture/SYSTEM_OVERVIEW.md)
- [interfaces/BLE_CONTRACTS.md](interfaces/BLE_CONTRACTS.md)
- [interfaces/CONFIG_SCHEMA.md](interfaces/CONFIG_SCHEMA.md)

## Dependencies
- Firmware sources in `main/`
- Web application sources in `webapp/`
- Workflow files in `.github/workflows/`

## Data Structures/Interfaces
### Hierarchy
- `architecture/`
  - [SYSTEM_OVERVIEW.md](architecture/SYSTEM_OVERVIEW.md)
  - [CI_CD.md](architecture/CI_CD.md)
- `firmware/`
  - [CONFIG_SERVICE.md](firmware/CONFIG_SERVICE.md)
  - [HID_PIPELINE.md](firmware/HID_PIPELINE.md)
  - [RUN_MODE_HID.md](firmware/RUN_MODE_HID.md)
- `webapp/`
  - [CONFIG_CONSOLE.md](webapp/CONFIG_CONSOLE.md)
  - [VALIDATION_SCENE.md](webapp/VALIDATION_SCENE.md)
- `interfaces/`
  - [BLE_CONTRACTS.md](interfaces/BLE_CONTRACTS.md)
  - [CONFIG_SCHEMA.md](interfaces/CONFIG_SCHEMA.md)
  - [HID_TO_GAMEPAD_MAPPING.md](interfaces/HID_TO_GAMEPAD_MAPPING.md)

## Expected State Changes
- Documentation should be regenerated or reviewed whenever a BLE characteristic, JSON schema, HID report layout, web parser, or workflow semantic changes.

## Known Limitations
- The codebase currently contains duplicated client logic and comment drift. See [../../AUDIT_REPORT.md](../AUDIT_REPORT.md).
