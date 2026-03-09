# HOTAS Bridge WebBLE Web App

A static, client-side Web Bluetooth console for the ESP32 **Config Service**.

## What it does

- Connects to the ESP32 in **CONFIG** mode using the Config Service UUID
- Discovers the Config Service characteristics
- Subscribes to **EVT** and **STREAM** notifications
- Requests and displays the attached device list
- Requests and displays raw HID report descriptors
- Shows live telemetry from the `STREAM` characteristic
- Runs a guided mapping wizard that detects the most active control over a short sample window
- Applies confirmed mappings immediately in firmware via `set_config`
- Includes a live response-tuning editor for deadzone, outer clamp, EMA smoothing, and cubic-bezier response curves
- Shows **pending** vs **saved** state in the UI
- Handles disconnects with a user-friendly reconnect flow and automatic retry attempts

## Files

- `index.html` — app shell
- `styles.css` — polished dark-mode UI
- `app.js` — WebBLE logic, chunk reassembly, telemetry decoding, mapping wizard, and response tuning UI

## Requirements

- Chrome or Edge with Web Bluetooth support
- A secure context such as `http://localhost`
- ESP32 running in **CONFIG** mode and advertising the Config Service UUID

## Run locally

From the repo root:

```bash
cd webapp
python3 -m http.server 8080
```

Then open:

```text
http://localhost:8080
```

## Usage

1. Put the ESP32 into **CONFIG** mode.
2. Open the page in Chrome.
3. Click **Connect**.
4. Pick the ESP32 from the Bluetooth picker.
5. The app automatically fetches devices, descriptors, and the current config after connecting.
6. Click **Start Detection** in the mapping wizard.
7. Move only the control you want to map.
8. Confirm the proposed mapping.
9. Select the mapped output in **Response Tuning** to adjust deadzone, curve, and smoothing in real time.
10. Click **Save Profile** to persist the profile to NVS.

## Notes

- The app uses `requestDevice()` with both the Config Service UUID and a `HOTAS` name prefix fallback.
- The descriptor panel shows the raw HID report descriptor as hex/ASCII.
- The telemetry panel shows the element that changed, the raw value, and the normalized Q15 value from the current firmware protocol.
- The response-tuning panel previews the selected mapped output using live STREAM samples and applies tuning changes immediately through `set_config`.

## Current limitations

- No follow-me mapping flow yet
- Hat outputs do not use the curve/deadzone editor


## Validation scene

A dedicated browser-based calibration/test page is available at:

```text
http://localhost:8080/validation.html
```

What it includes:

- a cleaner centered technical bench with clearer stick, throttle, and pedal composition
- large primary validation widgets for Stick XY, Pedals, and Throttle
- compact secondary widgets for twist / secondary yaw, hat switch, and aux slider
- compact mapped-output summary cards across the full page width
- save-to-device action
- reboot-to-run action

Recommended flow:

1. Put the ESP32 in **CONFIG** mode.
2. Open `validation.html`.
3. Click **Connect**.
4. Let the page read the current config and start the stream.
5. Move the real controls and confirm the virtual HOTAS/pedals react smoothly.
6. Click **Save to Device** when satisfied.
7. Click **Reboot to Run Mode** to return the bridge to HID gamepad mode.
