# WebApp: Main Config Console

## Purpose
Document the behavior of the main WebBLE configuration console implemented by `webapp/index.html` + `webapp/app.js`.

Related documents:
- [../interfaces/BLE_CONTRACTS.md](../interfaces/BLE_CONTRACTS.md)
- [../interfaces/CONFIG_SCHEMA.md](../interfaces/CONFIG_SCHEMA.md)
- [VALIDATION_SCENE.md](VALIDATION_SCENE.md)
- [../../AUDIT_REPORT.md](../../AUDIT_REPORT.md)

## Dependencies
- `webapp/index.html`
- `webapp/app.js`
- `webapp/render-mappings-summary.js`
- firmware CONFIG service docs in [../firmware/CONFIG_SERVICE.md](../firmware/CONFIG_SERVICE.md)

## Data Structures/Interfaces
### Local client state
`HotasConfigClient` tracks:
- `device`, `server`, `service`
- `characteristics.cmd`, `.evt`, `.stream`, `.cfg`
- `pendingJson`, `pendingDescriptorBinary`
- `devices[]`, `descriptorCache`, `elementMetaByDevice`
- `currentConfig`, `savedConfigString`, `pendingChanges`
- `latestSample`, `latestSampleByKey`, sample history, reconnect state

### Requested BLE characteristics
The console requests and uses:
- `CMD`
- `EVT`
- `STREAM`
- `CFG` (retrieved but not actively used by current `main` logic)

### Command timing policy
- `get_descriptor`: 30s timeout
- `get_devices`: 15s timeout
- `get_config`: 20s timeout
- others: 8s timeout

### Binary parsing
#### EVT frame reassembly
- 8-byte frame header and a `ChunkAssembler`
- consumes type `1` JSON and type `2` descriptor payloads

#### STREAM sample parsing
- expects a 16-byte packet
- parses `version`, `flags`, `deviceId`, `elementId`, `raw`, `normQ15`

## Expected State Changes
- Connect selects device, connects GATT, subscribes EVT, and delays before use.
- `get_devices` refreshes active device cards.
- `get_elements` hydrates element metadata.
- `get_descriptor` stores descriptor bytes.
- `set_config` updates local config state and marks pending changes.
- `save_profile` clears pending changes.

## Known Limitations
- Current `main` retrieves `CFG` but does not use it in `getConfig()`.
- The console depends on duplicated logic that also exists in `validation.js` but is not shared.
- `renderMappingsSummary()` is a compatibility shim rather than a real render function.
