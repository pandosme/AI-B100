# AI-B100 Axis ACAP Collection

This repository contains Axis ACAP applications that connect Axis camera analytics to LoRaWAN through an AI-B100 bridge. The top-level README gives the repository overview. The app-specific README files explain how each ACAP works and how to operate it:

- [aoa/README.md](aoa/README.md) - AI-B100 AOA for Axis Object Analytics Counting, Occupancy, and Presence
- [radar/README.md](radar/README.md) - AI-B100 Radar for radar Occupancy, Detection Alert, and Speed, with Counting reserved for future use
- [detectx/README.md](detectx/README.md) - AI-B100 DetectX for local YOLOv5 inference and label occupancy

For complete bench staging and field setup, including static IP addressing, camera accounts, bridge callbacks, LoRaWAN registration, and end-to-end verification, follow [DEPLOYMENT.md](DEPLOYMENT.md). The recommended static addresses are camera/radar `192.168.1.200` and AI-B100 bridge `192.168.1.250`.

The camera runs the analytics and ACAP locally. The ACAP sends only compact numeric payloads to the AI-B100 bridge over the local LAN; no images or video are sent over LoRaWAN.

## Version 2.0.0 Bridge Setup

AI-B100 firmware 2.0.0 separates the bridge web interface from its authenticated HTTP API:

| Interface | Port | Authentication | Recommended credentials |
| --- | --- | --- | --- |
| Bridge web interface | `80` | Fixed user `admin` | Password `lorabridge` |
| Bridge HTTP API | `81` | HTTP Digest | `lorabridge` / `lorabridge` |
| Camera callback endpoints | `80` | Axis Viewer account | `lorabridge` / `lorabridge` |

Use the same `lorabridge` username and password for the bridge HTTP API account and the Axis camera Viewer account. This gives the ACAP one consistent service credential in both directions: the camera authenticates to the bridge API on port `81`, and the bridge authenticates its callbacks to the camera on port `80`. The bridge web interface still uses its fixed `admin` username.

Fresh ACAP 2.0.0 installations default to API port `81` and these service credentials. Existing installations retain their saved API port; bridges running firmware earlier than 2.0.0 remain on the unauthenticated API on port `80`. See [DEPLOYMENT.md](DEPLOYMENT.md) for the complete setup.

---

## What The System Does

The AI-B100 is an Ethernet-to-LoRaWAN bridge from AI Embedded Nordic AB. It exposes a local HTTP API to the camera and handles LoRaWAN join, uplink, downlink, callback, GPS, and link-quality reporting.

```text
Axis camera + ACAP
       |
       | HTTP on local PoE/LAN network
       v
AI-B100 LoRaWAN bridge
       |
       | LoRaWAN uplink/downlink
       v
LoRaWAN gateway / network server
       |
       v
Backend, dashboard, TTN, ChirpStack, Node-RED, or private LNS
```

Typical deployments use an Axis camera, an AI-B100 or AI-B100-POE bridge, a local PoE switch, and a LoRaWAN gateway or existing LoRaWAN coverage. The site does not need internet access for the camera itself once the bridge is joined to the LoRaWAN network.

---

## Available ACAPs

| Directory | ACAP package | Source analytics | Main LoRaWAN uplinks | Read more |
| --- | --- | --- | --- | --- |
| [aoa/](aoa/) | AI-B100 AOA | Axis Object Analytics `CrosslineCounting` and `OccupancyInArea` | Counting on port 1, Occupancy on port 2, Presence on port 3 | [aoa/README.md](aoa/README.md) |
| [radar/](radar/) | AI-B100 Radar | Axis radar scene provider | Counting reserved on port 1, Occupancy on port 2, Detection Alert on port 3, Speed on port 4 | [radar/README.md](radar/README.md) |
| [detectx/](detectx/) | AI-B100 DetectX | Local YOLOv5 TFLite inference | 1-5 ordered label occupancy bytes on port 2 | [detectx/README.md](detectx/README.md) |

All variants use ACAP `appName` `aib100`. This keeps the camera URL path, settings store, and AI-B100 callback paths stable when switching variants. The packages are alternatives: install and run the one that matches the camera and use case.

---

## Shared Platform Behavior

All ACAP variants share the same AI-B100 integration pattern:

- The app UI is served from the Axis camera under `/local/aib100/`.
- Persistent settings are stored by the ACAP on the camera.
- For bridge firmware 2.0.0 and later, the ACAP uses HTTP Digest authentication for bridge API calls on port `81`; the bridge web interface remains on port `80`.
- Bridge callbacks use the Axis camera Viewer account. The recommended service credentials on both devices are `lorabridge` / `lorabridge`.
- The bridge is configured with short HTTP callback paths:
  - `/local/aib100/b100_status`
  - `/local/aib100/b100_receive`
  - `/local/aib100/b100_gps`
- Uplink payloads use fixed application ports so decoders can be simple and deterministic.
- Management downlinks use reserved ports: actions on `100`, configuration on `110`, information requests on `120`, and information replies on `121` and `122`.
- The LoRA Bridge page in each app handles bridge IP, callback address, callback credentials, join, restart, status request, and link check.
- The LoRA Downlink page logs received downlinks and lets supported command bytes be enabled or disabled.
- The GPS page shows GPS callback data when the bridge provides it.
- The About page provides app/camera information and an installation report for field documentation.

---

## Which App To Use

Use [AI-B100 AOA](aoa/README.md) when the camera uses Axis Object Analytics scenarios. It is the right choice for optical line counting, object-class counts, and AOA occupancy areas.

Use [AI-B100 Radar](radar/README.md) on radar-equipped Axis cameras when you want radar-based occupancy, active/inactive detection alerts, or speed summaries. Radar Counting remains reserved and should not be enabled for deployment. Radar is useful where lighting, privacy, or weather conditions make video analytics less suitable.

Use [AI-B100 DetectX](detectx/README.md) on ARTPEC-8/9 cameras when a validated build-time YOLOv5 model should drive occupancy directly. It publishes one byte per selected model label in deterministic model order.

---

## Operating Flow

1. Stage the camera, bridge, static IP addresses, shared `lorabridge` service credentials, and LoRaWAN registration using the complete setup guide in [DEPLOYMENT.md](DEPLOYMENT.md).
2. Install the matching `.eap` package for the camera architecture from [aoa/](aoa/), [radar/](radar/), or [detectx/](detectx/).
3. Start the ACAP from the Axis camera Apps page and open its web UI.
4. Configure bridge API port `81`, the bridge API credentials, and camera callback credentials on the LoRA Bridge page, then request status or join.
5. Configure the analytics use cases or DetectX occupancy area, labels, confidence, and overlap handling in the app pages.
6. Enable the desired publish streams on the Publish page.
7. Download the JavaScript translator/decoder from the Publish or About page and use it in the LoRaWAN network server.
8. Verify end-to-end behavior with manual publish, scheduled publish, downlink commands, signal quality, and the installation report.

Detailed operating instructions are in the app README files:

- [AOA operation guide](aoa/README.md)
- [Radar operation guide](radar/README.md)
- [DetectX operation guide](detectx/README.md)

---

## Build Packages

Docker is required for the ACAP cross-builds.

```bash
cd aoa
./build.sh

cd ../radar
./build.sh

cd ../detectx
./build.sh
```

Each build stages [common/app](common/app) and then the app's own `app/` directory on top, so all variants compile against the same platform layer. Shared files are edited in `common/app` only; the build refuses to run if an app keeps its own copy of one.

Current package outputs:

| App | ARTPEC-8/9 | ARTPEC-7 |
| --- | --- | --- |
| AOA | `AI-B100_AOA_2_0_0_aarch64.eap` | `AI-B100_AOA_2_0_0_armv7hf.eap` |
| Radar | `AI-B100_Radar_2_0_0_aarch64.eap` | `AI-B100_Radar_2_0_0_armv7hf.eap` |
| DetectX | `AI-B100_DetectX_2_0_0_aarch64.eap` | Not supported |

Use `aarch64` for ARTPEC-8 and ARTPEC-9 cameras. Use `armv7hf` for ARTPEC-7 cameras.

---

## Repository Layout

| Path | Purpose |
| --- | --- |
| [common/](common/) | Platform layer shared by every app: `ACAP.*`, `B100.*`, `cJSON.*`, web assets, and the build driver |
| [aoa/](aoa/) | AI-B100 AOA app-specific source, UI, settings, build script, and packages |
| [radar/](radar/) | AI-B100 Radar app-specific source, UI, settings, build script, decoder, and packages |
| [detectx/](detectx/) | AI-B100 DetectX inference, build-time model validation, occupancy, UI, decoder, and tests |
| [DEPLOYMENT.md](DEPLOYMENT.md) | Shared field deployment guide |
| [Customization.md](Customization.md) | Notes for cloning and adapting an ACAP variant |
| [http_api.md](http_api.md) | AI-B100 HTTP API notes |
| [images/](images/) | Shared documentation images |

---

## LoRaWAN Notes

- Use Class C when low-latency downlink commands are required.
- Verify RSSI and SNR at the installation site before final mounting.
- Keep scheduled publish intervals conservative. A 10 to 15 minute interval is a typical safe starting point for EU868 duty-cycle limits.
- Download a fresh decoder after changing enabled streams, labels, scenarios, areas, or occupancy modes.

---

## License

MIT - see [common/app/LICENSE](common/app/LICENSE).
