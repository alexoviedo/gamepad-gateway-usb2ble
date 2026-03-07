# HOTAS Bridge WebBLE Web App

A static, client-side Web Bluetooth console for the ESP32 **Config Service**.

## What it does

- Connects to the ESP32 in **CONFIG** mode using the Config Service UUID
- Discovers the Config Service characteristics
- Subscribes to **EVT** and **STREAM** notifications
- Requests and displays the attached device list
- Requests and displays raw HID report descriptors
- Shows live telemetry from the `STREAM` characteristic
- Handles disconnects with a user-friendly reconnect flow and automatic retry attempts

## Files

- `index.html` — app shell
- `styles.css` — polished dark-mode UI
- `app.js` — WebBLE logic, chunk reassembly, telemetry decoding

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
5. The app automatically fetches devices after connecting.
6. Click a device card, then **Load Descriptor** to inspect its HID descriptor.
7. Click **Start Stream** to view live telemetry.

## Notes

- The app uses `requestDevice({ filters: [{ services: [CONFIG_SERVICE_UUID] }] })`.
- If Chrome has an old cached Bluetooth permission that does not include the Config Service UUID, remove the saved device permission for the site and reconnect.
- The descriptor panel currently shows the raw HID report descriptor as hex/ASCII. No higher-level HID parsing UI is implemented yet.
- The telemetry panel shows the element that changed, the raw value, and the normalized Q15 value from the current firmware protocol.

## Current limitations

- No follow-me mapping UI yet
- No profile editing UI yet
- Descriptor rendering is raw hex rather than a parsed HID tree


## Discovery fallback

The device picker accepts either:
- the Config Service UUID, or
- a Bluetooth name starting with `HOTAS`

This makes Chrome discovery resilient if a firmware build is in CONFIG mode but the 128-bit service UUID is not exposed in advertisement data exactly as expected by the browser. The app still validates the connection by opening the Config Service UUID after selection.
