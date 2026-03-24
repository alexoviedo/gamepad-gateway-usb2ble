# WebApp: Validation Scene

## Purpose
Document the separate validation-oriented WebBLE client implemented by `webapp/validation.js` and highlight its contract overlap and divergence from the main config console.

Related documents:
- [CONFIG_CONSOLE.md](CONFIG_CONSOLE.md)
- [../interfaces/BLE_CONTRACTS.md](../interfaces/BLE_CONTRACTS.md)
- [../../AUDIT_REPORT.md](../../AUDIT_REPORT.md)

## Dependencies
- `webapp/validation.html`
- `webapp/validation.js`
- firmware CONFIG service docs in [../firmware/CONFIG_SERVICE.md](../firmware/CONFIG_SERVICE.md)

## Data Structures/Interfaces
### Local client state
`BridgeClient` tracks:
- `device`, `server`, `service`
- `characteristics.cmd`, `.evt`, `.stream`
- `chunkAssembler`, `pendingJson`, `requestId`
- `streamActive`, `currentConfig`, `savedConfigString`, `pendingChanges`
- `latestSample`, `latestSampleByKey`, sample timestamps

### Requested BLE characteristics
The validation scene requests:
- `CMD`
- `EVT`
- `STREAM`

It does **not** request `CFG` on current `main`.

### Parsing model
- EVT parsing uses the same 8-byte chunk header concept as the main console.
- STREAM parsing uses the same 16-byte binary sample layout.
- `getConfig()` expects inline config data only.

### Visualization layer
- Converts mapped outputs into visual widgets and a Three.js scene.
- Recomputes smoothed outputs locally from streamed source samples and the loaded config.

## Expected State Changes
- Connect loads config, then starts streaming immediately.
- Refresh config reissues `get_config`.
- Save button calls `save_profile`.
- Reboot button issues `reboot_to_run`.

## Known Limitations
- This client is not contract-symmetric with the main console.
- Flat 8-second command timeout is weaker than the main console timeout policy.
- No `CFG` characteristic support means future or hardened config transport changes can break this page first.
