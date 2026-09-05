# AI-B100 Radar JavaScript Encoder and Decoder

AI-B100 Radar provides three JavaScript integration files for LoRaWAN network servers, Node-RED, and other JavaScript runtimes:

| File | URL | Purpose |
| --- | --- | --- |
| Data Decoder | `/local/aib100/translator` | Decode Counting, Occupancy, Detection Alert, Speed, and information uplinks |
| OTA Encoder | `/local/aib100/js/ota_encoder.js` | Convert configuration requests into a LoRaWAN port and HEX payload |
| OTA Decoder | `/local/aib100/js/ota_decoder.js` | Decode OTA requests, responses, acknowledgements, capabilities, and errors |

Open **Publish > Data Decoder** or **LoRA Downlink > OTA Encoder / OTA Decoder** to view, copy, or download the files.

The Data Decoder is generated from the current Counting field order and Speed unit and limit. Download a fresh copy after changing Counting scenes or classes, or the Speed unit or limit. The OTA Encoder and OTA Decoder are configuration-independent and do not need to be refreshed after settings changes.

## Data Decoder

The generated Data Decoder provides two entry points:

```javascript
var decoded = Decode(port, hexPayload);
var networkServerResult = decodeUplink({ fPort: port, bytes: byteArray });
```

`Decode(hexPayload, port)` is also accepted. A HEX payload must have an even number of characters and must not include a `0x` prefix.

### Payload Structure

| Port | Use case | Payload |
| --- | --- | --- |
| `1` | Counting | One big-endian `uint16` for every enabled scene/class field embedded in `CountingFields` |
| `2` | Occupancy | One byte: selected-label interval maximum |
| `3` | Detection Alert | One byte: `0` when inactive, otherwise the active interval maximum |
| `4` | Speed | Five bytes: vehicles, speeding, maximum, average, minimum |
| `121` | Camera information | ASCII CSV: model, serial, firmware, uptime, CPU load, app version |
| `122` | Bridge information | ASCII CSV: hardware, firmware, power source, temperature, restarts, DevAddr |

Counting values are cumulative, wrap at 65536 on the wire, and follow the exact `CountingFields` array in the downloaded decoder. Human precedes Vehicle when both classes are enabled for a scene.

The examples below assume a generated decoder with this field mapping:

```javascript
var CountingFields = [
  { "scene": "Gate", "className": "human" },
  { "scene": "Gate", "className": "vehicle" }
];
```

Decode a Counting payload:

```javascript
Decode(1, "012C0005");
```

```json
{
  "port": 1,
  "useCase": "counting",
  "scenes": {
    "Gate": {
      "human": 300,
      "vehicle": 5
    }
  }
}
```

Decode an Occupancy interval maximum:

```javascript
Decode(2, "07");
```

```json
{
  "port": 2,
  "useCase": "occupancy",
  "maximumObjects": 7
}
```

Decode an active Detection Alert:

```javascript
Decode(3, "03");
```

```json
{
  "port": 3,
  "useCase": "detectionAlert",
  "active": true,
  "maximumObjects": 3
}
```

Decode a Speed summary generated in a configuration using `km/h` and a limit of `50`:

```javascript
Decode(4, "0C03543E1C");
```

```json
{
  "port": 4,
  "useCase": "speed",
  "unit": "km/h",
  "speedLimit": 50,
  "vehicles": 12,
  "speeding": 3,
  "maximumSpeed": 84,
  "averageSpeed": 62,
  "minimumSpeed": 28
}
```

## OTA Frame Structure

Modern Radar OTA frames use this byte structure:

```text
[command, version, transactionId, body..., crc8]
```

- Protocol version is `0x01`.
- `transactionId` is `0..255` and defaults to `0`.
- CRC-8 uses polynomial `0x07` and initial value `0x00`.
- The maximum frame length is 51 bytes.
- Requests and responses use the same LoRaWAN port.
- Coordinates use integers from `0` through `1000`, with `(0,0)` at the top-left.

| Code | Meaning |
| --- | --- |
| `0x01` | GET request |
| `0x02` | SET or execute request |
| `0x03` | Capabilities request |
| `0x04` | List request |
| `0x81` | GET response |
| `0x82` | SET acknowledgement |
| `0x83` | Capabilities response |
| `0x84` | List response |
| `0xE0` | Error response |

## OTA Services

| Port | Service |
| --- | --- |
| `100` | Actions |
| `110` | LoRaWAN data rate and ADR |
| `111` | Radar detection sensitivity |
| `120` | Camera and bridge information |
| `130` | Use-case publishing configuration |
| `131` | Counting scenes |
| `132` | Occupancy configuration |
| `133` | Detection Alert configuration |
| `134` | Speed configuration |

## OTA Encoder

The OTA Encoder defines:

```javascript
var encoded = Encode(request);
```

It returns a numeric port and an uppercase HEX payload:

```json
{
  "port": 100,
  "payload": "0201010155"
}
```

Convert the HEX string to bytes before handing it to a network server that expects a byte array or Base64 value. Do not transmit the ASCII characters that spell the HEX string.

### Port 100: Actions

Restart the AI-B100 bridge:

```json
{
  "port": 100,
  "command": "set",
  "transactionId": 1,
  "action": "restartBridge"
}
```

Use `resetAllData` to reset all Radar use-case data. Request supported actions with:

```json
{
  "port": 100,
  "command": "caps"
}
```

### Port 110: LoRaWAN Configuration

Read the current Data Rate and ADR state:

```json
{
  "port": 110,
  "command": "get",
  "transactionId": 2
}
```

Set DR4 and enable ADR:

```json
{
  "port": 110,
  "command": "set",
  "transactionId": 3,
  "config": {
    "dataRate": 4,
    "adrEnabled": true
  }
}
```

The SET example encodes as:

```json
{
  "port": 110,
  "payload": "020103030401E4"
}
```

### Port 111: Radar Configuration

Set Radar Detection Sensitivity to Medium:

```json
{
  "port": 111,
  "command": "set",
  "transactionId": 4,
  "config": {
    "detectionSensitivity": "medium"
  }
}
```

Valid values are `low`, `medium`, and `high`. Use `get` to read the setting and `caps` to read supported values.

### Port 120: Information

Request camera information:

```json
{
  "port": 120,
  "command": "get",
  "transactionId": 5,
  "infoType": "camera",
  "page": 0
}
```

Use `bridge` to request bridge information. Modern responses use structured fields; the decoder also accepts paged legacy text responses.

### Port 130: Publishing Configuration

Enable Occupancy with a 15-minute interval:

```json
{
  "port": 130,
  "command": "set",
  "transactionId": 6,
  "service": "occupancy",
  "config": {
    "enabled": true,
    "intervalMinutes": 15
  }
}
```

Valid services are `counting`, `occupancy`, and `presence`. Detection Alert (`presence`) has no periodic interval. Read a service by changing `command` to `get`, or request all supported services with `caps` and no service value.

### Port 131: Counting Scenes

List configured scenes:

```json
{
  "port": 131,
  "command": "list",
  "transactionId": 7,
  "page": 0
}
```

Read a scene by index:

```json
{
  "port": 131,
  "command": "get",
  "sceneIndex": 1
}
```

Update an existing scene using the IDs and fingerprints returned by LIST or GET:

```json
{
  "port": 131,
  "command": "set",
  "transactionId": 8,
  "config": {
    "sceneIndex": 1,
    "id": 42,
    "sceneFingerprint": 4660,
    "mapFingerprint": 22136,
    "name": "Gate",
    "enabled": true,
    "direction": "leftToRight",
    "classes": {
      "human": true,
      "vehicle": false
    },
    "line": [
      { "x": 100, "y": 500 },
      { "x": 900, "y": 500 }
    ]
  }
}
```

To delete a scene, retain its current identifiers and fingerprints and add `"action": "delete"` at the top level. Always LIST or GET immediately before changing a scene; stale fingerprints are rejected.

### Port 132: Occupancy Configuration

```json
{
  "port": 132,
  "command": "set",
  "transactionId": 9,
  "config": {
    "enabled": true,
    "label": "human",
    "intervalMinutes": 15,
    "aoi": {
      "enabled": true,
      "points": [
        { "x": 100, "y": 100 },
        { "x": 900, "y": 100 },
        { "x": 900, "y": 900 },
        { "x": 100, "y": 900 }
      ]
    }
  }
}
```

Occupancy supports `human` or `vehicle` and an interval of 1 through 60 minutes. An enabled area requires 3 through 10 points; a disabled area must use an empty `points` array. Its value type is fixed to Interval Maximum.

### Port 133: Detection Alert Configuration

```json
{
  "port": 133,
  "command": "set",
  "transactionId": 10,
  "config": {
    "enabled": true,
    "label": "vehicle",
    "heartbeatMinutes": 15,
    "activeIntervalSeconds": 60,
    "transitionSeconds": 5,
    "schedule": {
      "enabled": true,
      "start": "18:00",
      "end": "06:00"
    },
    "aoi": {
      "enabled": false,
      "points": []
    }
  }
}
```

Times use local 24-hour `HH:MM` values. An enabled area requires 3 through 10 points; a disabled area must use an empty `points` array.

### Port 134: Speed Configuration

```json
{
  "port": 134,
  "command": "set",
  "transactionId": 11,
  "config": {
    "enabled": true,
    "unit": "kmh",
    "intervalMinutes": 15,
    "speedLimit": 50,
    "aoi": {
      "enabled": false,
      "points": []
    }
  }
}
```

Speed supports `kmh` or `mph` and a speed limit from 1 through 255. An enabled area requires 3 through 8 points; a disabled area must use an empty `points` array.

For ports 111, 132, 133, and 134, omit `config` and use `get` to read the current configuration or `caps` to read supported ranges.

## OTA Decoder

The OTA Decoder defines:

```javascript
var decoded = Decode(port, hexPayload);
```

It accepts an even-length HEX string or byte array. It validates frame length, protocol version, and CRC before decoding service-specific fields. It also decodes the older fixed-size port 132 and 133 configuration frames for compatibility.

Decoding the port 110 SET request above returns:

```json
{
  "commandCode": 2,
  "command": "set",
  "version": 1,
  "transactionId": 3,
  "body": [3, 4, 1],
  "port": 110,
  "config": {
    "fieldMask": 3,
    "dataRate": 4,
    "adrEnabled": true
  }
}
```

A decoded acknowledgement contains the original transaction ID and status:

```json
{
  "commandCode": 130,
  "command": "set_ack",
  "version": 1,
  "transactionId": 11,
  "body": [0],
  "port": 134,
  "statusCode": 0,
  "status": "ok"
}
```

Error responses identify the rejected request command and status:

```json
{
  "commandCode": 224,
  "command": "error",
  "version": 1,
  "transactionId": 8,
  "body": [2, 7],
  "port": 131,
  "requestCommand": 2,
  "statusCode": 7,
  "status": "scene_fingerprint_mismatch"
}
```

Status values include `ok`, `invalid_length`, `invalid_value`, `invalid_range`, `crc_mismatch`, `unknown_command`, `unknown_scene`, `scene_fingerprint_mismatch`, `map_fingerprint_mismatch`, `partial_page_pending`, `apply_failed`, and `unsupported`.

## Node-RED Usage

### Current Flow Export

[Node-RED/radar-encoder-decoder.json](Node-RED/radar-encoder-decoder.json) currently contains one **Extract hexstring message and port** Change node. It maps:

```text
msg.payload.event.encodedData.port       -> msg.topic
msg.payload.event.encodedData.hexEncoded -> msg.payload
```

The node is wired to node ID `3b408da012746a13`, which is not included in the export. The file therefore is not yet a complete importable flow: it does not contain the Data Decoder, OTA Encoder, OTA Decoder, inject, downlink, or debug nodes described below.

### Data Decoder Function Node

Paste the complete generated Data Decoder into a Function node, then append:

```javascript
try {
  msg.payload = Decode(Number(msg.topic), String(msg.payload));
  return msg;
} catch (error) {
  node.error(error.message, msg);
  return null;
}
```

Example input:

```json
{
  "topic": 4,
  "payload": "0C03543E1C"
}
```

### OTA Request Encoder Function Node

Use an Inject node to place one of the OTA request objects above in `msg.payload`. Paste the complete OTA Encoder into a Function node, then append:

```javascript
try {
  var encoded = Encode(msg.payload);
  msg.topic = encoded.port;
  msg.payload = encoded.payload;
  return msg;
} catch (error) {
  node.error(error.message, msg);
  return null;
}
```

Example output:

```json
{
  "topic": 111,
  "payload": "0201040100020D"
}
```

Adapt this message to the downlink schema expected by the LoRaWAN network server and convert the HEX payload to bytes or Base64 as required.

### OTA Response Decoder Function Node

Normalize the response to numeric `msg.topic` and a HEX string in `msg.payload`. Paste the complete OTA Decoder into a Function node, then append:

```javascript
try {
  msg.payload = Decode(Number(msg.topic), msg.payload);
  return msg;
} catch (error) {
  node.error(error.message, msg);
  return null;
}
```

Use `transactionId` to correlate responses when multiple requests can overlap.

### Simulating a Bridge Downlink Callback

For commissioning, a Node-RED HTTP Request node can POST to:

```text
http://camera-ip/local/aib100/b100_receive
```

Replace `camera-ip`, configure HTTP Digest authentication for a camera Viewer account, set `Content-Type: application/json`, and send a callback body such as:

```json
{
  "confirmed": 0,
  "fcntDown": 42,
  "rssi": -76,
  "snr": 10.25,
  "tUnix": 1788600000,
  "next_upload_ms": 0,
  "port": 111,
  "length": 7,
  "payload": [2, 1, 4, 1, 0, 2, 13]
}
```

This simulates the `0201040100020D` request that sets Radar Detection Sensitivity to Medium. Production downlinks should arrive through the LoRaWAN network and AI-B100 bridge.

## Operational Checklist

1. Configure the active Radar use cases in the ACAP UI.
2. Download a fresh Data Decoder after changing Counting fields or Speed units or limits.
3. Copy the OTA Encoder and OTA Decoder from the LoRA Downlink page.
4. Normalize incoming messages to a numeric port plus HEX payload.
5. Convert encoder HEX output to the byte representation required by the network server.
6. Correlate OTA responses with `transactionId`.
7. LIST or GET a Counting scene immediately before SET or delete so its fingerprints are current.
