# WebBLE Protocol (Config Mode)

This project exposes a **custom BLE GATT service** in **CONFIG mode** so a Web Bluetooth (WebBLE) client can:

- connect
- enumerate HID devices and fetch their report descriptors
- start/stop a ~60Hz sample stream
- read/write a configuration blob
- optionally reboot into RUN mode

> In **RUN mode** the device advertises as a BLE HID gamepad and does **not** expose this custom service.

---

## Service UUID

**Config Service (primary):**

- UUID: `6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a1`
- Advertising name (CONFIG mode): typically `HOTAS_CFG`

---

## Characteristics

All characteristics below share the same base UUID with the last byte incremented.

| Name | UUID | Properties | Direction | Purpose |
|---|---|---:|---|---|
| CMD | `6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a2` | Write / Write Without Response | client → device | Send JSON commands (UTF‑8 text) |
| EVT | `6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a3` | Notify | device → client | Chunked event/response channel (JSON + binary payloads) |
| STREAM | `6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a4` | Notify | device → client | Fixed-size binary samples (~60Hz when enabled) |
| CFG | `6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a5` | Read / Write | bidirectional | Raw configuration blob (JSON text) |

---

## CMD: JSON commands

The client writes a single JSON object to `CMD` (UTF‑8). The device emits a JSON response on `EVT` (type=1, chunked).

Common request shape:

```json
{ "rid": 1, "cmd": "get_devices" }
```

- `rid` is optional but recommended (request id). It is echoed back in responses.

### Supported `cmd` values

#### `get_devices`
Returns an array of attached HID devices.

Response payload (example):

```json
{
  "evt":"resp",
  "rid":1,
  "cmd":"get_devices",
  "devices":[
    {"device_id":123,"dev_addr":1,"role":"stick","num_elements":21,"report_desc_len":134}
  ]
}
```

#### `get_descriptor`
Requests the HID report descriptor for a `device_id`.

Request:

```json
{ "rid": 2, "cmd": "get_descriptor", "device_id": 123 }
```

Response:

- A normal JSON response on `EVT` (type=1) with `descriptor_len`
- The raw descriptor bytes are streamed on `EVT` as **type=2** frames (see EVT framing below)

#### `start_stream` / `stop_stream`
Enable/disable the ~60Hz binary sample stream on the `STREAM` characteristic.

Request:

```json
{ "rid": 3, "cmd": "start_stream" }
```

#### `get_config`
Returns the current configuration blob as a JSON string field.

#### `set_config`
Sets the configuration blob from a JSON object in the `config` field.

#### `reboot_to_run` / `reboot_to_config`
Schedules a reboot into the specified mode.

---

## EVT: chunked notifications

Because some clients may be limited to an MTU of 23 (20-byte attribute payload), `EVT` uses a simple chunking header.

**Frame format (little-endian fields):**

| Offset | Type | Name | Notes |
|---:|---|---|---|
| 0 | `u8` | version | currently `1` |
| 1 | `u8` | type | `1=json`, `2=descriptor`, `3=config` (reserved) |
| 2 | `u16` | msg_id | increments per logical message |
| 4 | `u16` | offset | byte offset within the message |
| 6 | `u16` | total_len | total message length in bytes |
| 8 | `u8[]` | payload | chunk bytes (typically ≤12 bytes at MTU=23) |

The client reassembles chunks by `(msg_id, total_len)` until `offset + payload_len == total_len`.

---

## STREAM: 60Hz binary samples

When streaming is enabled and notifications are subscribed, the device emits one `StreamSample` about every 16 ms.

**Structure (`packed`):**

| Field | Type | Notes |
|---|---|---|
| version | `u8` | currently `1` |
| flags | `u8` | reserved (0) |
| device_id | `u32` | identifies which HID device |
| element_id | `u32` | identifies which input element |
| raw | `i32` | raw sample value |
| norm_q15 | `i16` | normalized [-1..1] mapped to Q15 (×32767) |

Total: 16 bytes.

---

## CFG: configuration blob

`CFG` is a raw byte characteristic intended to hold a JSON document.

- **Read:** returns the current config bytes.
- **Write:** supports sequential writes; if the write offset is 0 the blob is cleared and rebuilt.

Max size: 4096 bytes.

---

## Web Bluetooth notes

- The Web Bluetooth permission grant must include the Config service UUID.
  - Include the service UUID in `filters: [{ services: [UUID] }]` and/or `optionalServices: [UUID]`.
- If Chrome previously connected to the device without requesting the Config UUID, it may cache a permission grant that does not include it.
  - Fix by removing/forgetting the device from the site's Bluetooth permissions (or clearing site data) and reconnecting.
