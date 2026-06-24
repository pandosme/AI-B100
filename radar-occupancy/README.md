# AIB100 Radar Occupancy

An Axis ACAP application for radar-equipped Axis cameras. The app subscribes to Axis radar scene data, estimates occupancy from tracked radar objects, and publishes compact numeric snapshots over LoRaWAN through an AI-B100 bridge.

The package name is `aib100`, matching the AOA Counter app. This allows the Radar Occupancy and AOA Counter variants to reuse the same ACAP settings, HTTP endpoint paths, and AI-B100 callback configuration when switching between applications. The user-facing name shown in the camera UI is **AIB100 Radar Occupancy**.

Only numeric occupancy data is transmitted. No images or video leave the camera.

## Deployment

For hardware setup, IP addressing, camera account setup, bridge LAN configuration, LoRaWAN registration, and field verification, use the shared guide:

- [../DEPLOYMENT.md](../DEPLOYMENT.md)

The packaged defaults follow that deployment guide:

| Setting | Default |
|---------|---------|
| Camera callback address | `192.168.0.2` |
| AI-B100 bridge address | `192.168.0.3` |
| Callback digest user | `aib100` |
| Callback digest password | `aib100` |
| Callback port | `80` |
| LoRaWAN uplink port | `10` |
| Publish interval | `15` minutes |

The ACAP uses the camera address as the HTTP callback target for the bridge. The camera itself must still be configured with the static or fallback IP address described in [../DEPLOYMENT.md](../DEPLOYMENT.md).

## System Overview

```text
Local PoE LAN

Axis radar camera + ACAP  <--HTTP callbacks/status-->  AI-B100 bridge  --LoRaWAN-->  Network server
```

The camera runs the radar scene provider and this ACAP locally. The ACAP reads radar objects, maintains an occupancy state, encodes that state as a 10-byte binary payload, and sends it to the AI-B100 bridge over HTTP. The bridge transmits the payload over LoRaWAN and forwards downlinks back to the ACAP by HTTP callback.

## Web UI

Open the app from the Axis camera Apps page. The main pages are:

| Page | Purpose |
|------|---------|
| Publish | Enable publishing, set LoRaWAN port and interval, publish manually, download decoder |
| Radar | Select occupancy mode, object class, confidence/dwell settings, polygon area, and view live radar objects |
| LoRA Bridge | Configure bridge IP, callback address, callback credentials, join/restart/link-check, and bridge parameters |
| LoRA Downlink | View received downlinks and enable/disable supported command bytes |
| GPS | View GPS information reported by the AI-B100 bridge |
| About | App, camera, and decoder information |

## Occupancy Behaviour

Axis radar reports moving tracked objects. Stationary objects may stop being reported, so the app offers several interpretations of occupancy.

### Interval Peak

`occupancyMode: "maximum"`

The app tracks the highest number of simultaneously valid radar objects seen during the publish interval. This is useful when you want a conservative "how busy was it" value rather than a live inside/outside balance.

On each scheduled publish, the current interval peak is sent and then the interval state starts again.

### Area Balance

`occupancyMode: "entry_exit"`

The app uses the configured polygon area and object transitions to maintain an inside-area count. Objects that enter the polygon increment the count; objects that leave decrement it. Decrements that would make a count negative are ignored.

Use this mode when the radar view and area polygon can reasonably represent entry and exit transitions.

### Presence Alert

`occupancyMode: "alert"`

The app publishes only when a new detection episode starts. There is no interval publish in this mode. The first valid occupancy after an empty state marks an alert as pending; the publish thread sends it immediately. After publishing, the app waits until the area is clear before another alert can be generated.

Pressing **Publish Now** follows the same rule: if no alert is pending, the backend returns `409 No alert pending` and no uplink is sent.

## Radar Filtering

The Radar page shows live tracked objects and whether each object is currently countable. Objects may be filtered out if they are outside the configured polygon, below the confidence threshold, not the selected class, stale, or not yet past the minimum dwell time.

The app can publish one selected class bucket: Human or Vehicle. Unknown radar classifications are assigned to the selected bucket for publishing; the payload still contains an `unknown` field for decoder compatibility.

## LoRaWAN Uplink Payload

Radar occupancy uplinks are fixed 10-byte binary payloads:

| Byte(s) | Field |
|---------|-------|
| 0 | Protocol version, currently `1` |
| 1 | Mode: `0=Interval peak`, `1=Area balance`, `2=Presence alert` |
| 2-3 | Total occupancy, uint16 little-endian |
| 4-5 | Human occupancy, uint16 little-endian |
| 6-7 | Vehicle occupancy, uint16 little-endian |
| 8-9 | Unknown occupancy, uint16 little-endian |

The decoder is available in two places:

- [translator.js](translator.js)
- **Publish** or **About** page in the ACAP UI, using **Download JavaScript Translator**

## Downlink Commands

Downlinks are received by the AI-B100 bridge and delivered to the ACAP callback endpoint. Commands can be enabled or disabled on the **LoRA Downlink** page.

### Port 10 - Actions

| Byte | Action |
|------|--------|
| `0x01` | Restart the AI-B100 bridge |
| `0x02` | Initiate a new LoRaWAN OTAA join |
| `0x03` | Reset radar occupancy state and publish immediately |

### Port 11 - Configuration

Port 11 commands use two bytes: `[command, value]`.

| Byte 0 | Byte 1 | Action |
|--------|--------|--------|
| `0x01` | `5-60` | Set transmission interval in minutes |
| `0x02` | `0-5` | Set fixed data rate, DR0 to DR5 |
| `0x03` | `0/1` | Disable or enable ADR |

### Port 12 - Information Requests

| Byte | Reply port | Response payload |
|------|------------|------------------|
| `0x01` | 5 | Camera info: model, serial, firmware, uptime hours, CPU %, ACAP version |
| `0x02` | 6 | Bridge info: hardware, firmware, power source, temperature, restart count, DevAddr |
| `0x03` | 7 | Signal quality: data rate, max payload, RSSI, SNR, frame counters up/down |

## HTTP Endpoints

The app uses the same package path as the AOA Counter app:

```text
/local/aib100/<endpoint>
```

Important endpoints include:

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `app` | GET | Manifest, settings, status, and device information |
| `settings` | GET/POST | Read or update persistent app settings |
| `status` | GET | ACAP framework status store |
| `counters` | GET | Compatibility alias returning radar publish/status data |
| `radar` | GET | Radar occupancy state, settings, and object list |
| `radar_reset` | POST | Reset retained radar occupancy state |
| `publish` | POST | Publish immediately, or return 409 in alert mode with no pending alert |
| `translator` | GET | Download JavaScript LoRaWAN payload decoder |
| `b100_status` | POST | AI-B100 status callback endpoint |
| `b100_receive` | POST | AI-B100 downlink callback endpoint |
| `b100_gps` | POST | AI-B100 GPS callback endpoint |
| `b100_info` | GET | AI-B100 bridge identity and callback status |
| `b100_request_status` | POST | Configure callbacks and request bridge status |

The callback URI values are short enough for the AI-B100 firmware limit:

```text
/local/aib100/b100_status
/local/aib100/b100_receive
/local/aib100/b100_gps
```

## Default Settings

The packaged default settings are:

```json
{
	"b100": {
		"ip": "192.168.0.3",
		"port": 80,
		"timeout": 30,
		"callbackIP": "192.168.0.2",
		"callbackPort": 80,
		"callbackDigestUser": "aib100",
		"callbackDigestPassword": "aib100"
	},
	"lorawan": {
		"port": 10,
		"confirmed": false,
		"dataRate": 4,
		"autoJoin": true
	},
	"transmission": {
		"intervalMinutes": 15,
		"enabled": true
	},
	"radar": {
		"occupancyMode": "maximum",
		"objectClass": "human",
		"staleTimeoutSeconds": 60,
		"minDwellSeconds": 0,
		"minConfidence": 30
	},
	"polling": {
		"healthCheckIntervalSeconds": 60
	}
}
```

## Building

```bash
cd radar-occupancy
./build.sh
```

Output:

- `AIB100_Radar_Occupancy_1_0_1_aarch64.eap`
- `AIB100_Radar_Occupancy_1_0_1_armv7hf.eap`

Use `aarch64` for ARTPEC-8 and ARTPEC-9 cameras. Use `armv7hf` for ARTPEC-7 cameras.

## Install

Upload the correct `.eap` file through the Axis camera Apps page, or use the helper script:

```bash
./install.sh <camera-host> AIB100_Radar_Occupancy_1_0_1_aarch64.eap
```

If an older `radaroccupancy` package is still installed, remove it before using this package. Both the Radar Occupancy and AOA Counter variants now use `appName: "aib100"`; this is intentional so settings and callback paths are reused.

## License

MIT - see [app/LICENSE](app/LICENSE).