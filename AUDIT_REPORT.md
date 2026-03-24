# Audit Report

## Purpose
Enumerate interface discrepancies, broken or fragile contracts, stale comments, and documentation drift discovered while auditing the current `main` branch.

Related documents:
- [ARCHITECTURE.md](ARCHITECTURE.md)
- [docs/interfaces/BLE_CONTRACTS.md](docs/interfaces/BLE_CONTRACTS.md)
- [docs/interfaces/CONFIG_SCHEMA.md](docs/interfaces/CONFIG_SCHEMA.md)
- [docs/webapp/CONFIG_CONSOLE.md](docs/webapp/CONFIG_CONSOLE.md)
- [docs/webapp/VALIDATION_SCENE.md](docs/webapp/VALIDATION_SCENE.md)

## Dependencies
Audit inputs came from current `main` firmware, webapp, and workflow files.

## Data Structures/Interfaces
### Findings
#### A-001 — CONFIG transport mismatch between web clients
- **Severity:** High
- `webapp/app.js` requests `CFG`; `webapp/validation.js` does not.
- Any firmware shift toward metadata + `CFG` config transport will be symmetric with the main console only if the validation client is also updated.

#### A-002 — `validation.js` assumes `get_config` returns inline config only
- **Severity:** High
- `validation.js` only accepts `response.config` or `response.config_json`.
- No fallback path exists for `CFG`-based config retrieval.

#### A-003 — `ble_config_service.h` comment drift on notify pacing
- **Severity:** Medium
- Header comment says `ble_config_service_on_notify_tx()` paces EVT responses.
- Implementation currently leaves that function empty and paces in `notify_evt_chunked()`.

#### A-004 — Mapping schema version drift in comments
- **Severity:** Medium
- `mapping_engine.h` example still shows `"version": 1`.
- `mapping_engine_profile_to_json()` emits `"version": 2`.

#### A-005 — `replace_all` is parser-supported but under-documented
- **Severity:** Medium
- Firmware parser supports `replace_all: true`.
- Header-level schema comment does not describe it, though web flows depend on it.

#### A-006 — Duplicated BLE client logic across `app.js` and `validation.js`
- **Severity:** High
- Separate command queues, chunk assemblers, config parsing, timeouts, and reconnect logic.
- Fixes can land in one client but not the other.

#### A-007 — Command timeout asymmetry between web clients
- **Severity:** Medium
- `app.js`: command-specific timeouts up to 30s.
- `validation.js`: flat 8s timeout.

#### A-008 — `app.js` fetches `CFG` but does not use it on current `main`
- **Severity:** Medium
- The characteristic is requested but `getConfig()` still expects inline config.

#### A-009 — `renderMappingsSummary()` is a no-op compatibility shim
- **Severity:** Low
- Indicates incomplete cleanup and a stale implicit interface.

#### A-010 — `main/main.cpp` comments overstate translation-layer importance
- **Severity:** Medium
- `translate_usb_to_ble()` is identity-only.
- Real mapping happens in `mapping_engine_compute()`.

#### A-011 — `STREAM` sample is an undocumented binary contract in code comments alone
- **Severity:** Medium
- The 16-byte layout is implemented consistently but historically under-documented.

#### A-012 — CONFIG frame-type legend is stale
- **Severity:** Low
- Comments mention frame type `3=config`, but current `main` actively uses type `1` JSON and type `2` descriptor payloads.

#### A-013 — Validation scene omits the `CFG` UUID constant entirely
- **Severity:** Medium
- It cannot fully represent the CONFIG service surface.

## Expected State Changes
Healthy future state:
- one canonical WebBLE client reused by both `app.js` and `validation.js`
- generated or centrally owned contract/schema docs
- implementation comments aligned with code

## Known Limitations
- This report intentionally does **not** change source code.
- Findings are based on audited current `main`.
