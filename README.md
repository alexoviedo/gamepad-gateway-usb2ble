# HOTAS USB → BLE Gamepad Bridge <br>(ESP32-S3 / ESP-IDF)</br>

This repository contains an ESP-IDF firmware project for **ESP32-S3** that:

1. Acts as a **USB Host (OTG host mode)** for USB HID controllers (joystick / throttle / pedals *as generic HID devices*).
2. Parses each device’s **HID report descriptor** to discover supported axes / hat / buttons.
3. Decodes incoming **raw HID input reports** into a normalized `GamepadState`.
4. Publishes the merged state as a **BLE HID Gamepad (HOGP)** using **NimBLE**.
---

## Features


* **ESP32-S3 USB Host** initialization using the ESP-IDF USB Host stack (`usb_host_install`) and the internal USB PHY in **host mode**.
* **HID Host class driver** (`espressif/usb_host_hid`) for receiving raw HID input reports.
* **HID report descriptor parsing** to map supported fields into a flat list of bitfields (`HidMappedField`).
* **Input report decoding** into a normalized state:

  * Axes are normalized to **`int16_t` in −32767..32767**
  * Hat switch is normalized to **0=center, 1..8 directions**
  * Buttons are packed into a **32-bit bitmask** (`buttons` = Button1..Button32)
* **Multi-device merge** for up to **8** simultaneously connected HID devices:

  * Buttons are OR’d together
  * First non-center hat wins
  * For each axis, the value with the largest absolute deflection wins
* **BLE HID Gamepad (HOGP)** implemented with NimBLE:

  * GAP device name: **`HOTAS_BRIDGE`**
  * Advertises HID (0x1812) + Battery (0x180F) UUIDs
  * GATT database includes **Device Information (0x180A)**, **Battery (0x180F)**, **HID (0x1812)**
* **Report de-duplication**: identical consecutive input reports are not re-notified.
* **Notification gating**: HID input notifications are only sent after the central subscribes (CCCD enabled).
* **Bonding enabled** (keys persisted via NVS).

---

## Hardware Requirements

### ESP32 board requirements

The firmware is configured for:

* **Target:** `esp32s3` (see `sdkconfig.defaults`)
* **USB:** ESP32-S3 with **native USB OTG** routed to a USB connector that can operate in **host mode**
* **Flash / PSRAM (current defaults):**

  * `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`
  * PSRAM enabled with octal mode (`CONFIG_ESP32S3_SPIRAM_SUPPORT=y`, `CONFIG_SPIRAM_MODE_OCT=y`)

### USB Host / peripherals

The codebase does **not** hardcode any vendor/product IDs. Instead, it supports USB HID devices whose **report descriptors** contain specific *usages* (see “Supported HID inputs” below).

The HID parser / decoder currently maps these usages:

* **Usage Page 0x01 (Generic Desktop):**

  * X (0x30), Y (0x31), Z (0x32)
  * Rx (0x33), Ry (0x34), Rz (0x35)
  * Slider (0x36)
  * Hat switch (0x39)
* **Usage Page 0x02 (Simulation Controls):**

  * Rudder (0xBA)
  * Throttle (0xBB)
  * Toe Brake (0xBF)
* **Usage Page 0x09 (Button):**

  * Button 1..32 (bits 0..31)

Any HID fields outside of the above usages are currently ignored by the descriptor parser and decoder.

---

## Wiring & Pinouts

This firmware uses the ESP32-S3 **internal USB PHY** in host mode:

* `usb_phy_config_t.target = USB_PHY_TARGET_INT`
* Comment in `usb_host_manager.cpp`: “Pins **19 and 20** usually” (board-dependent routing)

| Function                      | ESP32-S3 Signal / Notes                                                                                    | Where defined               |
| ----------------------------- | ---------------------------------------------------------------------------------------------------------- | --------------------------- |
| USB (internal PHY, host mode) | Uses internal PHY routing (commonly exposed as **GPIO19/GPIO20** on many ESP32-S3 devkits; board-specific) | `main/usb_host_manager.cpp` |
| Console / logging             | Uses **UART console** (USB CDC console is disabled in defaults to avoid interfering with USB Host)         | `sdkconfig.defaults`        |


## Software Dependencies

### Framework

* **ESP-IDF:** `>= 5.5.0`

  * `dependencies.lock` pins the development environment to **ESP-IDF 5.5.3**.

### Managed components (ESP Component Manager)

Declared in `idf_component.yml`:

* `espressif/usb` (`>= 1.2.0`)

  * `dependencies.lock` pins it to **1.2.0**
* `espressif/usb_host_hid` (`>= 1.0.0`)

  * `dependencies.lock` pins it to **1.1.0**

### BLE stack

* **NimBLE** (part of ESP-IDF)

  * Enabled in defaults (`CONFIG_BT_ENABLED=y`, `CONFIG_BT_NIMBLE_ENABLED=y`)
  * Bonding persistence enabled (`CONFIG_BT_NIMBLE_NVS_PERSIST=y`)

---

## Installation & Building

### 1) Install ESP-IDF

Install ESP-IDF using Espressif’s official installer / tooling for your OS, ensuring the installed version satisfies:

* **ESP-IDF >= 5.5.0** (project requirement)
* (Recommended) **ESP-IDF 5.5.3** to match `dependencies.lock`

### 2) Clone the repo

```bash
git clone https://github.com/alexoviedo/gamepad-gateway-usb2ble.git
cd HOTAS_BRIDGE_MERGED
```

### 3) Activate the ESP-IDF environment

You must have `idf.py` on your PATH:

* **macOS / Linux (typical):**

  ```bash
  . "$IDF_PATH/export.sh"
  ```
* **Windows:** Use the ESP-IDF PowerShell / Command Prompt provided by the ESP-IDF installer.

### 4) Configure target + download managed components

```bash
idf.py set-target esp32s3
idf.py reconfigure
```

`idf.py reconfigure` will trigger the ESP Component Manager to download managed dependencies.

### 5) Build

```bash
idf.py build
```

### 6) Flash + monitor

```bash
idf.py -p <PORT> flash monitor
```

Examples for `<PORT>`:

* macOS: `/dev/cu.usbmodem*` or `/dev/cu.SLAB_USBtoUART*`
* Linux: `/dev/ttyUSB*` or `/dev/ttyACM*`
* Windows: `COM3` (etc.)

### Convenience scripts

These scripts assume your ESP-IDF environment is already activated:

```bash
./scripts/build.sh
./scripts/flash.sh <PORT>
./scripts/clean.sh
```

---

## Usage & Pairing

### 1) Connect USB HID peripherals

1. Connect your USB HID controller(s) to the ESP32-S3 **USB Host (OTG) port**.
2. Power the ESP32-S3 board.

On device connect, the HID manager:

* Waits briefly (`200 ms`) after enumeration (for better hub compatibility)
* Fetches and parses the HID report descriptor
* Starts the HID device to receive input reports
* Logs the mapped fields and a role classification (stick / throttle / pedals / unknown)

### 2) Pair the BLE gamepad

1. On your BLE host device, scan for a BLE peripheral named:

   * **`HOTAS_BRIDGE`**
2. Pair / connect to it.

Notes from current BLE implementation:

* The device advertises HID (0x1812) and Battery (0x180F) service UUIDs.
* HID input notifications are only sent after the central enables notifications for the input report characteristic.
* Bonding is enabled; pairing information is stored in NVS.

### 3) What the BLE host receives

The outgoing HID report is defined by `kHidReportMap` in `main/ble_gamepad.cpp`:

* **32 buttons** (bitmask)
* **8 axes** (`uint16_t`, logical range 0..32767): X, Y, Z, Rx, Ry, Rz, Slider, Slider
* **Hat switch** (8-bit with null state): 0=center, 1..8 directions

Internally, the firmware uses a signed, centered representation (−32767..32767) and converts it to the outgoing unsigned axis format before notifying.

---

## Customizing the USB → BLE mapping

The intended “translation layer” lives in:

* `main/main.cpp` → `translate_usb_to_ble(const GamepadState *in, GamepadState *out)`

By default, it performs an **identity mapping** (no remaps / deadzones / inversions). The file contains commented examples for:

* Axis inversion
* Deadzone application
* Axis swapping

---

## Limitations (current behavior)

* Only the HID usages listed in “Hardware Requirements → USB Host / peripherals” are parsed/decoded.
* Buttons are limited to **32** (`uint32_t buttons`).
* Input report buffer in the HID callback is **128 bytes**.
* Up to **8** HID devices can be tracked at once (`MAX_DEVICES`).
* The `debug_portal.*` module exists in the repository but is **not compiled** (not included in `main/CMakeLists.txt`) and is **not started** by `app_main()`.

---

## Debugging the Rudder Issue

This repo includes **Phase 0** instrumentation designed to answer one question:

> Is rudder being decoded correctly per-device but lost during merge/mapping (A),
> or decoded inconsistently at the input extraction layer (B)?

### 1) Enable verbose HID instrumentation

The instrumentation is gated behind a **compile-time** flag so release builds stay clean.

Enable it via menuconfig:

```bash
idf.py menuconfig
```

Then:

* **Gamepad Gateway → Verbose HID debug instrumentation** → **Enable**

Alternatively (no menuconfig), you can inject a compiler define:

```bash
idf.py -DCMAKE_C_FLAGS="-DGPG_VERBOSE_HID_DEBUG=1" -DCMAKE_CXX_FLAGS="-DGPG_VERBOSE_HID_DEBUG=1" build
```

(If you already have a build directory, do a clean build so flags take effect: `idf.py fullclean`.)

### 2) Build / flash / monitor

```bash
idf.py -p <PORT> flash monitor
```

### 3) What you should see (with stick + throttle + pedals attached)

#### A) Descriptor snapshot (printed once per device connect)

On each HID device connection, you should see a block like:

* `Descriptor snapshot DEV[slot] addr=... VID=... PID=... iface=... role=... fields=...`
* Followed by `IN elem:` lines (usage_page, usage, report_id, bit offsets/sizes, logical min/max)

This confirms how the firmware **thinks** the device’s input report is structured.

#### B) Per-device decoded state (BEFORE merging)

While moving controls, you should see periodic lines like:

* `DEV[slot] ... rpt_id=.. len=.. hz~..  x=.. y=.. z=.. rx=.. ry=.. rz=.. s1=.. s2=.. hat=.. buttons=..`

With the latest Phase 0 instrumentation, you will also see (rate-limited with the device dump):

* `DEV[slot] raw[16]: .. .. .. .. ...` (first 16 bytes of the most recent raw report)
* `DEV[slot] Zsrc ... bytes[..]=.. .. raw=0x....` (the raw bytes backing the Z/Rudder element)
* `DEV[slot] NOTE: Z ... appears stuck at max; rudder may be on Slider` (printed once if Z looks invalid/stuck)
* `DEV[slot] RUDDER_CANDIDATE: Slider (0x01/0x36) ...` (printed when the slider raw value changes)

This is the critical proof for **(A) vs (B)**.

#### C) Merge diagnostics (winner per axis + collisions)

You should also see periodic lines like:

* `MERGE winners: X=dev? Y=dev? Z=dev? Rx=dev? ...`
* `MERGE out: x=.. y=.. z=.. ...`
* Optional warnings: `MERGE collision: Z has N active contributors`

This tells you exactly **which device “won” each axis** in the final merged output.

### 4) How to interpret results

**If pedals’ device line shows Z moving smoothly** (rudder deflection changes), but:

* `MERGE out: z=...` does **not** track it, or
* `MERGE winners: Z=devX` is consistently **not** the pedals device,

…then the bug is likely **(A) merge/mapping**.

**If pedals’ device line shows Z updating inconsistently / intermittently**, or you see:

* `AXIS COLLISION Z overwritten (....)` warnings

…then the bug is likely **(B) input extraction / descriptor mapping**, especially cases where
multiple usages map into the same internal axis (e.g., Generic Desktop Z and Simulation Rudder both
writing to `state.z`).

