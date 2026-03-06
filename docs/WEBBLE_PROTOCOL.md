# WebBLE Config Protocol (CONFIG Mode)

This document describes the **custom BLE GATT service** used for configuring the USB→BLE bridge from a browser (Web Bluetooth).

## Modes

The firmware has two boot modes to avoid BLE “central conflict” scenarios:

- **RUN mode**
  - Advertises and behaves as a **BLE HID Gamepad** (HOGP)
  - The WebBLE config service is **not advertised / not registered**

- **CONFIG mode**
  - Advertises a **custom GATT Config Service** for a browser to connect with Web Bluetooth
  - **HID gamepad output is paused/disabled**
  - USB host + HID decoding remain active so the browser can inspect devices and stream samples

Mode is persisted in NVS and changed via commands (`reboot_to_run`, `reboot_to_config`).

## Service and Characteristics

### Service UUID

- **Config Service UUID:** `6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a1`

### Characteristics

All characteristic UUIDs share the same base UUID and differ only by the last byte:

| Name | Properties | UUID |
|---|---|---|
| **CMD** | Write / Write Without Response | `6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a2` |
| **EVT** | Notify | `6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a3` |
| **STREAM** | Notify | `6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a4` |
| **CFG** | Read / Write | `6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a5` |

## CMD (Write): JSON commands

`CMD` accepts UTF‑8 JSON objects.

Recommended fields:

- `cmd` (string): command name
- `rid` (number, optional): request id echoed in the response
- additional parameters per command

### Supported commands (skeleton)

- `get_devices`
- `get_descriptor` (requires `device_id`)
- `start_stream`
- `stop_stream`
- `get_config`
- `set_config` (requires `config` object/value)
- `save_profile` (stub)
- `reboot_to_run`
- `reboot_to_config`

Example:

```json
{ "rid": 1, "cmd": "get_devices" }
```

## EVT (Notify): responses and async events

EVT is notification-only. Subscribe to EVT before sending commands.

### Chunking / framing

To remain compatible with **very small ATT MTUs** (e.g., MTU=23 → 20 bytes payload), EVT notifications use a small binary chunk header.

Each EVT notification payload is:

```text
u8  version      (1)
u8  type         (1=json, 2=descriptor, 3=config)
u16 msg_id
u16 offset
u16 total_len
u8  payload[]    (fragment)
```

- `msg_id` groups fragments belonging to the same message
- `offset` is the byte offset of `payload` within the full message
- `total_len` is the total byte length of the full message

`type=1` payload is **UTF‑8 JSON bytes**.

`type=2` payload is **raw HID report descriptor bytes** (returned by `get_descriptor`).

### Response JSON shape

Responses use a simple envelope:

```json
{
  "evt": "resp",
  "rid": 1,
  "cmd": "get_devices",
  "devices": [ ... ]
}
```

## STREAM (Notify): real-time element/value stream

STREAM is notification-only. Subscribe to STREAM and then send `start_stream`.

### Payload format

STREAM notifications are **binary** (to reliably hit 60Hz even at MTU=23):

```text
u8  version   (1)
u8  flags     (reserved)
u32 device_id
u32 element_id
i32 raw
i16 norm_q15  (norm_m1_1 * 32767)
```

- `device_id` matches the values returned by `get_devices`
- `element_id` is the stable 32-bit element id computed by the descriptor parser
- `raw` is the decoded integer value for that element
- `norm_q15` is the element’s normalized value in Q15 form (-32767..32767)

### Streaming behavior

Current skeleton behavior:

- Runs at **60Hz** (one notification per tick)
- Cycles through active devices and chooses the next **axis/hat** element to emit

## CFG (Read/Write): profile/config JSON transfer

`CFG` provides a raw JSON byte buffer that can be:

- **Read** (supports read offset via GATT “read blob”)
- **Written** sequentially (supports offset=0 reset; sequential append)

This is currently **in-memory only** and used by `get_config` / `set_config`.

## Notes / current limitations

- This is an initial protocol skeleton intended to enable early WebBLE bring-up.
- Persistent profile storage (`save_profile`) is a stub.
- The stream currently emits one element sample per 60Hz tick (not a full snapshot).
