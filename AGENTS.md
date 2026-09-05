# AGENTS.md - AI-B100 Axis ACAP Collection

This file is the authoritative reference for LLMs working on this repository. It describes the current repository shape, shared AI-B100 bridge behavior, active ACAP variants, payload contracts, build outputs, and editing rules that must be preserved.

---

## 1. Project Summary

This repository contains Axis ACAP applications that connect Axis camera analytics to LoRaWAN through an external AI-B100 bridge on the local LAN.

Current ACAP variants:

| Directory | Friendly name | Source analytics | Active uplinks |
| --- | --- | --- | --- |
| `aoa/` | AI-B100 AOA | Axis Object Analytics `CrosslineCounting` and `OccupancyInArea` | Counting on port 1, Occupancy on port 2, Presence on port 3 |
| `radar/` | AI-B100 Radar | Axis radar scene provider | Occupancy on port 2, Detection Alert on port 3, Speed on port 4; Counting is reserved on port 1 but hidden |
| `detectx/` | AI-B100 DetectX | Local validated YOLOv5 TFLite inference | 1-5 ordered label occupancy bytes on port 2 |

All variants intentionally use ACAP `appName` `aib100`. This keeps the camera URL path, settings store, and AI-B100 callback paths stable when switching variants. The variants are alternatives: install and run the one that matches the target camera and use case.

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
|-- common/                 # shared by every app; see section 2.1
|   |-- build.sh            # real build driver, staging overlay
|   |-- check-shared.sh     # fails the build on shadowed shared files
|   `-- app/
|       |-- ACAP.c/h, B100.c/h, cJSON.c/h, LICENSE
|       |-- html/           # bridge.html, gps.html
|       |-- html/css/       # app.css, bootstrap.min.css
|       |-- html/js/        # jquery, bootstrap, media-stream-player, toast.js, events.js, chrome.js
|       `-- settings/       # events.json
|-- aoa/
|   |-- build.sh / install.sh / Dockerfile / README.md
|   `-- app/
|       |-- main.c, counter.c/h, occupancy.c/h
|       |-- manifest.json, Makefile
|       |-- html/           # Publish, Counting, Occupancy, Bridge, Downlink, GPS, About
|       |-- settings/       # settings.json, state.json, subscriptions.json
|       `-- localdata/      # persisted counters
|-- radar/
|   |-- build.sh / install.sh / Dockerfile / README.md
|   |-- decoder/            # OTA translators (ports 130/132/133) and a generated uplink decoder sample
|   `-- app/
|       |-- main.c, counting.c/h, occupancy.c/h, alert.c/h, speed.c/h
|       |-- RadarDetection.*, radarscene.pb-c.*, protobuf-c.*
|       |-- manifest.json, Makefile
|       |-- html/           # Publish, Occupancy, Detection Alert, Radar, Bridge, Downlink, GPS, About
|       |-- settings/
|       `-- localdata/
|-- detectx/
|   |-- build.sh / install.sh / Dockerfile / README.md
|   |-- decoder/            # reference port 2 decoder
|   |-- tools/              # strict TFLite model validator/metadata generator
|   `-- app/
|       |-- main.c, Model.c/h, Video.c/h, imgprovider.c/h, labelparse.c/h, occupancy.c/h
|       |-- manifest.json, Makefile, model/
|       |-- html/           # Publish, Model, Detection, Downlink, About
|       |-- settings/
|       `-- localdata/
`-- images/
```

Built `.eap` packages land in each app directory and are gitignored. Historical files named `lorawan-counter/` or `AI-B100_AOA_Counter_0_5_0_*` are stale references and should not be reintroduced.

### 2.1 The common/ Overlay

Every app shares one copy of the platform layer. There is no per-app copy of `ACAP.c`, `B100.c`, `cJSON.c`, or the web assets.

The ACAP SDK requires a single flat source directory, so the build assembles one:

1. `common/app` is copied into the app's `.stage/`.
2. The app's own `app/` is copied on top, so app-specific files win.
3. Docker builds `.stage/`. It is gitignored and removed after a successful build.

Each app's `build.sh` is a wrapper around `common/build.sh`. The documented workflow is unchanged: `cd aoa && ./build.sh`.

Because the overlay lets an app-local file silently shadow a shared one, `common/check-shared.sh` runs first and fails the build if any app keeps its own copy of a file that exists in `common/app`. This is not hypothetical: AOA and Radar previously drifted for months, always with Radar ahead, and nothing caught it.

**Rule: shared files are edited in `common/app` only.** A fix belonging to the platform layer goes there so every app gets it. Never copy a shared file back into an app directory to make a local change.

### 2.1.1 The Nav Bar

The nav bar is not markup in the pages. `common/app/html/js/chrome.js` renders it from `NAV_PAGES`, which each app declares in its own `html/js/pages.js` as an array of `{ href, label }`. This is what lets `bridge.html` and `gps.html` be shared at all: their only app-specific content was the nav.

Every page therefore needs:

```html
<script src="js/jquery-3.7.1.min.js"></script>
<script src="js/pages.js"></script>
<script src="js/chrome.js"></script>
```

and an empty `<nav class="nav-bar" id="nav-bar"></nav>` in the header. Load order matters: `chrome.js` registers its ready handler before the page's own scripts, so the nav and `#lora-nav-dot` exist by the time page code runs.

To add, rename, hide, or reorder a page in the nav, edit that app's `pages.js`. Do not put `<a>` tags back into the pages. Radar's `counting.html` is deliberately absent from `NAV_PAGES` because the Counting use case must stay hidden.

Pages that are shared vs per-app:

| Page | Where | Why |
| --- | --- | --- |
| `bridge.html`, `gps.html` | `common/app/html/` | Pure platform, no app-specific content |
| `downlink.html` | per-app | Command tables differ; Radar has OTA ports 130/132/133 |
| `about.html` | per-app | AOA's installation report queries AXIS Object Analytics |
| `advanced.html` | per-app | AOA loads counters, Radar loads detection sensitivity |
| Use-case pages | per-app | `index.html`, `occupancy.html`, `aoa.html`, `alert.html`, `counting.html` |

### 2.2 Adding A New App

1. Create `<name>/` with `app/`, `Dockerfile`, `install.sh`, `README.md`.
2. Copy an existing app's `build.sh` wrapper verbatim.
3. In the `Dockerfile`, keep `COPY ./.stage .`.
4. Put only app-specific sources in `<name>/app/`: `main.c`, use-case modules, `manifest.json`, `Makefile`, `settings/settings.json`, `settings/subscriptions.json`, `localdata/`, and the app's own HTML pages.
5. Do not copy `ACAP.*`, `B100.*`, `cJSON.*`, `LICENSE`, `settings/events.json`, `html/bridge.html`, `html/gps.html`, or anything under `html/css` and `html/js`. The overlay supplies them, and `check-shared.sh` rejects local copies.
6. Add `<name>/app/html/js/pages.js` declaring `NAV_PAGES` for the app's nav, and give every page the three script tags and the empty `<nav id="nav-bar">` described in section 2.1.1.
7. List the app's own sources in the `Makefile` `OBJS1` line. The shared C files still need naming there (`ACAP.c cJSON.c B100.c`) because they are staged into the same flat directory.

The new app then tracks the latest shared platform automatically.

---

## 3. Deployment Defaults

Use the same static addressing at every installation unless the user explicitly asks otherwise.

| Device | Recommended IP | Subnet mask |
| --- | --- | --- |
| Laptop for commissioning | `192.168.1.10` | `255.255.255.0` |
| Axis camera or radar camera | `192.168.1.200` | `255.255.255.0` |
| AI-B100 bridge | `192.168.1.250` | `255.255.255.0` |

Important defaults:

- Bridge GUI port: `80`
- Bridge API port for AOA 2.0.0 and AI-B100 firmware 2.0.0+: `81`
- ACAP callback IP: camera/radar address, usually `192.168.1.200`
- Recommended bridge GUI login: `admin` / `lorabridge`
- Recommended bridge API and camera callback login: `lorabridge` / `lorabridge`
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
- Status `0` (firmware 2.x ready) and status `7` (joined) are reliable joined states for `fcntUp`, data rate, `maxUp`, and `devAddr`.
- Status `8` means a downlink payload is present. Treat `status == 8 && payload exists` as the downlink condition.
- Status `9` means an uplink was sent or the bridge is in the brief post-uplink phase.
- `fcntUp` and `drUp` can be `0` and unreliable in status `8` and `9` responses. Do not overwrite cached good values from those responses.
- `confirming` reflects the brief Class C RX1/post-uplink phase, not whether an uplink was confirmed or unconfirmed.
- `payload_type` must be preserved. Display `HEX` payloads as byte pairs and `ASCII` payloads as text.
- `length` is payload byte count, not string length.

Recent B100 clients in all ACAPs are callback-driven. Bridge status, receive/downlink, and GPS data are delivered through the callback endpoints above and reflected into the ACAP status store/UI.

---

## 5. Shared ACAP Architecture

Both apps are C ACAP applications using:

- `main.c` for settings, runtime threads, HTTP handlers, payload encoding, and decoder generation. Per-app.
- `B100.c/h` for AI-B100 bridge HTTP calls and callback parsing. Shared, in `common/app`.
- `ACAP.c/h` for Axis settings/status/events/HTTP wrapper behavior. Shared, in `common/app`.
- `cJSON.c/h` for JSON parsing and generation. Shared, in `common/app`.
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

- `AI-B100_AOA_2_0_0_aarch64.eap`
- `AI-B100_AOA_2_0_0_armv7hf.eap`

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

Presence Alert:

- Uses independent AOA `motion` scenes containing an `individualTimeInArea` condition; it does not reuse or modify Occupancy settings.
- Creates, edits, and deletes alerts through the Presence Alert page, combining AOA names, classes, detection times, and draggable include-area polygons with per-alert clear transition times and schedules.
- Uses the AOA condition `time` as the detection delay before the alert goes high.
- Holds each active alert through its configured clear transition time and cancels the pending clear when detection returns.
- Supports an independent Always or local 24-hour activation window per alert, including overnight schedules such as `18:00` to `06:00`.
- Evaluates all configured alert rules continuously; `transmission.presence.enabled` controls LoRa publishing only.
- Publishes only effective state transitions on fixed LoRaWAN port `3`; there is no periodic heartbeat.
- Encodes one byte per configured alert in settings order, where `0` is clear and `1` is alert. There is no count prefix.

### AOA Settings

Current settings schema is version `8`.

- `transmission.counting.enabled`, `.intervalMinutes`, `.port`, `.classes`, `.scenarios`
- `transmission.occupancy.enabled`, `.intervalMinutes`, `.port`, `.value`, `.classes`, `.scenarios`
- `transmission.presence.enabled`, `.port`, `.scenarios`; each scenario owns `.cooldownSeconds` and `.schedule`, while detection dwell time is owned by its AOA `individualTimeInArea` condition
- `b100.port`, `.apiDigestUser`, and `.apiDigestPassword` configure the bridge API. Fresh installs use port `81` and `lorabridge` / `lorabridge`; migrated installations preserve their saved API port.
- Counting, Occupancy, and Presence Alert ports are fixed protocol metadata. Do not bring back user-selectable uplink ports.
- Default bridge/callback IPs should follow `192.168.1.250` and `192.168.1.200`.

### AOA UI Pages

| Page | Purpose |
| --- | --- |
| Publish | Enable Counting/Occupancy/Presence Alert, set periodic intervals, manual publish, recent uplinks, decoder download |
| Counting | Select AOA counting scenarios/classes and synchronize/reset counter state |
| Occupancy | Create/edit/delete OccupancyInArea scenarios and configure Occupancy values |
| Presence Alert | Add/edit/delete alerts, configure classes/detection/clear times, drag include areas over live video, set per-alert schedules, and view live state |
| LoRA Bridge | Bridge configuration and bridge actions |
| LoRA Downlink | Downlink log and command enablement |
| GPS | GPS callback data |
| About | App/device/bridge report and decoder download |

### AOA Important Rules

- Keep Counting on port `1`, Occupancy on port `2`, and Presence Alert on port `3`.
- Keep the generated decoder aligned with selected scenarios, classes, occupancy value type, and Presence Alert scene order.
- Do not reintroduce automatic text test payloads on port `7`.
- Preserve callback-driven bridge status/downlink/GPS behavior.

---

## 7. Radar ACAP Current State

Path: `radar/`

Friendly name: **AI-B100 Radar**

Package outputs:

- `AI-B100_Radar_2_0_0_aarch64.eap`
- `AI-B100_Radar_2_0_0_armv7hf.eap`

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
- Active payload is one byte `[selected_label_active_max]`.

Speed:

- Active use case on fixed uplink port `4`.
- Publishes a five-byte summary of vehicle speeds since the previous Speed uplink:
  `[vehicles, speeding, maximum, average, minimum]`, all `uint8`.
- Speeds are whole numbers in the configured output unit, `kmh` or `mph`. The radar driver already
  converts velocity to km/h, so the module keeps km/h internally and converts at encode time.
- A vehicle is measured only when its track is lost, using the maximum speed it reached inside the
  area of interest. Maximum, average, and minimum are all taken over those per-vehicle maxima.
- Humans are ignored. Tracks are discarded when they had no measuring point in the area of interest,
  when the in-area maximum was below 10 km/h, or when the straight-line birth-to-loss displacement
  was under 250 units of the 0-1000 space.
- An interval with no qualifying vehicles still publishes five zero bytes.
- The area of interest is a polygon of at most 8 points in the UI.

### Radar UI Pages

| Page | Purpose |
| --- | --- |
| Publish | Enable/disable Occupancy and Detection Alert, show fixed ports/timers, manual publish, recent uplinks, decoder download |
| Occupancy | Select label, publish frequency, and optional area of interest for port 2 |
| Detection Alert | Select label, heartbeat, active interval, hold time, and optional area of interest for port 3 |
| Speed | Select output unit, speed limit, and optional area of interest for port 4, and view the last published summary |
| Radar | Live stream, Radar Detection Sensitivity, and Radar OTA encoder/decoder for port 130 |
| LoRA Bridge | Bridge configuration and bridge actions |
| LoRA Downlink | Downlink log and command enablement |
| GPS | GPS callback data |
| About | App/device/bridge report and decoder download |

### Radar OTA Configuration

Radar OTA configuration uses fixed port `130` for both requests and responses. It is separate from Occupancy OTA port `132` and Detection Alert OTA port `133`.

Commands:

| Command | Purpose |
| --- | --- |
| `0x01` | GET_CONFIG request |
| `0x81` | GET_CONFIG response |
| `0x02` | SET_CONFIG request |
| `0x82` | SET_CONFIG ACK/NACK |
| `0x03` | GET_CAPS request |
| `0x83` | GET_CAPS response |

The Radar config body is 8 bytes: `version`, `fieldMask` as uint16 BE, `detectionSensitivity`, three reserved zero bytes, and CRC8 over the first seven bytes. Field mask `0x0001` is Radar Detection Sensitivity, encoded as `1=low`, `2=medium`, `3=high`. Keep the Radar page modal and generated `radar_ota_translator` endpoint aligned with this format when adding future Radar-tab fields.

### Radar Payload Contract

| Port | Use case | Payload |
| --- | --- | --- |
| `1` | Counting | Reserved; hidden in UI until developed |
| `2` | Occupancy Interval Maximum | 1 byte: `[selected_label_interval_max]` |
| `3` | Detection Alert inactive | 1 byte: `[0x00]` |
| `3` | Detection Alert active | 1 byte: `[selected_label_active_max]` |
| `4` | Speed | 5 bytes: `[vehicles, speeding, maximum, average, minimum]` |

The reference decoders in `radar/decoder/` and the generated `/translator` output must match this contract. Do not re-add `Occupancy Area Balance` or a two-byte port 2 decoder branch while the use case is hidden.

### Radar Timer Model

Radar use-case timers must remain independent.

- Counting and Occupancy use explicit `g_counting_next_publish_time` and `g_occupancy_next_publish_time` in `radar/app/main.c`.
- Speed uses `g_speed_next_publish_time` and resets only when it is newly enabled, its interval changes, its schedule is uninitialized, or a Speed publish succeeds.
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

## 8. DetectX ACAP Current State

Path: `detectx/`

Friendly name: **AI-B100 DetectX**

Package output: `AI-B100_DetectX_2_0_0_aarch64.eap` (ARTPEC-8/9 only)

- Runs one validated YOLOv5 TFLite model locally and feeds every post-filter frame, including empty frames, into the occupancy module.
- Selects 1-5 unique active-model labels in model order.
- Publishes one saturating uint8 interval maximum per selected label on fixed port `2`.
- Resets interval maxima when a publish begins, records frames arriving while the send is in flight into the new interval, and merges detached maxima back if the bridge send fails.
- Shows a live next-publish countdown and current per-label interval maxima on Publish; the maxima reset at the publish boundary.
- Generates a copyable/downloadable uplink decoder plus DetectX-specific OTA encoder and decoder JavaScript. All three read the persisted `transmission.occupancy.selectedLabels` order and embed an explicit `[{ byte, label }]` mapping; decoded port 2 JSON returns that mapping under `configuration.occupancy.labels`. Current OTA services are actions on port 100, configuration on port 110, information requests on port 120, and information replies on ports 121/122; keep their service registries extensible for future DetectX use cases.
- Uses one model, label file, and metadata sidecar fixed at build time; operators cannot upload or replace the model.
- Validates the bundled model with `detectx/tools/validate_model.py` and generates its metadata before packaging.
- Supports only uint8 NHWC `[1,H,W,3]` input and one YOLOv5 `[1,N,5+C]` float32/int8/uint8 output with valid per-tensor output quantization when quantized.
- NMS is class-aware and confidence-ordered through explicit `classIndex` values.
- Uses settings schema version `3`; `detection.captureMode` selects `balanced`, `crop`, or `letterbox`.
- Balanced captures a 4:3 frame and stretches it to the square model input, Center-cropping uses VDO `image.fit=crop`, and Letterbox uses VDO `image.fit=scale`.
- Excludes MQTT, crops, SD capture, certificates, events, and remote model OTA.
- Uses exactly four navigation items: Publish, Occupancy, LoRA Bridge, and About. GPS and LoRA Downlink remain backend capabilities but are hidden from DetectX navigation.
- Configures live detections, capture mode, the draggable area of interest, label selection, confidence, and overlap handling together on the Occupancy page.
- Uses settings `variant: "detectx"`; switching from another `aib100` variant preserves shared bridge settings but replaces incompatible transmission settings.

Keep DetectX aarch64-only and keep its decoder synchronized with selected label order and exact payload length.

---

## 9. Build and Install

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

Build DetectX:

```bash
cd /home/fred/development/ai-b100/detectx
./build.sh
```

Architecture selection:

- Use `aarch64` packages for ARTPEC-8 and ARTPEC-9 cameras.
- Use `armv7hf` packages for ARTPEC-7 cameras.

Install through the Axis camera Apps page or the app-specific `install.sh` helper. If an older differently named package is still installed, remove it before installing the current `aib100` package variant.

---

## 10. Documentation Rules

- Root [README.md](README.md) should remain a repository overview and point to [aoa/README.md](aoa/README.md), [radar/README.md](radar/README.md), [detectx/README.md](detectx/README.md), and [DEPLOYMENT.md](DEPLOYMENT.md).
- [DEPLOYMENT.md](DEPLOYMENT.md) is the complete staging and field setup guide.
- Keep recommended IP addresses on the `192.168.1.x` scheme.
- Keep AOA and Radar READMEs app-specific and operational.
- If payloads, ports, settings schemas, or UI-exposed use cases change, update the README, decoder, and AGENTS.md together.

---

## 11. Editing Rules For Agents

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
11. Edit shared files in `common/app` only, and never create an app-local copy of one. A change to `ACAP.*`, `B100.*`, `cJSON.*`, or a shared web asset affects every app, so rebuild and check all of them, not just the one being worked on.
12. Do not commit changes unless the user explicitly asks.
13. Keep the DetectX model fixed at build time, validate it against the documented YOLOv5 tensor contract, and generate matching per-model metadata; do not add runtime uploads or claim arbitrary TFLite compatibility.
