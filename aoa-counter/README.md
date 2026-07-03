# AI-B100 AOA Counter

An Axis ACAP application that reads Axis Object Analytics (AOA) Counting and Occupancy scenarios on an Axis camera and publishes compact numeric payloads over LoRaWAN through an AI-B100 bridge.

The camera performs all analytics locally. The ACAP sends only numeric Counting and Occupancy data to the bridge; no images or video leave the camera.

---

## What It Does

- Reads AOA `CrosslineCounting` scenarios and publishes the accumulated counts shown by the AOA GUI.
- Creates, manages, and reads AOA `OccupancyInArea` scenarios.
- Publishes Counting and Occupancy independently, each with its own interval, LoRaWAN port, enabled scenarios, and enabled object classes.
- Generates a combined JavaScript decoder/translator for both Counting and Occupancy payloads.
- Receives AI-B100 status, downlink, GPS, and link-check callbacks.
- Provides web pages for Publish, Counting, Occupancy, LoRA Bridge, LoRA Downlink, GPS, and About.
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

---

## User Interface

| Page | Purpose |
| --- | --- |
| Publish | Configure Counting and Occupancy publish intervals/ports, publish manually, and view the last 10 LoRaWAN publishes |
| Counting | View AOA accumulated counts, select classes and scenarios for Counting payloads, and synchronize/reset counter state |
| Occupancy | Create/edit/delete OccupancyInArea scenarios, select classes and areas for Occupancy payloads, and view current occupancy status |
| LoRA Bridge | Configure bridge IP/callbacks, join/restart the bridge, request status, and run link checks |
| LoRA Downlink | View downlink messages and enable/disable supported downlink commands |
| GPS | View GPS callback data from the bridge |
| About | View app/device information, download the combined decoder, and download the Installation Info text report |

---

## Settings Model

The current settings schema is version 3. Counting and Occupancy publish configuration is separated under `transmission.counting` and `transmission.occupancy`.

```json
{
       "settingsVersion": 3,
       "transmission": {
              "counting": {
                     "intervalMinutes": 15,
                     "port": 10,
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
                     "intervalMinutes": 15,
                     "port": 0,
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
              }
       }
}
```

Port `0` disables publishing for that payload type. Enabled classes are encoded in the fixed order `human`, `car`, `bike`, `bus`, `truck`, `other`.

---

## Uplink Payloads

Counting and Occupancy use independent LoRaWAN ports. The generated decoder knows the configured ports, enabled scenarios, enabled labels, and Occupancy value type.

### Counting Payload

Counting payloads use the AOA accumulated counts as the source of truth. Each selected label is encoded as a little-endian unsigned 16-bit value and wraps modulo 65536.

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

Occupancy payloads repeat one block per configured OccupancyInArea scenario.

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

---

## Decoder / Translator

The app generates one JavaScript decoder for both Counting and Occupancy. Download it from the Publish page or from the About page.

The decoder includes:

- configured Counting and Occupancy ports
- configured scenario names
- enabled classes per scenario
- buffer layout comments
- example decoded JSON output
- a `decodeByPort(port, bytes)` dispatcher

The repository also contains [translator.js](translator.js) as a standalone decoder reference.

---

## Downlink Commands

Commands are short binary payloads sent from the LoRaWAN network server to the bridge. Individual commands can be enabled or disabled in the LoRA Downlink page.

### Port 10 - Actions

| Byte | Action |
| --- | --- |
| `0x01` | Restart the AI-B100 bridge |
| `0x02` | Initiate a new LoRaWAN OTAA join |
| `0x03` | Reset AOA and internal counters |

### Port 11 - Configuration

Two-byte payloads use `[command, value]`.

| Byte 0 | Byte 1 | Action |
| --- | --- | --- |
| `0x01` | `5..60` | Set publish interval in minutes |
| `0x02` | `0..5` | Set fixed data rate, DR0..DR5 |
| `0x03` | `0` or `1` | Disable or enable ADR |

### Port 12 - Information Requests

| Byte | Action |
| --- | --- |
| `0x01` | Reply on port 5 with camera information |
| `0x02` | Reply on port 6 with bridge information |
| `0x03` | Update local signal quality status from bridge/link data |

The app no longer sends automatic text test payloads on port 7.

---

## Bridge Integration

The ACAP configures AI-B100 callback endpoints for status, downlink receive, and GPS updates:

- `/local/aib100/b100_status`
- `/local/aib100/b100_receive`
- `/local/aib100/b100_gps`

The B100 API accepts commands such as join, send, status request, and link check asynchronously. The UI therefore uses callbacks and recent status data when showing bridge health.

Every 10th uplink is sent confirmed for link-health observation. If the confirmed ACK is not observed within the timeout, the app records the timeout and clears the waiting state without sending extra application payloads.

---

## Installation Info Report

The About page includes an `Installation Info` button at the top of the page. It downloads a plain-text report instead of raw JSON.

The report includes:

- Camera information: model, serial, firmware, network, uptime, and app version
- Bridge information: connection/callback status, LoRaWAN state, signal quality, GPS, and AI-B100 parameters
- Counting settings: publish interval, port, classes, selected scenarios, current counter state, and AOA Counting scenarios
- Occupancy settings: publish interval, port, value type, classes, selected areas, current Occupancy status, and AOA OccupancyInArea scenarios
- Collection warnings if one of the source endpoints cannot be read

---

## Building

Docker is required. The build script creates packages for both supported architectures.

```bash
./build.sh
```

Output:

- `AI-B100_AOA_Counter_<version>_aarch64.eap`
- `AI-B100_AOA_Counter_<version>_armv7hf.eap`

---

## Installing

Install the matching `.eap` package from the camera web UI under Apps, or upload it to the Axis application upload endpoint with digest authentication.

```bash
curl --digest -u '<user>:<password>' \
       -F 'packfil=@AI-B100_AOA_Counter_1_1_0_aarch64.eap;type=application/octet-stream' \
       'http://<camera-host>/axis-cgi/applications/upload.cgi'
```

---

## Repository Layout

| Path | Description |
| --- | --- |
| [app/main.c](app/main.c) | ACAP backend, settings migration, AOA events, LoRaWAN publishing, HTTP endpoints, decoder generation |
| [app/B100.c](app/B100.c) | AI-B100 HTTP client and callback parsing |
| [app/html/index.html](app/html/index.html) | Publish page |
| [app/html/aoa.html](app/html/aoa.html) | Counting page |
| [app/html/occupancy.html](app/html/occupancy.html) | Occupancy page |
| [app/html/bridge.html](app/html/bridge.html) | LoRA Bridge page |
| [app/html/downlink.html](app/html/downlink.html) | LoRA Downlink page |
| [app/html/about.html](app/html/about.html) | About page and Installation Info report |
| [app/settings/settings.json](app/settings/settings.json) | Default settings |
| [translator.js](translator.js) | Standalone decoder reference |

---

# History

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
