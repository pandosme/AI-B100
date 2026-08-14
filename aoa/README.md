# AI-B100 AOA

An Axis ACAP application that reads Axis Object Analytics (AOA) Counting and Occupancy scenarios on an Axis camera and publishes compact numeric payloads over LoRaWAN through an AI-B100 bridge.

The camera performs all analytics locally. The ACAP sends only numeric Counting and Occupancy data to the bridge; no images or video leave the camera.

---

## What It Does

- Reads AOA `CrosslineCounting` scenarios and publishes the accumulated counts shown by the AOA GUI.
- Creates, manages, and reads AOA `OccupancyInArea` scenarios.
- Publishes Counting and Occupancy independently, each with its own interval, enabled scenarios, and enabled object classes.
- Uses fixed LoRaWAN application ports: Counting on port `1` and Occupancy on port `2`.
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

For shared staging, IP addressing, callback-account setup, LoRaWAN registration, and field verification, start with [../DEPLOYMENT.md](../DEPLOYMENT.md). This README focuses on how the AOA ACAP itself works and how to operate it after installation.

---

## How It Works

Counting and Occupancy are separate publish streams.

Counting reads AOA `CrosslineCounting` scenario totals. The app keeps the scenario selection and class selection in its own settings, then encodes the currently selected accumulated AOA values as compact little-endian `uint16` values on LoRaWAN port `1`.

Occupancy manages AOA `OccupancyInArea` scenarios and reads the selected area values. It encodes one compact block per selected area on LoRaWAN port `2`, using the configured value type: maximum, minimum, or average.

The AI-B100 bridge handles all LoRaWAN radio work. The ACAP talks to the bridge over HTTP and receives bridge status, downlink, and GPS data by callback on the camera.

---

## Operating Flow

1. Install and start the `.eap` package from the Axis camera Apps page.
2. Open the app UI and go to **LoRA Bridge**.
3. Set the AI-B100 bridge IP, callback IP, callback port, and callback digest credentials. Save, then request status or join.
4. Go to **Counting** and select the AOA `CrosslineCounting` scenarios and object classes that should be included in the Counting payload.
5. Go to **Occupancy** to create or edit AOA `OccupancyInArea` scenarios, choose the value type, and select the areas/classes to include in the Occupancy payload.
6. Go to **Publish** to enable Counting and/or Occupancy, set each interval, publish manually, and inspect recent uplinks.
7. Download the JavaScript translator from **Publish** or **About** after changing scenario, area, class, or value-type selections.
8. Use **LoRA Downlink**, **GPS**, and **About** for received downlinks, GPS callback data, link state, and the Installation Info report.

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

The app is served from the shared package path `/local/aib100/`. The package friendly name is **AI-B100 AOA** and the package `appName` is `aib100`.

---

## Settings Model

The current settings schema is version 4. Counting and Occupancy publish configuration is separated under `transmission.counting` and `transmission.occupancy`.

```json
{
       "settingsVersion": 4,
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
              }
       }
}
```

Counting and Occupancy use fixed protocol ports. The `enabled` field controls whether a stream publishes; the `port` field is kept as read-only protocol metadata. Enabled classes are encoded in the fixed order `human`, `car`, `bike`, `bus`, `truck`, `other`.

---

## Uplink Payloads

Counting and Occupancy use independent fixed LoRaWAN ports. The generated decoder knows the fixed ports, enabled scenarios, enabled labels, and Occupancy value type.

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

---

## Decoder / Translator

The app generates one JavaScript decoder for both Counting and Occupancy. Download it from the Publish page or from the About page.

The decoder includes:

- fixed Counting and Occupancy ports
- configured scenario names
- enabled classes per scenario
- buffer layout comments
- example decoded JSON output
- a `decodeByPort(port, bytes)` dispatcher

Download a fresh decoder after changing scenario selections, labels, areas, or Occupancy value type.

---

## Downlink Commands

Commands are short binary payloads sent from the LoRaWAN network server to the bridge. Individual commands can be enabled or disabled in the LoRA Downlink page.

### Port 100 - Actions

| Byte | Action |
| --- | --- |
| `0x01` | Restart the AI-B100 bridge |
| `0x02` | Initiate a new LoRaWAN OTAA join |
| `0x03` | Reset AOA and internal counters |

### Port 110 - Configuration

Two-byte payloads use `[command, value]`.

| Byte 0 | Byte 1 | Action |
| --- | --- | --- |
| `0x01` | `5..60` | Set publish interval in minutes |
| `0x02` | `0..5` | Set fixed data rate, DR0..DR5 |
| `0x03` | `0` or `1` | Disable or enable ADR |

### Port 120 - Information Requests

| Byte | Action |
| --- | --- |
| `0x01` | Reply on port 121 with camera information |
| `0x02` | Reply on port 122 with bridge information |
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

- `AI-B100_AOA_1_2_0_aarch64.eap`
- `AI-B100_AOA_1_2_0_armv7hf.eap`

Use `aarch64` for ARTPEC-8 and ARTPEC-9 cameras. Use `armv7hf` for ARTPEC-7 cameras.

---

## Installing

Install the matching `.eap` package from the camera web UI under Apps, or use the helper script from this directory.

```bash
./install.sh <camera-host> AI-B100_AOA_1_2_0_aarch64.eap
```

The equivalent Axis application upload endpoint call is:

```bash
curl --digest -u '<user>:<password>' \
       -F 'packfil=@AI-B100_AOA_1_2_0_aarch64.eap;type=application/octet-stream' \
       'http://<camera-host>/axis-cgi/applications/upload.cgi'
```

The AOA and Radar variants both use `appName: "aib100"`. Install the AOA variant when the target camera should run Axis Object Analytics based Counting and Occupancy.

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

---

# History

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
