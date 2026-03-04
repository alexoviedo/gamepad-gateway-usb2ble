# USB HID → BLE Gamepad Bridge (ESP32‑S3)

This ESP‑IDF project turns an **ESP32‑S3** into a **USB‑OTG HID host** (joysticks/throttles/pedals) and publishes the merged state as a **BLE gamepad**. The most mature part of the codebase is the **USB HID read + report‑descriptor parse + input decode** pipeline — that functionality is kept intact.

## What it does

* Enumerates USB devices via the ESP‑IDF USB Host stack.
* Uses the HID Host class driver to receive **raw input reports**.
* Parses the HID report descriptor to discover fields (axes, hats, buttons).
* Decodes input reports into a normalized `GamepadState`.
* Merges multiple devices (stick + throttle + pedals) into one state.
* Advertises over BLE and notifies a HID‑like report.
* Starts a small **debug portal** (SoftAP + HTTP) that shows the merged state.

## Hardware

* **ESP32‑S3** with native USB‑OTG (example: ESP32‑S3‑WROOM‑1‑N16R8 / similar devkit)
* **Powered USB hub** (strongly recommended)
* USB HID device(s): joystick, throttle, pedals, etc.

> Why powered hub? Many flight peripherals draw more current than the ESP32‑S3 (or a headset) should supply.

## Quick start (ESP‑IDF)

### 1) Install ESP‑IDF (5.1+)

Install ESP‑IDF using Espressif’s recommended method for your OS.

### 2) Get the repo + fetch managed components

```bash
git clone <this-repo>
cd USBtoBLE

# This project uses ESP-IDF managed components (downloads into managed_components/)
idf.py reconfigure
```

### 3) Build

```bash
idf.py set-target esp32s3
idf.py build
```

### 4) Flash + monitor

```bash
# Replace with your serial port (examples below)
idf.py -p /dev/ttyUSB0 flash monitor
```

Serial port examples:
* macOS: `/dev/cu.usbmodem*` or `/dev/cu.SLAB_USBtoUART*`
* Linux: `/dev/ttyUSB*` or `/dev/ttyACM*`
* Windows: `COM3` (etc)

## One-liner helpers

Scripts assume `idf.py` is already on your PATH (ESP‑IDF environment activated):

```bash
./scripts/build.sh
./scripts/flash.sh /dev/ttyUSB0
```

## Debug portal

The firmware starts a SoftAP and web page that refreshes every second.

* Wi‑Fi SSID: **HOTAS-Debug**
* Open: **http://192.168.4.1/**
* Logs: **http://192.168.4.1/logs**

## Notes & troubleshooting

### USB enumeration / hub quirks

* Use a powered hub.
* Try a shorter USB cable.
* Some devices behind hubs need extra time after enumeration — this project waits briefly before requesting the report descriptor.

### “It builds but I don’t see devices”

* Confirm you are in **USB host mode** (OTG cable/hub, proper VBUS).
* Check serial logs for `USB Host initialized` and `HID Device Connected`.

### Configuration

Defaults live in `sdkconfig.defaults`. Customize via:

```bash
idf.py menuconfig
```

## Repo hygiene

* `managed_components/` is **not committed** (it’s downloaded by ESP‑IDF).
* `build/` and `sdkconfig` are generated.

---

If you want, the next step is to add a fully-compliant HID‑over‑GATT (HOGP) service definition for maximum compatibility with different BLE hosts.
