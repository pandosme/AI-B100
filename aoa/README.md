# AI-B100 AOA

An Axis ACAP application that reads Axis Object Analytics (AOA) Counting and Occupancy In Area scenarios on an Axis camera and publishes compact Counting, Occupancy, and Presence Alert payloads over LoRaWAN through an AI-B100 bridge.

The camera performs all analytics locally. The ACAP sends only numeric Counting and Occupancy data to the bridge; no images or video leave the camera.

---

## What It Does

- Reads AOA `CrosslineCounting` scenarios and publishes the accumulated counts shown by the AOA GUI.
- Creates, manages, and reads AOA `OccupancyInArea` scenarios.
- Publishes Counting and Occupancy periodically and Presence Alert whenever a retained threshold state changes.
- Uses fixed LoRaWAN application ports: Counting on port `1`, Occupancy on port `2`, and Presence Alert on port `3`.
- Generates a combined JavaScript decoder/translator for all three payloads.
- Receives AI-B100 status, downlink, GPS, and link-check callbacks.
- Provides web pages for Publish, Counting, Occupancy, Presence Alert, LoRA Bridge, LoRA Downlink, GPS, and About.
- Provides a plain-text Installation Info report for field documentation.

---

## System Overview

```text
Axis camera with AOA + ACAP
                            |
                            | HTTP on local LAN
                            v
AI-B100 LoRaWAN bridge
                            |
                            | LoRaWAN uplink/downlink
                            v
LoRaWAN gateway / network server
                            |
                            v
Backend, dashboard, Node-RED, TTN, ChirpStack, or private LNS
```

---

## Required Hardware

| Component | Description |
| --- | --- |
| Axis camera | Axis camera supporting ACAP and Axis Object Analytics |
| AI-B100 bridge | Ethernet-to-LoRaWAN bridge by AI Embedded Nordic AB |
| Local network | Camera and bridge must be reachable from each other on the LAN |
| LoRaWAN network | Gateway and network server for OTAA join and payload routing |

For shared staging, IP addressing, callback-account setup, LoRaWAN registration, and field verification, start with [../DEPLOYMENT.md](../DEPLOYMENT.md). This README focuses on how the AOA ACAP itself works and how to operate it after installation.

---

## How It Works

Counting, Occupancy, and Presence Alert are separate publish streams.

Counting reads AOA `CrosslineCounting` scenario totals. The app keeps the scenario selection and class selection in its own settings, then encodes the currently selected accumulated AOA values as compact little-endian `uint16` values on LoRaWAN port `1`.

Occupancy manages AOA `OccupancyInArea` scenarios and reads the selected area values. It encodes one compact block per selected area on LoRaWAN port `2`, using the configured value type: maximum, minimum, or average.

Presence Alert creates and manages independent AOA `occupancyInArea` scenarios with threshold events enabled. Each area configures a minimum number of objects and an AOA trigger delay. The ACAP maps `Device1Scenario<ID>Threshold` events back to the configured scenario ID because threshold events contain only `active`, not scenario name or type. AOA controls the high delay. When AOA goes low, the ACAP retains the internal high state for twice the configured trigger delay; another high event cancels that pending clear. State changes are sent on port `3`.

The AI-B100 bridge handles all LoRaWAN radio work. The ACAP talks to the bridge over HTTP and receives bridge status, downlink, and GPS data by callback on the camera.

---

## Operating Flow

1. Install and start the `.eap` package from the Axis camera Apps page.
2. Open the app UI and go to **LoRA Bridge**.
3. Set the AI-B100 bridge IP, callback IP, callback port, and callback digest credentials. Save, then request status or join.
4. Go to **Counting** and select the AOA `CrosslineCounting` scenarios and object classes that should be included in the Counting payload.
5. Go to **Occupancy** to create or edit AOA `OccupancyInArea` scenarios and configure periodic Occupancy values.
6. Open **Presence Alert** to add or edit areas, drag each area over live video, select object classes, configure the object threshold and trigger delay, and view the retained state.
7. Go to **Publish** to enable the required streams, set periodic intervals, publish manually, and inspect recent uplinks.
8. Download the JavaScript translator from **Publish** or **About** after changing scenario, area, class, or value-type selections.
9. Use **LoRA Downlink**, **GPS**, and **About** for received downlinks, GPS callback data, link state, and the Installation Info report.

---

## User Interface

| Page | Purpose |
| --- | --- |
| Publish | Configure Counting, Occupancy, and Presence Alert publishing, publish manually, and view the last 10 LoRaWAN publishes |
| Counting | View AOA accumulated counts, select classes and scenarios for Counting payloads, and synchronize/reset counter state |
| Occupancy | Create/edit/delete OccupancyInArea scenarios and configure Occupancy payloads |
| Presence Alert | Add/edit/delete areas, configure classes and threshold timing, drag area polygons over live video, and view retained alert state |
| LoRA Bridge | Configure bridge IP/callbacks, join/restart the bridge, request status, and run link checks |
| LoRA Downlink | View downlink messages and enable/disable supported downlink commands |
| GPS | View GPS callback data from the bridge |
| About | View app/device information, download the combined decoder, and download the Installation Info text report |

The app is served from the shared package path `/local/aib100/`. The package friendly name is **AI-B100 AOA** and the package `appName` is `aib100`.

---

## Settings Model

The current settings schema is version 9. Counting, Occupancy, and Presence Alert configuration is separated under `transmission.counting`, `transmission.occupancy`, and `transmission.presence`.

```json
{
       "settingsVersion": 9,
          "b100": {
                 "ip": "192.168.1.250",
                 "port": 81,
                 "apiDigestUser": "lorabridge",
                 "apiDigestPassword": "lorabridge",
                 "callbackIP": "192.168.1.200",
                 "callbackPort": 80,
                 "callbackDigestUser": "lorabridge",
                 "callbackDigestPassword": "lorabridge"
          },
       "transmission": {
              "counting": {
                     "enabled": true,
                     "intervalMinutes": 15,
                     "port": 1,
                     "classes": {
                            "human": true,
                            "car": true,
                            "bike": true,
                            "bus": true,
                            "truck": true,
                            "other": true
                     },
                     "scenarios": {}
              },
              "occupancy": {
                     "enabled": false,
                     "intervalMinutes": 15,
                     "port": 2,
                     "value": "average",
                     "classes": {
                            "human": true,
                            "car": true,
                            "bike": true,
                            "bus": true,
                            "truck": true,
                            "other": true
                     },
                     "scenarios": {}
              },
              "presence": {
                     "enabled": false,
                     "port": 3,
                     "scenarios": {
                                       "Entrance": {}
                     }
              }
       }
}
```

All three streams use fixed protocol ports. The `enabled` field controls whether a stream publishes; the `port` field is read-only protocol metadata. Enabled Occupancy classes are encoded in the fixed order `human`, `car`, `bike`, `bus`, `truck`, `other`.

---

## Uplink Payloads

Counting, Occupancy, and Presence Alert use independent fixed LoRaWAN ports. The generated decoder knows the fixed ports and selected scenarios.

### Counting Payload

Counting payloads are sent on port `1` and use the AOA accumulated counts as the source of truth. Each selected label is encoded as a little-endian unsigned 16-bit value and wraps modulo 65536.

```text
counter 1 human: low byte, high byte
counter 1 car:   low byte, high byte
...
counter N label: low byte, high byte
```

Example bytes for two uint16 values:

```text
[0x2C, 0x01, 0x05, 0x00] => 300, 5
```

### Occupancy Payload

Occupancy payloads are sent on port `2` and repeat one block per configured OccupancyInArea scenario.

```text
byte 0: labelCount, number of following label values for this area
byte 1: valueType, 0=max, 1=min, 2=avg
byte 2..N: labelCount uint8 values in selected label order
```

Occupancy values are unscaled EventInterval values rounded and clamped to `0..255`.

Example payload:

```text
[0x01, 0x00, 0x00, 0x01, 0x00, 0x01]
```

Example decoded JSON:

```json
{
       "Area_1": { "type": "max", "human": 0 },
       "Area_2": { "type": "max", "car": 1 }
}
```

### Presence Alert Payload

Presence Alert state is sent on port `3` when publishing is enabled and a configured area's retained state changes. Each configured scene contributes exactly one byte in settings order. The checked classes define which detected objects can satisfy that scene's threshold; they do not create separate class values in the payload.

```text
byte 0: area 1 state, `0` clear or `1` alert
...
byte N-1: area N state, `0` clear or `1` alert
```

A value of `1` means the scene detected enough objects matching any of its checked classes. A value of `0` means it did not.

Examples:

```text
one area alert: [0x01]
one area clear: [0x00]
three areas with only area 2 alert: [0x00, 0x01, 0x00]
```

---

## Decoder / Translator

The app generates one JavaScript decoder for Counting, Occupancy, and Presence Alert. Download it from the Publish page or from the About page.

The decoder includes:

- fixed Counting, Occupancy, and Presence ports
- configured scenario names
- enabled classes per scenario
- buffer layout comments
- example decoded JSON output
- a `decodeByPort(port, bytes)` dispatcher
- explicit wrappers: `JavaScriptTranslator(port, bytes)`, `Decode(fPort, bytes)`, and `Decoder(bytes, port)`

The decoder accepts only an even-length hexadecimal string such as `"01"` or `"0001"`. The caller must convert base64, binary buffers, or other representations to hex before decoding. With the Node-RED flow setting `msg.topic` to the numeric port and `msg.payload` to the hex string, use `msg.payload = JavaScriptTranslator(msg.topic, msg.payload)`.

Download a fresh decoder after changing scenario selections, labels, areas, or Occupancy value type.

---

## OTA Configuration

All OTA requests and responses use the same LoRaWAN port. Ports 1 through 20 remain reserved for use-case data.

| Port | Group | Supported requests |
| --- | --- | --- |
| 100 | Actions: Restart Bridge and Reset All Data | SET, CAPS |
| 110 | AI-B100 Data Rate and ADR configuration | GET, SET, CAPS |
| 120 | Camera and bridge information | GET, CAPS |
| 130 | Use-case enable state and publish rate | GET, SET, CAPS |
| 131 | Existing Counting scene configuration | GET, SET, CAPS, LIST |
| 132 | Existing Occupancy scene configuration | GET, SET, CAPS, LIST |
| 133 | Existing Presence Alert configuration | GET, SET, CAPS, LIST |

Every frame is `[command, version, transactionId, body..., crc8]`. Version is `0x01`. CRC-8 uses polynomial `0x07` and initial value `0x00` over every preceding byte. Frames are limited to 51 bytes.

The command byte is transport metadata, not part of the canonical configuration body. It is required because both GET and SET requests arrive as LoRaWAN downlinks: GET asks the application to uplink its current configuration, while SET applies the supplied configuration. For configuration ports, the GET response body is the same body accepted by SET.

The transaction ID associates a GET response, acknowledgement, or error with its request. The generated encoder defaults it to `0`; callers normally do not need to provide it. An integration sending concurrent requests may optionally set `transactionId` from 0 through 255.

| Command | Request | Response |
| --- | --- | --- |
| `0x01` | GET | `0x81` GET response |
| `0x02` | SET or execute | `0x82` SET acknowledgement |
| `0x03` | Capabilities | `0x83` capabilities response |
| `0x04` | List scenes | `0x84` list response |
| | | `0xE0` error response |

Status values are `0x00` OK, `0x01` invalid length, `0x02` invalid value, `0x03` invalid range, `0x04` CRC mismatch, `0x05` unknown command, `0x06` unknown scene, `0x07` scene fingerprint mismatch, `0x08` map fingerprint mismatch, `0x09` more pages pending, `0x0A` apply failed, and `0x0B` unsupported.

### Actions and AI-B100

Port 100 SET bodies contain one action byte: `0x01` restarts the bridge and `0x02` resets Counting, Occupancy, and Presence runtime data. Its acknowledgement body is `[action, status]`.

Port 110 GET responses and SET requests share `[fieldMask, dataRate, adrEnabled]`. Field-mask bit 0 selects Data Rate and bit 1 selects ADR. Data Rate is DR0 through DR5 and ADR is 0 or 1.

Port 110 SET requests are applied by the background worker after the bridge downlink callback has returned. The SET acknowledgement reports the final bridge API result rather than only request validation.

Port 120 GET bodies are `[informationType, page]`, where type 1 is camera information and type 2 is bridge information. Current responses use compact length-prefixed fields and binary numeric values so the complete information normally fits in one 51-byte LoRaWAN frame. The decoder exposes camera information as `model`, `serial`, `firmware`, `uptimeHours`, and `appVersion`. Firmware and application versions are strings because dotted versions are not JSON numbers.

If unusually long device strings cannot fit in the structured frame, the ACAP falls back to legacy `[informationType, page, pageCount, textLength, text...]` responses. Read `pageCount` and request the remaining pages in that case. The decoder continues to merge legacy pages passed as `Decode({port: 120, messages: [...]})`.

Bridge information is decoded as `hardware`, `hardwareVersion`, `firmware`, `powerSource`, `temperatureC`, `restartCounter`, and `devAddr`.

OTA responses are queued after the downlink callback returns and retried while the bridge reports a duty-cycle delay. Accepted responses appear in the Publish log on their OTA request port.

### Transmission Configuration

Port 130 addresses one use case per request. GET takes a one-byte use-case index. Its response body is identical to the corresponding SET body.

| Index | Canonical GET response / SET body |
| --- | --- |
| 1 Counting | `[0x01, enabled, intervalMinutes]` |
| 2 Occupancy | `[0x02, enabled, intervalMinutes]` |
| 3 Presence Alert | `[0x03, enabled]` |

For example, the canonical body `01 01 05` enables Counting with a five-minute interval. `03 00` disables Presence Alert publishing. Intervals are 1 through 60 minutes.

### Scene Configuration

Ports 131 through 133 can update existing scenes but cannot create, delete, or rename them. Scene order is stable ascending AOA scene ID. Names and IDs are embedded in the camera-generated JavaScript files.

LIST requests contain `[page]`. Each response contains `[mapFingerprint:u16, totalScenes, page, pageCount, entriesOnPage, entries...]`; every six-byte entry is `[sceneIndex, sceneId:u16, sceneFingerprint:u16, pointCount]`. The generated decoder returns these as `mapFingerprint`, `totalScenes`, `page`, `pageCount`, and a `scenes` array enriched with scene names from its embedded map.

GET takes `[sceneIndex, page]`. The GET response body is byte-for-byte the same canonical body accepted by SET:

```text
[configVersion, sceneIndex, sceneId:u16, sceneFingerprint:u16,
 mapFingerprint:u16, page, pageCount, pointStart, pointsInPage,
 totalPoints, useCaseFields..., packedPoint:u24, ...]
```

Scene config version 2 exposes integer coordinates from 0 through 1000 with the origin at the top-left. `(0,0)` maps to AOA `(-1,-1)`, the center is `(500,500)`, and `(1000,1000)` maps to AOA `(1,1)`. Each point is packed little-endian into three bytes as `x | (y << 10)`; the upper four bits are reserved and must be zero. This reduces coordinate data from four to three bytes per point, allowing every supported ten-point scene to fit in one 51-byte frame.

SET continues to accept legacy config version 1 coordinates as two signed Q15 integers for compatibility. GET and the generated encoder use version 2. The generated decoder always returns version 2-style integer coordinates and includes `coordinateSystem: {origin: "topLeft", minimum: 0, maximum: 1000}`. Scene and map CRC16 fingerprints reject messages created from a stale scene map.

Presence Alert scene bodies include schedule fields in config version 2: `scheduleEnabled`, `startMinutes:u16`, and `endMinutes:u16`. Times are minutes from midnight (`0..1439`) and are exposed in JavaScript as 24-hour `HH:MM` strings. Default values are `18:00` to `06:00` with schedule disabled.

- Counting fields: direction, publish-class mask, reserved byte, then line vertices.
- Occupancy fields: publish-class mask, value type (`0=max`, `1=min`, `2=average`), reserved byte, then area vertices.
- Presence fields: detection-class mask, threshold object count, trigger delay as `u16`, schedule enabled flag, schedule start and end minutes as `u16`, then area vertices.
- Class-mask bits 0 through 5 are human, car, bike, bus, truck, and other.

### JavaScript Encoder and Decoder

The LoRA Downlink page provides separate **OTA Encoder** and **OTA Decoder** JavaScript files. Both are generated from the current camera scene map. Download new files after creating, deleting, or renaming a scene. They define global `Encode`, `EncodeHex`, and `Decode` functions for direct pasting into a Node-RED function node or LoRaWAN provider console; no module loader is required.

```javascript
var encoded = Encode({
       type: "Publish",
       config: {
              service: "counting",
              active: true,
              intervall: 5
       }
});

// { port: 130, message: "02010001010555" }
```

`type` selects the OTA port and the supplied fields determine whether the request is GET or SET. For example, Publish with only `service` requests the current setting; adding `active` or `intervall` applies a setting. The generated encoder contains JSON examples for ports 100, 110, 120, 130, 131, 132, and 133.

`Encode()` returns `{port, message}` for a single LoRaWAN message. Scene geometry can require multiple messages; in that case it returns `{port, messages}` and every message must be sent in order. `EncodeHex()` is available when the caller requires exactly one message. `Decode()` accepts the encoder result directly or `(port, hexPayload)`.

The decoder returns named fields for all acknowledgements, errors, and capability responses. These include `statusName`, the action, use case, or scene index associated with a SET acknowledgement, and port-specific capability objects instead of opaque byte arrays.

In a Node-RED function node for a single-message operation:

```javascript
var encoded = Encode(msg.payload);
msg.topic = encoded.port;
msg.payload = encoded.message;
return msg;
```

---

## Bridge Integration

AI-B100 firmware 2.0.0 and later can separate the web GUI and HTTP API. The recommended setup is GUI port `80` and API port `81`. The ACAP authenticates every API request with HTTP Digest using the configured Bridge API User and Password. Fresh installations default to `lorabridge` / `lorabridge`.

Bridges running firmware earlier than 2.0.0 remain supported on port `80`. Upgrading the ACAP preserves the saved bridge API port, so an existing port-80 installation is not moved automatically. Digest authentication is challenge-based and does not prevent an older unauthenticated endpoint from responding normally. The ACAP never silently falls back from port `81` to port `80`; an authentication or port error must be corrected in LoRA Bridge settings.

Bridge API credentials are separate from callback credentials. Callback credentials are the Axis camera Viewer account the bridge uses when posting to the ACAP. The recommended account is also `lorabridge` / `lorabridge`.

The ACAP configures AI-B100 callback endpoints for status, downlink receive, and GPS updates:

- `/local/aib100/b100_status`
- `/local/aib100/b100_receive`
- `/local/aib100/b100_gps`

The B100 API accepts commands such as join, send, status request, and link check asynchronously. The UI therefore uses callbacks and recent status data when showing bridge health.

Every 10th uplink is sent confirmed for link-health observation. If the confirmed ACK is not observed within the timeout, the app records the timeout and clears the waiting state without sending extra application payloads.

The bridge can be controlled from the LoRA Bridge page. Typical field actions are status request, OTAA join, restart, and link check. All of these operations are asynchronous because the AI-B100 may take several seconds to respond.

---

## Installation Info Report

The About page includes an `Installation Info` button at the top of the page. It downloads a plain-text report instead of raw JSON.

The report includes:

- Camera information: model, serial, firmware, network, uptime, and app version
- Bridge information: connection/callback status, LoRaWAN state, signal quality, GPS, and AI-B100 parameters
- Counting settings: enabled state, publish interval, fixed port, classes, selected scenarios, current counter state, and AOA Counting scenarios
- Occupancy settings: enabled state, publish interval, fixed port, value type, classes, selected areas, current Occupancy status, and AOA OccupancyInArea scenarios
- Collection warnings if one of the source endpoints cannot be read

---

## Building

Docker is required. The build script creates packages for both supported architectures.

```bash
cd aoa
./build.sh
```

Output:

- `AI-B100_AOA_2_0_0_aarch64.eap`
- `AI-B100_AOA_2_0_0_armv7hf.eap`

Use `aarch64` for ARTPEC-8 and ARTPEC-9 cameras. Use `armv7hf` for ARTPEC-7 cameras.

---

## Installing

Install the matching `.eap` package from the camera web UI under Apps, or use the helper script from this directory.

```bash
./install.sh <camera-host> AI-B100_AOA_2_0_0_aarch64.eap
```

The equivalent Axis application upload endpoint call is:

```bash
curl --digest -u '<user>:<password>' \
       -F 'packfil=@AI-B100_AOA_2_0_0_aarch64.eap;type=application/octet-stream' \
       'http://<camera-host>/axis-cgi/applications/upload.cgi'
```

The AOA and Radar variants both use `appName: "aib100"`. Install the AOA variant when the target camera should run Axis Object Analytics based Counting and Occupancy.

---

## Repository Layout

| Path | Description |
| --- | --- |
| [app/main.c](app/main.c) | ACAP backend, settings migration, AOA events, LoRaWAN publishing, HTTP endpoints, decoder generation |
| [../common/app/B100.c](../common/app/B100.c) | Shared AI-B100 HTTP client and callback parsing |
| [app/html/index.html](app/html/index.html) | Publish page |
| [app/html/aoa.html](app/html/aoa.html) | Counting page |
| [app/html/occupancy.html](app/html/occupancy.html) | Occupancy page |
| [app/html/presence.html](app/html/presence.html) | Presence page |
| [../common/app/html/bridge.html](../common/app/html/bridge.html) | Shared LoRA Bridge page |
| [app/html/downlink.html](app/html/downlink.html) | LoRA Downlink page |
| [app/html/about.html](app/html/about.html) | About page and Installation Info report |
| [app/settings/settings.json](app/settings/settings.json) | Default settings |

---

# History

## 2.0.0 - Authenticated Bridge API

- Added HTTP Digest authentication for AI-B100 firmware 2.0.0 and later.
- Changed the fresh-install bridge API default to port 81 while keeping the web GUI on port 80.
- Preserved saved port-80 settings for bridges running firmware earlier than 2.0.0.
- Added separate bridge API and camera callback credentials to the LoRA Bridge page.
- Hardened callback parsing for modern JSON content types and payload formats.
- Added a link to the bridge event log on GUI port 80.

## 1.2.0 - Fixed LoRaWAN Port Scheme

- Migrated settings to schema version 4.
- Replaced user-selectable Counting and Occupancy uplink ports with fixed protocol ports: Counting on port 1 and Occupancy on port 2.
- Added explicit `enabled` flags for Counting and Occupancy publishing.
- Moved downlink command groups to reserved management ports: actions on port 100, configuration on port 110, and information requests on port 120.
- Moved camera and bridge information replies to ports 121 and 122.
- Updated the Publish page to use enable toggles and fixed port labels.
- Updated the generated decoder to treat Counting and Occupancy ports as fixed protocol ports.

## 1.1.0 - Occupancy Branch

- Renamed the original AOA/Counters UI flow to Counting.
- Added an Occupancy tab for AOA OccupancyInArea setup and current status.
- Added independent Counting and Occupancy publishing with separate intervals, LoRaWAN ports, enabled classes, and enabled scenarios.
- Migrated settings to schema version 3 with `transmission.counting` and `transmission.occupancy`.
- Changed Counting LoRaWAN payload generation to use AOA accumulated counts as the source of truth.
- Encoded Counting values as little-endian uint16 with modulo-65536 wrapping.
- Added compact Occupancy payloads using per-area label count, value type, and uint8 label values.
- Added a combined JavaScript decoder/translator for Counting and Occupancy with detailed payload comments and JSON examples.
- Reworked the Publish page to show Counting and Occupancy controls side by side and moved the Last 10 LoRaWAN Publishes to its own row.
- Added callback-driven bridge status handling, GPS callbacks, and link-check status handling.
- Removed the Bridge page Send Test control.
- Removed automatic text payload sends on LoRaWAN port 7.
- Updated downlink Signal Quality handling to update local status instead of replying with a port 7 text message.
- Added the About page Installation Info text report for field documentation.

## 1.0.1 - Baseline

- CrosslineCounting support for AOA counters.
- AI-B100 bridge integration for LoRaWAN publishing.
- Basic bridge configuration, downlink log, and decoder support.

---

## License

MIT - see [app/LICENSE](app/LICENSE).

---

## Credits & Contact

- ACAP: Fred Juhlin
- AI-B100 hardware: AI Embedded Nordic AB, ai-b100@ai-embedded.se
- Axis Object Analytics: Axis Communications
