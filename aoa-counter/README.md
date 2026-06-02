# AI-B100 AOA Counter

An [ACAP](https://www.axis.com/developer-community/acap) for Axis cameras that reads **Axis Object Analytics (AOA)** CrosslineCounting events and publishes aggregated people and vehicle counts over **LoRaWAN** via an [AI-B100 bridge](https://www.ai-embedded.se).

The camera performs all inference locally. Only numeric counts leave the device — no images, no video, no internet connection required at the camera site.

---

## Why This Exists

Standard IP-connected cameras require internet access or a cloud subscription to forward data off-site. In many deployments — remote field sites, smart city installations, GDPR-sensitive locations — that is either unavailable, undesirable, or prohibited.

This solution replaces the IP backhaul entirely:

- The camera runs AOA locally and counts objects in the scene
- The ACAP reads those counts and forwards them over the local LAN to the AI-B100 bridge
- The AI-B100 transmits compact binary payloads via LoRaWAN radio to any standard network server (TTN, Chirpstack, private LNS)
- Only numeric counters leave the device

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
| **PoE switch** | Powers camera and PoE splitter |
| **PoE splitter (ETH + USB-C 5V)** | Splits PoE into Ethernet data and USB-C power for the AI-B100 |

**AI-B100 variants:** AI-B100-POE (PoE-powered), AI-B100-ANT (external antenna), AI-B100-POE-ANT (both).

---

## Quick Start

1. Connect camera and AI-B100 to the same PoE switch
2. Configure AOA CrosslineCounting scenarios on the camera
3. Register the AI-B100 on your LoRaWAN network server (TTN, Chirpstack, etc.)
4. Install the ACAP `.eap` file on the camera
5. Open the ACAP UI, set the AI-B100 IP, and verify LoRaWAN join

For complete step-by-step instructions, see **[DEPLOYMENT.md](DEPLOYMENT.md)**.

---

## Features

- **Automatic AOA discovery** — detects all CrosslineCounting scenarios automatically
- **6 object classes** — human, car, bike, bus, truck, other (individually enable/disable)
- **Configurable interval** — 5–60 minutes between LoRaWAN transmissions
- **Downlink commands** — remotely reset counters, change interval, request device info
- **Auto-join** — automatically rejoins LoRaWAN if connection is lost
- **Persistent counters** — survive app and camera restarts
- **Web UI** — live counters, bridge status, downlink log, AOA scenarios

---

## Downlink Commands

Commands are short binary payloads sent from your LoRaWAN network server.

### Port 10 — Actions

| Byte | Action |
|------|--------|
| `0x01` | Restart the AI-B100 bridge |
| `0x02` | Initiate a new LoRaWAN OTAA join |
| `0x03` | Reset all counters to zero and publish immediately |

### Port 11 — Configuration (2 bytes: `[command, value]`)

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

## Uplink Payload

Counters are encoded as compact binary. A JavaScript payload decoder is provided:

- [`translator.js`](translator.js) — TTN Payload Formatter / Node-RED Function node

The decoder is also downloadable from the ACAP web UI (**About** page).

---

## Building from Source

```bash
# Requires Docker and the Axis ACAP SDK image
cd aoa-counter
./build.sh
```

Output:
- `AI-B100_AOA_Counter_<version>_aarch64.eap`
- `AI-B100_AOA_Counter_<version>_armv7hf.eap`

---

## Documentation

| Document | Description |
|----------|-------------|
| **[DEPLOYMENT.md](DEPLOYMENT.md)** | Complete field deployment guide — hardware setup, configuration, best practices |
| **[Customization.md](Customization.md)** | How to clone this ACAP and adapt it for a different detection/publishing use case |
| [AI-B100 Integration Guide](../http_api.md) | AI-B100 device HTTP API reference |

---

## License

MIT — see [LICENSE](app/LICENSE).

---

## Credits & Contact

- **ACAP:** Fred Juhlin
- **AI-B100 hardware:** [AI Embedded Nordic AB](https://www.ai-embedded.se) — ai-b100@ai-embedded.se
- **Axis Object Analytics:** [Axis Communications](https://www.axis.com)
