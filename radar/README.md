# Radar

An Axis ACAP application for radar-equipped Axis cameras. The app subscribes to Axis radar scene data, estimates occupancy from tracked radar objects, and publishes compact numeric snapshots over LoRaWAN through an AI-B100 bridge.

The package name is `aib100`, matching the AOA app. This allows the Radar and AOA variants to reuse the same ACAP settings, HTTP endpoint paths, and AI-B100 callback configuration when switching between applications. The user-facing name shown in the camera UI is **Radar**.

Only numeric occupancy data is transmitted. No images or video leave the camera.

## Deployment

For hardware setup, IP addressing, camera account setup, bridge LAN configuration, LoRaWAN registration, and field verification, use the shared guide:

- [../DEPLOYMENT.md](../DEPLOYMENT.md)

The packaged defaults follow that deployment guide:

| Setting | Default |
|---------|---------|
| Camera callback address | `192.168.1.2` |
| AI-B100 bridge address | `192.168.1.250` |
| Callback digest user | `aib100` |
| Callback digest password | `aib100` |
| Callback port | `80` |
| LoRaWAN uplink ports | Counting `1`, Occupancy `2`, Alert `3` |
| Publish interval | `15` minutes |

The ACAP uses the camera address as the HTTP callback target for the bridge. The camera itself must still be configured with the static or fallback IP address described in [../DEPLOYMENT.md](../DEPLOYMENT.md).

## System Overview

```text
Local PoE LAN

Axis radar camera + ACAP  <--HTTP callbacks/status-->  AI-B100 bridge  --LoRaWAN-->  Network server
```

The camera runs the radar scene provider and this ACAP locally. The ACAP reads radar objects, maintains independent use-case state, encodes compact binary payloads, and sends them to the AI-B100 bridge over HTTP. The bridge transmits the payload over LoRaWAN and forwards downlinks back to the ACAP by HTTP callback.

## Web UI

Open the app from the Axis camera Apps page. The main pages are:

| Page | Purpose |
|------|---------|
| Publish | Enable/disable each use case, view fixed LoRaWAN ports, see next publish times, publish manually, download decoder, and view the last 20 publishes |
| Area Balance | Select Human or Vehicle detection, publish frequency, and optional area of interest for entering/exiting counts on fixed uplink port 1 |
| Occupancy | Select Interval Maximum or Area Balance, Human or Vehicle detection, publish frequency, and area of interest for fixed uplink port 2 |
| Detection Alert | Select Human or Vehicle detection, inactive heartbeat publish, active publish, hold time, and optional area of interest for fixed uplink port 3 |
| Radar | View the live stream and set Radar Detection Sensitivity |
| LoRA Bridge | Configure bridge IP, callback address, callback credentials, join/restart/link-check, and bridge parameters |
| LoRA Downlink | View received downlinks and enable/disable supported command bytes |
| GPS | View GPS information reported by the AI-B100 bridge |
| About | App, camera, and decoder information |

## Use Cases

Axis radar reports moving tracked objects. The app now runs use cases independently so Counting, Occupancy, and Detection Alert can be enabled at the same time.

### Area Balance

Area Balance uses fixed uplink port `1`. It counts selected Human or Vehicle objects entering and exiting the configured area of interest during the publish interval. On publish, it sends entering and exiting counts and then resets the interval counters.

### Occupancy

Occupancy uses fixed uplink port `2`. It can publish either the interval maximum or Area Balance, with a publish frequency from 1 to 60 minutes. It can count Humans or Vehicles. Area Balance requires an area of interest and counts selected detections entering and exiting that area.

### Interval Peak

The app tracks the highest number of simultaneously valid radar objects seen during the publish interval. This is useful when you want a conservative "how busy was it" value rather than a live inside/outside balance.

On each scheduled publish, the current interval peak is sent and then the interval state starts again.

### Detection Alert

Detection Alert uses fixed uplink port `3`. It can detect Humans or Vehicles and can optionally limit detection to an area of interest. The hold time controls how long detections must remain present before entering active state, and how long detections must remain absent before returning to inactive state.

When inactive, Detection Alert publishes payload `0x00` on the inactive heartbeat publish timer. When detections become active, it publishes immediately. While active, it publishes one byte containing the maximum number of selected Human or Vehicle detections seen since the previous active publish.

## Radar Filtering

The Radar page shows the live stream and reads/sets the camera Radar Detection Sensitivity directly through `/axis-cgi/radar/radaranalytics.cgi` using `low`, `medium`, or `high`. Occupancy counts valid radar objects internally. Detection Alert applies its selected label and optional area of interest before entering active state.

## LoRaWAN Uplink Payload

Each use case has a fixed LoRaWAN port, so compact payloads do not include a mode byte. Area Balance publishes on port `1`, Occupancy publishes on port `2`, and Detection Alert publishes on port `3`. Counts are clamped to unsigned 8-bit values, `0-255`. The selected label, Humans or Vehicles, is configured in the ACAP UI and is not encoded in the uplink.

| Port | Use case | Payload |
|------|----------|---------|
| `1` | Area Balance | 2 bytes: `[entering, exiting]` |
| `2` | Occupancy Interval Maximum | 1 byte: `[selected_label_interval_max]` |
| `2` | Occupancy Area Balance | 2 bytes: `[entering, exiting]` |
| `3` | Detection Alert inactive | 1 byte: `[0x00]` |
| `3` | Detection Alert active | 1 byte: `[selected_label_count]` |

The decoder is available in two places:

- [translator.js](translator.js)
- **Publish** or **About** page in the ACAP UI, using **Download JavaScript Translator**

## Downlink Commands

Downlinks are received by the AI-B100 bridge and delivered to the ACAP callback endpoint. Commands can be enabled or disabled on the **LoRA Downlink** page.

### Port 100 - Actions

| Byte | Action |
|------|--------|
| `0x01` | Restart the AI-B100 bridge |
| `0x02` | Initiate a new LoRaWAN OTAA join |
| `0x03` | Reset radar use-case state and publish occupancy immediately |

### Port 110 - Configuration

Port 110 commands use two bytes: `[command, value]`.

| Byte 0 | Byte 1 | Action |
|--------|--------|--------|
| `0x01` | `1-60` | Set Occupancy transmission interval in minutes |
| `0x02` | `0-5` | Set fixed data rate, DR0 to DR5 |
| `0x03` | `0/1` | Disable or enable ADR |

### Port 120 - Information Requests

| Byte | Reply port | Response payload |
|------|------------|------------------|
| `0x01` | 121 | Camera info: model, serial, firmware, uptime hours, CPU %, ACAP version |
| `0x02` | 122 | Bridge info: hardware, firmware, power source, temperature, restart count, DevAddr |
| `0x03` | Local status | Signal quality: data rate, max payload, RSSI, SNR, frame counters up/down |

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
| `publish?stream=counting|occupancy|alert` | POST | Publish a specific use case immediately |
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
		"ip": "192.168.1.250",
		"port": 80,
		"timeout": 30,
		"callbackIP": "192.168.1.2",
		"callbackPort": 80,
		"callbackDigestUser": "aib100",
		"callbackDigestPassword": "aib100"
	},
	"lorawan": {
		"port": 2,
		"confirmed": false,
		"dataRate": 4,
		"autoJoin": true
	},
	"transmission": {
		"counting": {
			"enabled": false,
			"port": 1,
			"intervalMinutes": 15,
			"label": "human",
			"aoi": { "enabled": false }
		},
		"occupancy": {
			"enabled": true,
			"port": 2,
			"intervalMinutes": 15,
			"type": "maximum",
			"label": "human",
			"aoi": { "enabled": false }
		},
		"alert": {
			"enabled": false,
			"port": 3,
			"label": "human",
			"heartbeatMinutes": 15,
			"activeIntervalSeconds": 60,
			"transitionSeconds": 2,
			"aoi": { "enabled": false }
		}
	},
	"radar": {
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

- `Radar_1_2_0_aarch64.eap`
- `Radar_1_2_0_armv7hf.eap`

Use `aarch64` for ARTPEC-8 and ARTPEC-9 cameras. Use `armv7hf` for ARTPEC-7 cameras.

## Install

Upload the correct `.eap` file through the Axis camera Apps page, or use the helper script:

```bash
./install.sh <camera-host> Radar_1_2_0_aarch64.eap
```

If an older `radaroccupancy` package is still installed, remove it before using this package. Both the Radar and AOA variants now use `appName: "aib100"`; this is intentional so settings and callback paths are reused.

## License

MIT - see [app/LICENSE](app/LICENSE).