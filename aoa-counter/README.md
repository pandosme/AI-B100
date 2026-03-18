# AI-B100 AOA Counter

An [ACAP](https://www.axis.com/developer-community/acap) for Axis cameras that reads **Axis Object Analytics (AOA)** CrosslineCounting events and publishes aggregated people and vehicle counts over LoRaWAN via an [AI-B100 bridge](../README.md).

The camera performs all inference locally. Only numeric counts leave the device — no images, no video.

---

## Requirements

- Axis camera with ACAP v4+ support
- **Axis Object Analytics** installed and configured on the camera with at least one CrosslineCounting scenario
- **AI-B100 LoRaWAN bridge** on the same LAN — see the [project README](../README.md) for hardware setup
- A LoRaWAN network server (TTN, Chirpstack, or private LNS) with the AI-B100 registered

---

## Installation

### 1. Configure Axis Object Analytics

In the camera web UI, open **Axis Object Analytics** and create one or more **CrosslineCounting** scenarios with counting lines drawn in the scene. The ACAP automatically discovers all scenarios and tracks them independently.

### 2. Install the ACAP

Download the `.eap` file matching your camera's architecture:
- `AI-B100_AOA_Counter_*_aarch64.eap` — ARTPEC-8 / CV25 cameras
- `AI-B100_AOA_Counter_*_armv7hf.eap` — ARTPEC-6/7 cameras

In the camera web UI: **Settings → Apps → Add app** — upload the `.eap` and start it.

### 3. Configure the ACAP

Open the ACAP from **Settings → Apps → AI-B100 AOA Counter → Open**:

- Set the **AI-B100 IP address**
- Verify the bridge is connected and joined — the nav bar shows a green **LoRa** dot when joined
- Set the **transmission interval** (minimum 10 minutes recommended)
- Enable/disable individual object classes (human, car, bike, bus, truck, other)
- Click **Join Network** if the bridge has not yet joined the LoRaWAN network

---

## Configuration

```json
{
  "b100": {
    "ip": "10.13.8.47",
    "port": 80,
    "timeout": 30
  },
  "lorawan": {
    "port": 10,
    "confirmed": false,
    "dataRate": 4,
    "autoJoin": true
  },
  "transmission": {
    "intervalMinutes": 15,
    "enabled": true,
    "classes": {
      "human": true, "car": true, "bike": true,
      "bus": true, "truck": true, "other": true
    }
  },
  "polling": {
    "downlinkIntervalSeconds": 30,
    "healthCheckIntervalSeconds": 60
  }
}
```

---

## Uplink Payload

Counts are encoded as compact binary and sent over LoRaWAN. JavaScript payload decoders are provided:

- [`ttn-decoder.js`](axis_AoA_and_AIEmb.js) — TTN Payload Formatter / Node-RED Function node

The decoder is also downloadable directly from the ACAP web UI (**About** page → **Download payload decoder**).

---

## Downlink Commands

The ACAP listens for downlink messages from the network server and acts on them. Commands are short binary payloads.

### Port 10 — Actions

| Byte | Action |
|------|--------|
| `0x01` | Restart the AI-B100 bridge |
| `0x02` | Initiate a new LoRaWAN OTAA join |
| `0x03` | Reset all counters to zero and publish immediately |

### Port 11 — Configuration

Two-byte commands: `[command byte, value byte]`

| Byte 0 | Byte 1 | Action |
|--------|--------|--------|
| `0x01` | `5–60` | Set transmission interval (minutes) |
| `0x02` | `0–5`  | Set fixed data rate (DR0 SF12 … DR5 SF7) |
| `0x03` | `0/1`  | Disable / enable Adaptive Data Rate (ADR) |

### Port 12 — Information Requests

| Byte | Reply port | Response |
|------|-----------|----------|
| `0x01` | 5 | Camera info: `model,serial,firmware,uptime_days` |
| `0x02` | 6 | Bridge info: `hw_version,sw_version[,DR,maxPayload]` |

### Port 13 — Test

| Byte | Action |
|------|--------|
| `0x01` | Send `Hello` test message on port 7 |

---

## Web UI Pages

| Page | Description |
|------|-------------|
| **Counters** | Transmission settings, live counter cards, Publish Now button |
| **AOA** | Active AOA scenarios |
| **LoRA Bridge** | Bridge connection, hardware/software version, join, polling config |
| **LoRA Downlink** | Live log of received downlinks; enable/disable commands |
| **About** | App version, payload decoder download |

---

## Building from Source

Requires Docker with the [Axis ACAP SDK](https://github.com/AxisCommunications/acap-native-sdk) image.

```bash
cd aoa-counter
./build.sh
```

Output:
- `AI-B100_AOA_Counter_<version>_aarch64.eap`
- `AI-B100_AOA_Counter_<version>_armv7hf.eap`

---

## License

MIT — see [app/LICENSE](app/LICENSE).


> **This ACAP is useless without the AI-B100.**
> It requires an [AI-B100 LoRaWAN bridge](https://www.ai-embedded.se) connected on the same LAN as the camera. The AI-B100 is the hardware that sends and receives LoRaWAN radio packets. Without it there is no LoRa connectivity.

---

## Why This Exists

Standard IP-connected cameras require internet access or a cloud subscription to forward data off-site. In many deployments — remote field sites, smart city installations, GDPR-sensitive locations — that is either unavailable, undesirable, or prohibited.

This solution replaces the IP backhaul entirely:

- The camera runs AOA locally and counts objects in the scene
- The ACAP reads those counts and forwards them **over the local LAN** to the AI-B100 bridge
- The AI-B100 transmits compact binary payloads via **LoRaWAN radio** to any standard LoRaWAN network (TTN, Chirpstack, private LNS)
- No images, no video, no internet connection at the camera site — only numeric counts leave the device

---

## Typical Use Cases

| Scenario | Why LoRaWAN |
|----------|-------------|
| Smart city traffic / pedestrian counting | Existing city LoRaWAN network; no IP backhaul at poles |
| Retail / public space footfall | Aggregated counts only; GDPR-friendly |
| Remote site monitoring | LoRaWAN is the only available connectivity |
| GDPR-sensitive deployments | Only numeric counters transmitted; no images |

---

## Required Hardware

| Component | Description |
|-----------|-------------|
| **Axis camera** | Any model supporting ACAP v4+ and Axis Object Analytics |
| **[AI-B100 LoRaWAN bridge](https://www.ai-embedded.se)** | Ethernet-to-LoRaWAN bridge by AI Embedded Nordic AB |
| **PoE switch** | Powers camera and PoE splitter; provides commissioning port (e.g. Netgear GS305EPP) |
| **PoE splitter (ETH + USB-C 5 V)** | Splits PoE into Ethernet data and USB-C power for the AI-B100 |

**Optional variants of the AI-B100:**
- **AI-B100-POE** — PoE-powered; eliminates the need for a separate PoE splitter
- **AI-B100-ANT** — External antenna connector for extended RF range
- **AI-B100-POE-ANT** — Both

Contact AI Embedded Nordic: **ai-b100@ai-embedded.se** | **www.ai-embedded.se**

---

## System Overview

```
┌─────────────────────────────────────────────┐
│  Local LAN (PoE Switch)                     │
│                                             │
│  ┌────────────┐   HTTP/REST   ┌──────────┐  │
│  │ Axis Camera│◄─────────────►│ AI-B100  │  │
│  │ (AOA+ACAP) │               │ Bridge   │  │
│  └────────────┘               └────┬─────┘  │
│                                    │ LoRa   │
└────────────────────────────────────┼────────┘
                                     │
                              LoRaWAN Gateway
                                     │
                        LoRaWAN Network Server
                        (TTN / Chirpstack / etc.)
                                     │
                          Your backend / dashboard
```

The camera and AI-B100 communicate entirely over the local LAN via HTTP. No MQTT broker is needed. The ACAP polls the AI-B100 for downlink commands and sends counter payloads on a configurable interval.

---

## Installation

### 1. Hardware Setup

Connect everything to the PoE switch:
- Camera → PoE switch (camera is PoE-powered)
- AI-B100 → PoE splitter → PoE switch (USB-C powers the bridge; Ethernet delivers data)

### 2. Configure the AI-B100

Using the AI-B100 built-in web UI (`http://<bridge-ip>`):
- Set LAN address (DHCP or static)
- Set LoRaWAN keys: **AppEUI**, **DevEUI**, **AppKey**
- Set MQTT broker (not required for this integration — the ACAP uses the HTTP API directly)

### 3. Provision on your LoRaWAN Network Server

Register the device on TTN, Chirpstack, or your private LNS using the same keys configured on the bridge. The bridge uses **OTAA** (Over-the-Air Activation), **Class C**, **EU868** by default.

### 4. Configure Axis Object Analytics

In the camera's web UI, open **Axis Object Analytics** and create one or more **CrosslineCounting** scenarios with counting lines drawn in the scene. The ACAP will automatically discover and track all scenarios.

### 5. Install the ACAP

Download the appropriate `.eap` file for your camera architecture:
- `AI-B100_AOA_Counter_*_aarch64.eap` — modern ARTPEC-8 / CV25 cameras
- `AI-B100_AOA_Counter_*_armv7hf.eap` — older ARTPEC-6/7 cameras

In the camera web UI: **Settings → Apps → Add app** → upload the `.eap` file and start it.

### 6. Configure the ACAP

Open the ACAP settings page from **Settings → Apps → AI-B100 AOA Counter → Open**:
- Set the **AI-B100 IP address**
- Set the **LoRaWAN uplink port** (default: 10)
- Set the **transmission interval** (minimum 10 minutes recommended — respect LoRaWAN duty cycle)
- Enable/disable individual object classes (human, car, bike, bus, truck, other)
- Click **Join Network** if the bridge is not yet joined

---

## Configuration Reference

```json
{
  "b100": {
    "ip": "10.13.8.47",
    "port": 80,
    "timeout": 30
  },
  "lorawan": {
    "port": 10,
    "confirmed": false,
    "dataRate": 4,
    "autoJoin": true
  },
  "transmission": {
    "intervalMinutes": 15,
    "enabled": true,
    "classes": {
      "human": true, "car": true, "bike": true,
      "bus": true, "truck": true, "other": true
    }
  },
  "polling": {
    "downlinkIntervalSeconds": 30,
    "healthCheckIntervalSeconds": 60
  }
}
```

---

## Uplink Payload

Counters are encoded as compact binary and sent via the AI-B100 `/send` endpoint. A JavaScript payload decoder for TTN and Node-RED is included:

- [`ttn-decoder.js`](../ttn-decoder.js) — TTN Payload Formatter
- [`ttn-decoder-nodered.js`](../ttn-decoder-nodered.js) — Node-RED Function node

The decoder is also available for download directly from the ACAP web UI (**About** page).

---

## Downlink Commands

The ACAP listens for downlink messages and acts on them. Each downlink is a short binary payload sent from your LoRaWAN network server or application.

### Port 10 — Actions

| Byte | Action |
|------|--------|
| `0x01` | Restart the AI-B100 bridge |
| `0x02` | Initiate a new LoRaWAN OTAA join |
| `0x03` | Reset all counters to zero and publish immediately |

### Port 11 — Configuration

All port 11 commands are **2 bytes**: `[command, value]`

| Byte 0 | Byte 1 | Action |
|--------|--------|--------|
| `0x01` | `5–60` | Set transmission interval in minutes |
| `0x02` | `0–5`  | Set fixed data rate (DR0=SF12 … DR5=SF7) |
| `0x03` | `0/1`  | Disable/enable Adaptive Data Rate (ADR) |

### Port 12 — Information Requests

| Byte | Reply port | Response payload |
|------|-----------|-----------------|
| `0x01` | 5 | Camera info: `model,serial,firmware,uptime_days` |
| `0x02` | 6 | Bridge info: `hw_version,sw_version[,DR,maxPayload]` |

### Port 13 — Test

| Byte | Action |
|------|--------|
| `0x01` | Send `Hello` test message on port 7 |

---

## LoRaWAN Considerations

- **Duty cycle** — LoRaWAN imposes regional duty cycle limits. Keep the transmission interval at **10–15 minutes minimum**. The ACAP enforces a minimum of 5 minutes.
- **Payload size** — The binary counter payload is typically 20–50 bytes depending on how many scenarios are active. This fits comfortably at all data rates including DR0 (SF12, 51-byte limit).
- **Class C** — The AI-B100 operates in Class C (always listening). Downlink commands from the network server are received within seconds regardless of uplink schedule.
- **Coverage** — Verify RSSI and SNR at the installation site before finalizing the camera mount. The ACAP reports signal quality on the LoRA Bridge page.

---

## Web UI Pages

| Page | Description |
|------|-------------|
| **Counters** | Transmission settings, live counter values, Publish Now button |
| **AOA** | View active AOA scenarios |
| **LoRA Bridge** | Bridge connection status, hardware/software version, join button, polling config |
| **LoRA Downlink** | Live log of received downlink messages; enable/disable commands |
| **About** | App version, payload decoder download |

---

## Building from Source

```bash
# Requires Docker and the Axis ACAP SDK image
cd lorawan-counter
./build.sh
```

Output:
- `AI-B100_AOA_Counter_<version>_aarch64.eap`
- `AI-B100_AOA_Counter_<version>_armv7hf.eap`

### Dependencies (resolved by SDK container)
- `glib-2.0`, `gio-2.0` — GLib/GIO
- `axevent` — Axis event system
- `axparameter` — Axis parameter storage
- `fcgi` — FastCGI HTTP server
- `libcurl` — HTTP client for AI-B100 communication

---

## Project Structure

```
lorawan-counter/
├── build.sh
└── app/
    ├── main.c          # Application entry point, threads, HTTP endpoints
    ├── B100.c/h        # AI-B100 HTTP client library
    ├── ACAP.c/h        # Axis SDK wrapper
    ├── cJSON.c/h       # Embedded JSON library
    ├── manifest.json
    ├── settings/
    │   └── settings.json
    ├── localdata/
    │   └── counters.json
    └── html/           # Web UI (Bootstrap 5 + jQuery)
        ├── index.html
        ├── bridge.html
        ├── downlink.html
        ├── aoa.html
        └── about.html
```

---

## License

MIT — see [LICENSE](app/LICENSE).

---

## Credits & Contact

- **ACAP:** Fred Juhlin
- **AI-B100 hardware:** [AI Embedded Nordic AB](https://www.ai-embedded.se) — ai-b100@ai-embedded.se
- **Axis Object Analytics:** [Axis Communications](https://www.axis.com)

## Status Information

The ACAP maintains real-time status accessible via `/local/aib100/status`:

- Connection status (AI-B100 reachable)
- LoRaWAN join status
- Frame counters (uplink/downlink)
- Signal quality (RSSI, SNR)
- Device address
- Last transmission details

## Documentation

- [PROGRESS.md](PROGRESS.md) - Detailed development progress
- [../AI-B100-Integration-Guide.md](../AI-B100-Integration-Guide.md) - AI-B100 device documentation
- [../ACAP-Integration-Plan.md](../ACAP-Integration-Plan.md) - Complete integration plan

## Build

```bash
./build.sh
```

Generates packages for both aarch64 and armv7hf architectures.

