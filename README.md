# AI-B100 — Axis Camera ACAP Collection

## What Is the AI-B100?

The **AI-B100** is an industrial LoRaWAN bridge by [AI Embedded Nordic AB](https://www.ai-embedded.se) that connects Ethernet-based devices — such as Axis cameras — to LoRaWAN networks. It sits on your local LAN and handles all LoRa radio communication, so the camera only needs to make simple HTTP calls over the local network.

| Property | Value |
|----------|-------|
| Protocol | LoRaWAN 1.0.4, Class A and Class C |
| Region | EU868 (other regions available) |
| LAN | 10/100 Ethernet, DHCP or static IP |
| Interface | JSON HTTP REST API |
| Configuration | Built-in web UI — no CLI required |
| Power | USB-C 5 V (standard) |
| Operating range | −20 °C to +85 °C |

**Variants:**
- **AI-B100**
- **AI-B100-GPS** — GPS antenna

Contact: **ai-b100@ai-embedded.se** | **www.ai-embedded.se**  
Location: AI Embedded Nordic AB, Vellinge, Sweden

---

## Why Use These ACAPs?

Standard Axis camera integrations rely on IP connectivity — cloud subscriptions, VPNs, or internet access at the installation site. These ACAPs remove that dependency entirely:

- The camera processes data **locally** using Axis analytics
- The ACAP communicates with the AI-B100 **over the local LAN via HTTP**
- The AI-B100 transmits compact payloads **over LoRaWAN radio** to any standard LoRaWAN network

This makes the solution suitable for:

| Scenario | Why it fits |
|----------|-------------|
| **Remote sites** | LoRaWAN is the only available connectivity |
| **Smart city installations** | Uses the existing city LoRaWAN infrastructure |
| **GDPR-sensitive deployments** | Only anonymised/numeric data leaves the device — no images, no video |
| **Locations without internet** | No cloud subscription or IP backhaul needed |
| **Unattended long-term deployments** | Configurable watchdog, Class C downlink for remote commands |

---

## Typical Hardware Setup

```
┌─────────────────────────────────────────────┐
│  Local LAN (PoE Switch)                     │
│                                             │
│  ┌────────────┐  HTTP/REST   ┌───────────┐  │
│  │ Axis Camera│◄────────────►│  AI-B100  │  │
│  │  + ACAP    │              │  Bridge   │  │
│  └────────────┘              └─────┬─────┘  │
│                                    │ LoRa   │
└────────────────────────────────────┼────────┘
                                     │
                              LoRaWAN Gateway
                                     │
                        LoRaWAN Network Server
                        (TTN / Chirpstack / private)
                                     │
                          Your backend / dashboard
```

**Minimum bill of materials:**
- Axis camera (ACAP v4+)
- AI-B100 bridge
- PoE switch (to power the camera and provide a commissioning port)
- PoE splitter with USB-C 5 V output (to power the standard AI-B100 from the PoE switch)

If using the **AI-B100-POE** variant, connect it directly to the PoE switch — no splitter needed.

---

## LoRaWAN Considerations

- **Duty cycle** — LoRaWAN imposes regional transmission limits. ACAPs in this collection enforce a minimum publish interval to stay compliant. Typical safe minimum: 10–15 minutes.
- **Payload size** — Payloads are kept compact. All ACAPs use binary or short ASCII encoding.
- **Class C** — The AI-B100 in Class C mode listens continuously, so downlink commands from the network server arrive within seconds.
- **Coverage** — Verify RSSI and SNR at the installation site before finalising the camera mount.
- **Network servers** — The AI-B100 works with TTN, Chirpstack, and any standard LoRaWAN LNS.

---

## Available ACAPs

| Directory | ACAP | Description |
|-----------|------|-------------|
| [`aoa-counter/`](aoa-counter/) | **AI-B100 AOA Counter** | Reads Axis Object Analytics CrosslineCounting events and publishes aggregated people/vehicle counts over LoRaWAN |

More ACAPs may be added here in the future, each in their own subdirectory with their own README.

---

## License

MIT — see [aoa-counter/app/LICENSE](aoa-counter/app/LICENSE).
