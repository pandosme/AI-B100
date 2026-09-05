# AI-B100 AOA JavaScript Encoder and Decoder

AI-B100 AOA generates three JavaScript files for integrating LoRaWAN payloads with Node-RED, a network server, or another JavaScript runtime:

| File | Endpoint | Purpose |
| --- | --- | --- |
| Data Decoder | `/local/aib100/translator` | Decode Counting, Occupancy, and Presence Alert uplinks on ports 1-3 |
| OTA Encoder | `/local/aib100/ota_encoder` | Convert JSON requests into OTA downlink port and HEX payload values |
| OTA Decoder | `/local/aib100/ota_decoder` | Decode OTA requests, responses, acknowledgements, capabilities, and errors on ports 100 and 110-133 |

Open **Publish > Data Decoder** or **LoRA Downlink > OTA Encoder / OTA Decoder** to view, copy, or download the current files.

The files are generated for the current camera configuration. Download fresh copies after changing selected scenarios, labels, Occupancy value type, or after creating, deleting, renaming, or reordering AOA scenes. The OTA Encoder and OTA Decoder embed scene IDs and fingerprints that protect against updating the wrong or an outdated scene.

## Data Decoder

The Data Decoder defines one global entry point:

```javascript
var decoded = Decode(port, hexPayload);
```

- `port` is the numeric LoRaWAN application port.
- `hexPayload` is an even-length hexadecimal string without a `0x` prefix, for example `"2C010500"`.
- `Decode(hexPayload, port)` is also accepted.
- The returned value is a JavaScript object ready for JSON serialization.

The examples below assume a generated decoder with these configured items:

- Counting scenario `Entry`, labels `human` and `car`
- Occupancy scenario `Lobby`, labels `human` and `car`, value type `max`
- Presence Alert scenarios `Door` and `Loading Dock`

Actual property names and label order come from the configuration embedded in your downloaded decoder. Scenario names are converted to JSON-safe keys, so `Loading Dock` becomes `Loading_Dock`.

### Counting, Port 1

Each configured scenario contributes one little-endian `uint16` per selected label. Values use the configured scenario order and label order.

```javascript
Decode(1, "2C010500");
```

```json
{
  "Entry": {
    "human": 300,
    "car": 5
  }
}
```

### Occupancy, Port 2

Each encoded area contributes `[labelCount, valueType, labelValue...]`, where `valueType` is `0=max`, `1=min`, or `2=avg`.

The current payload and decoder are positional, and runtime area order is established by the order in which AOA samples first arrive. After every app or camera restart, wait until every configured area has a fresh sample and then download a new Data Decoder before consuming Occupancy payloads. Do not reuse a pre-restart decoder: area arrival order can change and silently associate values with the wrong area name. The payload builder also omits an area until it has a sample, so decoding before all areas are ready can shift values or report a missing trailing header.

```javascript
Decode(2, "02000301");
```

```json
{
  "Lobby": {
    "type": "max",
    "human": 3,
    "car": 1
  }
}
```

### Presence Alert, Port 3

Each configured Presence Alert contributes one byte in configuration order. `0` is clear and `1` is alert.

```javascript
Decode(3, "0100");
```

```json
{
  "Door": {
    "presence": true
  },
  "Loading_Dock": {
    "presence": false
  }
}
```

## OTA Frame Structure

Every OTA frame is an uppercase HEX representation of:

```text
[command, version, transactionId, body..., crc8]
```

- Protocol version is `0x01`.
- `transactionId` is `0..255` and defaults to `0`.
- CRC-8 uses polynomial `0x07` and initial value `0x00`.
- The maximum frame length is 51 bytes.
- OTA requests and responses use the same LoRaWAN port.

| Code | Meaning |
| --- | --- |
| `0x01` | GET request |
| `0x02` | SET or execute request |
| `0x03` | Capabilities request |
| `0x04` | Scene list request |
| `0x81` | GET response |
| `0x82` | SET acknowledgement |
| `0x83` | Capabilities response |
| `0x84` | Scene list response |
| `0xE0` | Error response |

## OTA Encoder

The generated OTA Encoder defines:

```javascript
var encoded = Encode(request);
var singleHexPayload = EncodeHex(request);
```

`Encode()` returns one of these structures:

```json
{
  "port": 130,
  "message": "02010001010555"
}
```

```json
{
  "port": 132,
  "messages": [
    "<first HEX frame>",
    "<second HEX frame>"
  ]
}
```

The second form is used only when paged scene geometry needs multiple frames. Send every frame in array order. `EncodeHex()` returns only the HEX string and throws an error if the request requires multiple frames.

`type` selects the service port. You can instead supply a numeric `port` and explicit `command`. The canonical input field names are shown below; the generated encoder also accepts compatibility aliases such as `active`, `intervall`, and `adr`.

### Port 100: Actions

Restart the bridge:

```json
{
  "type": "Action",
  "config": {
    "action": "restartBridge"
  }
}
```

Reset Counting, Occupancy, and Presence runtime data:

```json
{
  "type": "Action",
  "config": {
    "action": "resetAllData"
  }
}
```

Request supported actions:

```json
{
  "port": 100,
  "command": "caps"
}
```

### Port 110: AI-B100 LoRaWAN Configuration

Read Data Rate and ADR:

```json
{
  "type": "LoRaWAN"
}
```

Set DR3 and enable ADR:

```json
{
  "type": "LoRaWAN",
  "config": {
    "dataRate": 3,
    "adrEnabled": true
  }
}
```

Request supported ranges:

```json
{
  "port": 110,
  "command": "caps"
}
```

### Port 120: Information

Request camera information:

```json
{
  "type": "Information",
  "config": {
    "service": "camera",
    "page": 0
  }
}
```

Use `"service": "bridge"` for bridge information. Normally each response fits in one frame. If a legacy response has `pageCount` greater than one, request each page and decode all returned frames together.

### Port 130: Publishing Configuration

Read the Counting publishing configuration:

```json
{
  "type": "Publish",
  "config": {
    "service": "counting"
  }
}
```

Enable Counting with a five-minute interval:

```json
{
  "type": "Publish",
  "config": {
    "service": "counting",
    "enabled": true,
    "intervalMinutes": 5
  }
}
```

Valid services are `counting`, `occupancy`, and `presence`. Presence Alert has no periodic interval.

### Port 131: Counting Scenes

List configured scenes:

```json
{
  "type": "Counting",
  "command": "list",
  "page": 0
}
```

Read a scene by generated scene index:

```json
{
  "type": "Counting",
  "sceneIndex": 1
}
```

Update an existing scene:

```json
{
  "type": "Counting",
  "sceneIndex": 1,
  "config": {
    "direction": "leftToRight",
    "publishClasses": ["human", "car"],
    "points": [
      [250, 500],
      [750, 500]
    ]
  }
}
```

### Port 132: Occupancy Scenes

List or read scenes using the same `list` and `sceneIndex` forms as Counting, with `"type": "Occupancy"`.

```json
{
  "type": "Occupancy",
  "sceneIndex": 1,
  "config": {
    "publishClasses": ["human", "car"],
    "valueType": "average",
    "points": [
      [250, 250],
      [750, 250],
      [750, 750],
      [250, 750]
    ]
  }
}
```

### Port 133: Presence Alerts

List or read alerts using the same `list` and `sceneIndex` forms, with `"type": "PresenceAlert"`.

```json
{
  "type": "PresenceAlert",
  "sceneIndex": 1,
  "config": {
    "classes": ["human"],
    "thresholdObjectCount": 1,
    "triggerDelaySeconds": 10,
    "schedule": {
      "enabled": true,
      "start": "18:00",
      "end": "06:00"
    },
    "points": [
      [250, 250],
      [750, 250],
      [750, 750],
      [250, 750]
    ]
  }
}
```

Scene coordinates are integers from `0` through `1000`, with `(0,0)` at the top-left and `(1000,1000)` at the bottom-right. OTA can update existing scenes but cannot create, delete, or rename them.

## OTA Decoder

The generated OTA Decoder defines:

```javascript
var decoded = Decode(port, hexPayload);
var decodedFromEncoderShape = Decode({ port: 130, message: hexPayload });
var decodedPages = Decode({ port: 132, messages: hexPayloads });
```

Every decoded frame starts with transport metadata:

```json
{
  "port": 110,
  "command": "set_ack",
  "commandCode": 130,
  "transactionId": 0
}
```

The decoder then adds service-specific fields. A successful port 110 SET acknowledgement is:

```json
{
  "port": 110,
  "command": "set_ack",
  "commandCode": 130,
  "transactionId": 0,
  "status": 0,
  "statusName": "ok"
}
```

An OTA error response is decoded as:

```json
{
  "port": 132,
  "command": "error",
  "commandCode": 224,
  "transactionId": 7,
  "requestCommand": 2,
  "requestCommandName": "set",
  "status": 7,
  "statusName": "sceneFingerprintMismatch"
}
```

A decoded Occupancy scene GET response has this general shape:

```json
{
  "port": 132,
  "command": "get_response",
  "commandCode": 129,
  "transactionId": 0,
  "scene": {
    "index": 1,
    "id": 8,
    "name": "Lobby",
    "fingerprint": 2002,
    "mapFingerprint": 1002
  },
  "page": 0,
  "pageCount": 1,
  "pointStart": 0,
  "totalPointCount": 4,
  "coordinateSystem": {
    "origin": "topLeft",
    "minimum": 0,
    "maximum": 1000
  },
  "config": {
    "publishClasses": {
      "human": true,
      "car": true,
      "bike": false,
      "bus": false,
      "truck": false,
      "other": false
    },
    "valueType": "average",
    "points": [
      { "x": 250, "y": 250 },
      { "x": 750, "y": 250 },
      { "x": 750, "y": 750 },
      { "x": 250, "y": 750 }
    ]
  }
}
```

The decoder validates frame length, protocol version, CRC, page ordering, transaction IDs, and cross-page scene IDs and fingerprints. The camera validates SET fingerprints against its current scene map and returns `sceneFingerprintMismatch` or `mapFingerprintMismatch` when a generated request is stale. Invalid decoder input throws a JavaScript `Error`; catch it in integration code.

## Node-RED Usage

### Current Flow Export

[Node-RED/aoa-encoder-decoder.json](Node-RED/aoa-encoder-decoder.json) currently contains only one **Post AOA Downlink simulation** HTTP Request node. It is wired to node ID `8366d979a806582f`, which is not present in the export. It does not currently contain the Data Decoder, OTA Encoder, OTA Decoder, inject, LoRaWAN, or debug nodes described below. Re-export the complete flow before treating this file as an importable example.

### Data Decoder Function Node

Paste the complete generated Data Decoder JavaScript into a Function node, then append:

```javascript
// Expected input:
// msg.topic   = numeric LoRaWAN fPort
// msg.payload = even-length HEX payload string
msg.payload = Decode(Number(msg.topic), String(msg.payload));
return msg;
```

Example input message:

```json
{
  "topic": 2,
  "payload": "02000301"
}
```

Example output message payload:

```json
{
  "Lobby": {
    "type": "max",
    "human": 3,
    "car": 1
  }
}
```

Normalize the network-server event before this Function node. For example, if the event exposes `payload.event.rawData.fPort` and `payload.event.rawData.data`, a Change node can set:

```json
[
  {
    "t": "set",
    "p": "topic",
    "pt": "msg",
    "to": "payload.event.rawData.fPort",
    "tot": "msg"
  },
  {
    "t": "set",
    "p": "payload",
    "pt": "msg",
    "to": "payload.event.rawData.data",
    "tot": "msg"
  }
]
```

### OTA Request Encoder Function Node

Use an Inject node to place one of the OTA request JSON objects above in `msg.payload`. Paste the complete generated OTA Encoder JavaScript into a Function node, then append:

```javascript
var encoded = Encode(msg.payload);
var frames = encoded.messages || [encoded.message];

return [frames.map(function (frame) {
  return {
    topic: encoded.port,
    payload: frame,
    otaRequest: msg.payload
  };
})];
```

The Function node emits one message per frame on output 1:

```json
{
  "topic": 130,
  "payload": "02010001010555",
  "otaRequest": {
    "type": "Publish",
    "config": {
      "service": "counting",
      "enabled": true,
      "intervalMinutes": 5
    }
  }
}
```

Adapt `msg.topic` and `msg.payload` to the downlink schema required by your LoRaWAN network server. Send HEX as bytes, not as the ASCII characters that spell the HEX string.

### OTA Response Decoder Function Node

For a single OTA response, paste the complete generated OTA Decoder JavaScript into a Function node, then append:

```javascript
try {
  msg.payload = Decode(Number(msg.topic), String(msg.payload));
  return msg;
} catch (error) {
  node.error(error.message, msg);
  return null;
}
```

For a multi-page information or scene response, collect the HEX frames in order before decoding:

```javascript
msg.payload = Decode({
  port: Number(msg.topic),
  messages: msg.payload
});
return msg;
```

Here `msg.payload` must be an array of HEX strings from one transaction. Do not combine pages from different transaction IDs.

### Simulating a Bridge Downlink Callback

The checked-in HTTP Request node posts to:

```text
http://camera-ip/local/aib100/b100_receive
```

Replace `camera-ip`, configure HTTP Digest authentication for a camera Viewer account, set `Content-Type: application/json`, and provide a callback body such as:

```json
{
  "confirmed": 0,
  "fcntDown": 42,
  "rssi": -76,
  "snr": 10.25,
  "tUnix": 1788600000,
  "next_upload_ms": 0,
  "port": 130,
  "length": 7,
  "payload": [2, 1, 0, 1, 1, 5, 85]
}
```

This example simulates the encoded `02010001010555` port 130 request. Use callback simulation only for commissioning and tests; production downlinks should arrive through the LoRaWAN network and AI-B100 bridge.

## Operational Checklist

1. Configure scenarios, labels, value types, and Presence Alert schedules in AI-B100 AOA.
2. Download all three generated JavaScript files.
3. Paste each file into its corresponding Node-RED Function node.
4. Normalize incoming LoRaWAN messages to numeric port plus HEX payload.
5. Convert encoder HEX output to bytes in the network-server integration.
6. Correlate OTA requests and responses with `transactionId` when requests can overlap.
7. Re-download the scripts whenever camera scene configuration changes, and regenerate the Data Decoder after every restart once all Occupancy areas have fresh samples.

Never hand-edit embedded scene IDs or fingerprints in generated scripts. Read or list a scene before updating it, and regenerate both OTA files if the camera reports `sceneFingerprintMismatch` or `mapFingerprintMismatch`.