# AGENTS.md - AI-B100 Axis ACAP Collection

This file is the authoritative reference for LLMs working on this repository. It describes the current repository shape, shared AI-B100 bridge behavior, active ACAP variants, payload contracts, build outputs, and editing rules that must be preserved.

---

## 1. Project Summary

This repository contains Axis ACAP applications that connect Axis camera analytics to LoRaWAN through an external AI-B100 bridge on the local LAN.

Current ACAP variants:

| Directory | Friendly name | Source analytics | Active uplinks |
| --- | --- | --- | --- |
| `aoa/` | AI-B100 AOA | Axis Object Analytics `CrosslineCounting` and `OccupancyInArea` | Counting on port 1, Occupancy on port 2 |
| `radar/` | AI-B100 Radar | Axis radar scene provider | Occupancy on port 2, Detection Alert on port 3; Counting is reserved on port 1 but hidden |

Both variants intentionally use ACAP `appName` `aib100`. This keeps the camera URL path, settings store, and AI-B100 callback paths stable when switching between AOA and Radar variants. The variants are alternatives: install and run the one that matches the target camera and use case.

The top-level [README.md](README.md) is the repository overview. App-specific operation is documented in [aoa/README.md](aoa/README.md) and [radar/README.md](radar/README.md). Complete staging and field setup is documented in [DEPLOYMENT.md](DEPLOYMENT.md).

---

## 2. Repository Layout

```text
ai-b100/
|-- AGENTS.md
|-- README.md
|-- DEPLOYMENT.md
|-- Customization.md
|-- http_api.md
|-- aoa/
|   |-- AI-B100_AOA_1_2_0_aarch64.eap
|   |-- AI-B100_AOA_1_2_0_armv7hf.eap
|   |-- build.sh / install.sh / Dockerfile / README.md
|   `-- app/
|       |-- main.c, B100.c/h, ACAP.c/h, cJSON.c/h, counter.c/h, occupancy.c/h
|       |-- manifest.json, Makefile
|       |-- html/        # Publish, Counting, Occupancy, Bridge, Downlink, GPS, About
|       |-- settings/    # settings.json, state.json, events.json, subscriptions.json
|       `-- localdata/   # persisted counters
|-- radar/
|   |-- AI-B100_Radar_1_2_0_aarch64.eap
|   |-- AI-B100_Radar_1_2_0_armv7hf.eap
|   |-- build.sh / install.sh / Dockerfile / README.md
|   |-- translator.js
|   `-- app/
|       |-- main.c, B100.c/h, ACAP.c/h, cJSON.c/h
|       |-- counting.c/h, occupancy.c/h, alert.c/h
|       |-- RadarDetection.*, radarscene.pb-c.*, protobuf-c.*
|       |-- manifest.json, Makefile
|       |-- html/        # Publish, Occupancy, Detection Alert, Radar, Bridge, Downlink, GPS, About
|       |-- settings/
|       `-- localdata/
`-- images/
```

Historical files named `lorawan-counter/` or `AI-B100_AOA_Counter_0_5_0_*` are stale references and should not be reintroduced.

---

## 3. Deployment Defaults

Use the same static addressing at every installation unless the user explicitly asks otherwise.

| Device | Recommended IP | Subnet mask |
| --- | --- | --- |
| Laptop for commissioning | `192.168.1.10` | `255.255.255.0` |
| Axis camera or radar camera | `192.168.1.200` | `255.255.255.0` |
| AI-B100 bridge | `192.168.1.250` | `255.255.255.0` |

Important defaults:

- Bridge HTTP port: `80`
- ACAP callback IP: camera/radar address, usually `192.168.1.200`
- Callback digest user/password example: `aib100` / `aib100`
- AI-B100 callback paths:
  - `/local/aib100/b100_status`
  - `/local/aib100/b100_receive`
  - `/local/aib100/b100_gps`

Avoid adding new documentation that recommends the old `192.168.0.x` network scheme.

---

## 4. AI-B100 Bridge Behavior

The AI-B100 bridge is an ESP32-S3 based Ethernet-to-LoRaWAN bridge. The ACAP talks to it over local HTTP and the bridge handles LoRaWAN join, uplink, downlink, GPS, link check, and callback delivery.

Key constraints:

- The bridge effectively supports one HTTP socket/request at a time. Serialize bridge calls.
- Responses can be slow. Use 25-30 second timeouts for bridge HTTP operations.
- Queue-full or temporary unavailable responses must be handled gracefully.
- Class C LoRaWAN is preferred when low-latency downlinks are needed.
- For EU868, use DR0 for OTAA join and DR4/DR5 as typical joined uplink rates when coverage allows.

Important firmware observations from live testing:

- `/status` is the primary endpoint for health and downlink detection in polling-style code.
- Status `7` means joined/idle and is the only reliable source for `fcntUp`, `drUp`, `maxUp`, and `devAddr`.
- Status `8` means a downlink payload is present. Treat `status == 8 && payload exists` as the downlink condition.
- Status `9` means an uplink was sent or the bridge is in the brief post-uplink phase.
- `fcntUp` and `drUp` can be `0` and unreliable in status `8` and `9` responses. Do not overwrite cached good values from those responses.
- `confirming` reflects the brief Class C RX1/post-uplink phase, not whether an uplink was confirmed or unconfirmed.
- `payload_type` must be preserved. Display `HEX` payloads as byte pairs and `ASCII` payloads as text.
- `length` is payload byte count, not string length.

Recent B100 clients in both ACAPs are callback-driven. Bridge status, receive/downlink, and GPS data are delivered through the callback endpoints above and reflected into the ACAP status store/UI.

---

## 5. Shared ACAP Architecture

Both apps are C ACAP applications using:

- `main.c` for settings, runtime threads, HTTP handlers, payload encoding, and decoder generation.
- `B100.c/h` for AI-B100 bridge HTTP calls and callback parsing.
- `ACAP.c/h` for Axis settings/status/events/HTTP wrapper behavior.
- `cJSON.c/h` for JSON parsing and generation.
- Bootstrap 5 and jQuery in the static HTML UI.
- Docker based ACAP SDK builds for `aarch64` and `armv7hf`.

Common HTTP/UI behavior:

- The app UI is served from `/local/aib100/`.
- `settings` is the persistent ACAP settings endpoint.
- `status` is the ACAP internal status store endpoint.
- `translator` downloads the current JavaScript LoRaWAN decoder.
- `b100_status`, `b100_receive`, and `b100_gps` are bridge callback endpoints.
- The LoRA Bridge page configures bridge IP, callbacks, join/restart/status/link-check, and bridge parameters.
- The LoRA Downlink page logs downlinks and enables/disables supported command bytes.
- The About page provides app/camera/bridge information and an installation report.

Management downlink ports shared by current apps:

| Port | Purpose |
| --- | --- |
| `100` | Actions, such as bridge restart, OTAA join, and reset/publish actions |
| `110` | Configuration, such as interval, data rate, and ADR |
| `120` | Information requests |
| `121` | Camera info response |
| `122` | Bridge info response |

---

## 6. AOA ACAP Current State

Path: `aoa/`

Friendly name: **AI-B100 AOA**

Package outputs:

- `AI-B100_AOA_1_2_0_aarch64.eap`
- `AI-B100_AOA_1_2_0_armv7hf.eap`

### AOA Use Cases

Counting:

- Reads AOA `CrosslineCounting` scenario totals.
- Publishes selected scenarios/classes on fixed LoRaWAN port `1`.
- Encodes values as little-endian unsigned 16-bit values and wraps modulo 65536.
- Uses AOA accumulated counts as the source of truth.

Occupancy:

- Creates, manages, and reads AOA `OccupancyInArea` scenarios.
- Publishes selected areas/classes on fixed LoRaWAN port `2`.
- Encodes one block per selected occupancy area: label count, value type, then selected label values.
- Value types are maximum, minimum, or average.

### AOA Settings

Current settings schema is version `4`.

- `transmission.counting.enabled`, `.intervalMinutes`, `.port`, `.classes`, `.scenarios`
- `transmission.occupancy.enabled`, `.intervalMinutes`, `.port`, `.value`, `.classes`, `.scenarios`
- Counting and Occupancy ports are fixed protocol metadata. Do not bring back user-selectable uplink ports.
- Default bridge/callback IPs should follow `192.168.1.250` and `192.168.1.200`.

### AOA UI Pages

| Page | Purpose |
| --- | --- |
| Publish | Enable Counting/Occupancy, set intervals, manual publish, recent uplinks, decoder download |
| Counting | Select AOA counting scenarios/classes and synchronize/reset counter state |
| Occupancy | Create/edit/delete OccupancyInArea scenarios and select areas/classes/value type |
| LoRA Bridge | Bridge configuration and bridge actions |
| LoRA Downlink | Downlink log and command enablement |
| GPS | GPS callback data |
| About | App/device/bridge report and decoder download |

### AOA Important Rules

- Keep Counting on port `1` and Occupancy on port `2`.
- Keep the generated decoder aligned with selected scenarios, classes, and occupancy value type.
- Do not reintroduce automatic text test payloads on port `7`.
- Preserve callback-driven bridge status/downlink/GPS behavior.

---

## 7. Radar ACAP Current State

Path: `radar/`

Friendly name: **AI-B100 Radar**

Package outputs:

- `AI-B100_Radar_1_2_0_aarch64.eap`
- `AI-B100_Radar_1_2_0_armv7hf.eap`

### Radar Use Cases

Counting:

- Reserved for fixed uplink port `1`.
- Not currently developed.
- Hidden from the Publish UI. The Publish page Counting card must remain hidden (`d-none`) unless the use case is intentionally completed.
- User-facing labels should say `Counting`, not `Area Balance`.

Occupancy:

- Active use case on fixed uplink port `2`.
- Publishes only Interval Maximum today: one byte `[selected_label_interval_max]`.
- Supports Human or Vehicle label selection.
- Can optionally restrict counting to an area of interest.
- The old Occupancy Area Balance mode is not ready and must remain hidden/removed from the UI, docs, decoder, and publish path.
- Old saved settings with `type: "areaBalance"` are coerced back to `maximum` by the backend.

Detection Alert:

- Active use case on fixed uplink port `3`.
- Supports Human or Vehicle label selection and optional area of interest.
- Has inactive heartbeat publishing and active-state publishing.
- Inactive payload is one byte `0x00`.
- Active payload is one byte `[selected_label_count]`.

### Radar UI Pages

| Page | Purpose |
| --- | --- |
| Publish | Enable/disable Occupancy and Detection Alert, show fixed ports/timers, manual publish, recent uplinks, decoder download |
| Occupancy | Select label, publish frequency, and optional area of interest for port 2 |
| Detection Alert | Select label, heartbeat, active interval, hold time, and optional area of interest for port 3 |
| Radar | Live stream and Radar Detection Sensitivity |
| LoRA Bridge | Bridge configuration and bridge actions |
| LoRA Downlink | Downlink log and command enablement |
| GPS | GPS callback data |
| About | App/device/bridge report and decoder download |

### Radar Payload Contract

| Port | Use case | Payload |
| --- | --- | --- |
| `1` | Counting | Reserved; hidden in UI until developed |
| `2` | Occupancy Interval Maximum | 1 byte: `[selected_label_interval_max]` |
| `3` | Detection Alert inactive | 1 byte: `[0x00]` |
| `3` | Detection Alert active | 1 byte: `[selected_label_count]` |

The static `radar/translator.js` and generated `/translator` output must match this contract. Do not re-add `Occupancy Area Balance` or a two-byte port 2 decoder branch while the use case is hidden.

### Radar Timer Model

Radar use-case timers must remain independent.

- Counting and Occupancy use explicit `g_counting_next_publish_time` and `g_occupancy_next_publish_time` in `radar/app/main.c`.
- Detection Alert owns its heartbeat/active timers inside `radar/app/alert.c` and exposes `Alert_Should_Publish`, `Alert_Next_Publish_Time`, `Alert_Mark_Published`, and `Alert_Reset_Timers`.
- Initial settings load calls `Load_Transmission_Config(transmission, 1)` to initialize schedules.
- Runtime settings updates must call `Load_Transmission_Config(data, 0)`.
- Runtime transmission reloads must compare old and new values and reset only the changed use case's timer.
- Toggling Detection Alert publishing must not reset Occupancy's next publish time.
- Alert timer resets should happen through `Alert_Reset_Timers()` and outside `g_publish_mutex`.
- Occupancy should reset its timer only when newly enabled, its interval changes, a downlink interval command changes it, its schedule is uninitialized, or an Occupancy publish succeeds.

### Radar Important Rules

- Keep Counting hidden until implemented.
- Keep Occupancy Area Balance hidden/removed until implemented.
- Keep Occupancy publishing one-byte Interval Maximum only.
- Keep Detection Alert timer state separate from Occupancy and Counting timer state.
- Use `g_publish_mutex` when accessing shared transmission settings and schedules.
- Keep the decoder and README synchronized with the actual payloads.

---

## 8. Build and Install

Docker is required for cross-building.

Build AOA:

```bash
cd /home/fred/development/ai-b100/aoa
./build.sh
```

Build Radar:

```bash
cd /home/fred/development/ai-b100/radar
./build.sh
```

Architecture selection:

- Use `aarch64` packages for ARTPEC-8 and ARTPEC-9 cameras.
- Use `armv7hf` packages for ARTPEC-7 cameras.

Install through the Axis camera Apps page or the app-specific `install.sh` helper. If an older differently named package is still installed, remove it before installing the current `aib100` package variant.

---

## 9. Documentation Rules

- Root [README.md](README.md) should remain a repository overview and point to [aoa/README.md](aoa/README.md), [radar/README.md](radar/README.md), and [DEPLOYMENT.md](DEPLOYMENT.md).
- [DEPLOYMENT.md](DEPLOYMENT.md) is the complete staging and field setup guide.
- Keep recommended IP addresses on the `192.168.1.x` scheme.
- Keep AOA and Radar READMEs app-specific and operational.
- If payloads, ports, settings schemas, or UI-exposed use cases change, update the README, decoder, and AGENTS.md together.

---

## 10. Editing Rules For Agents

1. Do not change the shared ACAP `appName` from `aib100` unless the user explicitly requests a package identity change.
2. Preserve the AI-B100 callback paths under `/local/aib100/`.
3. Serialize bridge HTTP access and keep long timeouts for B100 calls.
4. Do not trust `fcntUp`, `drUp`, `maxUp`, or `devAddr` from status `8` or `9` responses.
5. Preserve downlink detection as status `8` with a payload, and preserve payload type handling.
6. Keep LoRaWAN app ports fixed unless a protocol revision is explicitly requested.
7. For Radar, do not expose Counting or Occupancy Area Balance in the UI/decoder/docs while those use cases are marked not ready.
8. For Radar, do not couple Detection Alert settings updates to Occupancy timer resets.
9. When editing shared runtime state in C, hold the appropriate mutex (`g_publish_mutex`, counter/radar locks, or module-specific locks as used locally).
10. Rebuild the affected app with `./build.sh` after backend, packaged UI, manifest, settings, or decoder-generation changes.
11. Do not commit changes unless the user explicitly asks.
