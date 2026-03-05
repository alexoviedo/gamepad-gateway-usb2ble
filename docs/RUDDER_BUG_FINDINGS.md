# Rudder Bug Findings (Phase 0 Instrumentation)

This document captures **what the Phase 0 instrumentation is designed to prove**, and the
current conclusion based on **real device logs**.

---

## What we’re trying to prove

The goal is to determine whether rudder is:

* **(A) Decoded correctly per-device but lost during merge/mapping**, or
* **(B) Decoded inconsistently at the input extraction layer** (descriptor mapping / report decode)

The instrumentation prints:

1) **Per-device decoded state** (before merge)
2) **Merge winners** (which device contributes each final axis)
3) **Descriptor snapshots** (parsed input elements)
4) **Axis collision warnings** when multiple usages write the same internal axis

---

## Preliminary hypothesis from code inspection

### Highest-likelihood failure mode: decode-layer axis collisions (B)

In `main/input_decoder.cpp`, multiple HID usages can legally refer to “rudder-ish” motion:

* **Generic Desktop Z**: usage page **0x01**, usage **0x32** → currently maps to `state.z`
* **Simulation Rudder**: usage page **0x02**, usage **0xBA** → currently also maps to `state.z`

If a device’s descriptor (or multiple report IDs) presents both of these in a way that causes
both to be parsed and decoded, the later write can **overwrite** the earlier write within a
single decoded report.

Phase 0 adds explicit logging for this case:

* `AXIS COLLISION Z overwritten (....)`

If you see these collision warnings while moving pedals, and the per-device `z=` value is
intermittent, that’s strong evidence for **(B)**.

### Secondary failure mode: merge winner not selecting pedals (A)

The merge policy in `main/input_decoder.cpp` chooses, for each axis, the device with the
largest **absolute deflection** from center.

If some other device reports a non-trivial Z deflection/noise (or maps a different physical
control into `state.z`), it can “win” over the pedals.

Phase 0 prints:

* `MERGE winners: ... Z=devN ...`

If per-device pedals `z=` moves smoothly but merged `z=` does not track, that’s evidence
for **(A)**.

---

## How to write the final conclusion (after you run)

After enabling **Verbose HID debug instrumentation** (see README), capture logs for:

1) Moving rudder left/right through full travel
2) Pressing toe brakes independently
3) Keeping stick/throttle still while moving pedals

Then answer:

* Does **the pedals device line** show `z=` moving smoothly?
* Does `MERGE winners` show `Z=dev(pedals)` while moving rudder?
* Do you see any `AXIS COLLISION Z overwritten` warnings?

### Template conclusion

Fill in once you have logs:

* **Observed:** (per-device pedals `z=` …)
* **Observed:** (merge winner for Z …)
* **Observed:** (any axis collision warnings …)

**Conclusion:** Rudder disappears at **(A) merge/mapping** OR **(B) extraction/decoding**.

---

## Conclusion from logs (Mar 4, 2026)

### Devices observed

Two HID devices enumerated:

* **DEV[0]** addr=2 VID=0x044F PID=0xB687, report_id=1, **has a Z element** in descriptor snapshot:
  * `usage_page=0x0001 (Generic Desktop), usage=0x0032 (Z)` with `bit_ofs=112`, `bit_sz=16`, logical `[0..65535]`
* **DEV[1]** addr=3 VID=0x044F PID=0xB10A, report_id=0, **does not show any Z element** in the snapshot
  (it shows X/Y + Rz + Slider + Hat + Buttons).

Given the RJ12 chain (pedals -> throttle -> USB hub), it is expected that the pedals’ rudder/toe-brakes
appear on the *throttle-side* HID device. In this run, only **DEV[0]** exposes a `Z` usage.

### What happened when pedals were moved

Per-device decoded values:

* **DEV[0]** `z=32767` (saturated max) and did not show observable change during the captured window.
* **DEV[1]** `z=0` (no Z field present in the descriptor snapshot; this value is effectively “not provided”).

Merged/output behavior:

* Merge consistently reports **`Z=dev0`** as the winner.
* Output `z` stays **`32767`** (max) consistently.

Notably:

* No `AXIS COLLISION Z overwritten ...` messages were observed, which suggests the decoded `state.z`
  is not being overwritten by multiple usages within a single report (at least not in this snapshot).

### Root placement: where the rudder disappears

This run supports **(B) decoded incorrectly / missing at the input extraction layer**, not (A).

Reason: the merged output is doing exactly what it is told — it always selects DEV[0] for Z because DEV[0]
reports a saturated Z value, and DEV[1] does not present a Z field.

So the failure is upstream of merge:

* Either DEV[0] is **not actually carrying rudder changes** (pedal axes might be on a different interface
  or report ID than the one currently opened), **or**
* The Z element is present but the current decode is reading **the wrong bits / wrong report payload alignment**,
  resulting in a constant `0xFFFF` (normalized to +32767).

### Next Phase 0 instrumentation (optional, still no fixes)

If you want to conclusively distinguish “device never sends rudder” vs “we read the wrong bits”, the
most useful additional Phase 0 dump is:

* On each input report for DEV[0], print the **first ~16 bytes of the raw payload** (hex) when `report_id=1`,
  along with the 2 bytes that correspond to `bit_ofs=112` (i.e., bytes 14–15 of the payload when `bit_ofs=112`).

If those bytes change while moving the rudder, the descriptor/offset mapping is correct and the issue is in
normalization/mapping. If they *don’t* change, the pedals’ rudder data is not present in that interface/report.


## Latest confirmation (rudder-only run)

Using **only throttle + pedals** connected (pedals chained via **RJ12** into the throttle, then throttle into the USB hub), and moving **only the rudder axis**:

- The merged output shows **`slider1` changing smoothly** while `z` and `rz` remain essentially constant.
- The verbose dump shows **Desktop Z (0x01/0x32)** repeatedly reading **0xFFFF** (max) — a strong sign that this device **does not use Z for rudder**.
- With the added instrumentation in this build, you should now see explicit one-liners like:
  - `DEV[n] NOTE: Z (0x01/0x32) raw=0xFFFF appears stuck at max; rudder may be on Slider`
  - `DEV[n] RUDDER_CANDIDATE: Slider (0x01/0x36) ... norm(slider1)=...`

### Implication

The pedals' rudder axis is being decoded consistently, but it is landing in **`slider1`** (usage **0x01/0x36**) rather than the axis your PC calibration UI expects as “rudder” (often **Rz** or **Z**).

That means the missing-rudder symptom is **not** a flaky USB poll issue; it is primarily a **mapping issue**: we need to map the device's **Slider** field into the **output axis** that the BLE HID gamepad advertises as rudder (and/or verify which output axis Windows/Quest uses for rudder).
