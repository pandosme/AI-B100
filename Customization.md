# Customization Guide

This document describes how to clone the **AOA Counter ACAP** and adapt it to detect and publish something specific to your use case — for example, counting only bicycles at a particular location, or filtering by time of day, or encoding a different payload format.

---

## Overview

The AOA Counter ACAP is designed as a general-purpose template. Its architecture is modular:

| Layer | File(s) | Role |
|-------|---------|------|
| **AI-B100 communication** | `B100.c`, `B100.h` | HTTP client for the LoRaWAN bridge — reusable as-is |
| **ACAP framework** | `ACAP.c`, `ACAP.h` | Axis SDK wrapper (settings, status, events, HTTP server) — reusable as-is |
| **Application logic** | `main.c` | Event handling, counter logic, payload encoding, downlink commands |
| **Web UI** | `html/` | Configuration and monitoring pages |
| **Package metadata** | `manifest.json` | ACAP identity, version, endpoints |
| **Build system** | `Dockerfile`, `build.sh`, `Makefile` | Docker-based cross-compilation |

To create a customized version, you clone the repository and modify `main.c`, `manifest.json`, the web UI, and the settings — while keeping `B100.c/h` and `ACAP.c/h` unchanged.

---

## Step 1: Clone and Rename

```bash
# Copy the entire aoa-counter directory
cp -r aoa-counter my-counter

cd my-counter
```

### Update Package Identity

Edit `app/manifest.json`:

```json
{
    "schemaVersion": "1.7.1",
    "acapPackageConf": {
        "setup": {
            "friendlyName": "My Custom Counter",
            "appName": "mycounter",
            "vendor": "Your Name",
            "version": "1.0.0"
        }
    }
}
```

**Important:** The `appName` must be unique on the camera. It determines the URL path (`/local/<appName>/`) and filesystem location.

Update the `APP_PACKAGE` define in `main.c`:

```c
#define APP_PACKAGE "mycounter"
```

---

## Step 2: Modify Event Detection

The core event handling is in `main.c`. The ACAP subscribes to AOA CrosslineCounting events and processes them in a callback.

### Changing Object Classes

To count only specific object types, modify the `g_publish_*` flags or the payload encoding:

```c
// Example: Only publish bicycle and human counts
static int g_publish_human = 1;
static int g_publish_car = 0;    // disabled
static int g_publish_bike = 1;
static int g_publish_bus = 0;    // disabled
static int g_publish_truck = 0;  // disabled
static int g_publish_other = 0;  // disabled
```

Or remove the settings UI for classes you don't need and hard-code the selection.

### Subscribing to Different Events

If you want to detect something other than CrosslineCounting (e.g., ObjectDetection, MotionDetection), modify the event subscription in `app/settings/subscriptions.json` and adapt the event callback in `main.c`.

The Axis event system uses a key-value subscription model. See the [ACAP documentation](https://axiscommunications.github.io/acap-documentation/) for available event types.

---

## Step 3: Customize the Payload

The uplink payload is encoded in the publish function in `main.c`. The default format is a compact binary encoding of counter deltas.

### Example: Custom Binary Format

```c
// Example: Simple 4-byte payload with just total-in and total-out
static void encode_my_payload(uint8_t *buf, int *len) {
    buf[0] = (uint8_t)((total_in >> 8) & 0xFF);
    buf[1] = (uint8_t)(total_in & 0xFF);
    buf[2] = (uint8_t)((total_out >> 8) & 0xFF);
    buf[3] = (uint8_t)(total_out & 0xFF);
    *len = 4;
}
```

### Update the TTN Decoder

When you change the payload format, update `translator.js` to match:

```javascript
function decodeUplink(input) {
    var bytes = input.bytes;
    return {
        data: {
            total_in: (bytes[0] << 8) | bytes[1],
            total_out: (bytes[2] << 8) | bytes[3]
        }
    };
}
```

---

## Step 4: Modify Settings

Edit `app/settings/settings.json` to add or remove configuration options:

```json
{
  "b100": {
    "ip": "192.168.1.20",
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
    "enabled": true
  },
  "custom": {
    "myNewSetting": "value"
  }
}
```

Settings are read in `main.c` during initialization and can be modified at runtime via the `/settings` HTTP endpoint.

---

## Step 5: Customize the Web UI

The web UI is in `app/html/` and uses Bootstrap 5 + jQuery. Each page communicates with the ACAP backend via the FastCGI HTTP endpoints.

### Adding a New Page

1. Create `app/html/mypage.html` (copy an existing page as a template)
2. Update the navigation bar in all HTML files to include the new link
3. If the page needs a new backend endpoint, add it to `manifest.json`:

```json
{"name": "myendpoint", "access": "viewer", "type": "fastCgi"}
```

4. Handle the endpoint in `main.c`:

```c
if (strcmp(request, "myendpoint") == 0) {
    // Handle GET/POST for this endpoint
}
```

### Removing Pages

Remove unused HTML files and their corresponding `httpConfig` entries from `manifest.json`. Remove the endpoint handlers from `main.c`.

---

## Step 6: Modify Downlink Commands

Downlink commands are handled in the `B100_Downlink_Handler()` function in `main.c`. You can:

- Add new command ports
- Change the command byte mappings
- Add custom actions (e.g., trigger a camera action, change detection parameters)

```c
// Example: Port 20 — custom commands
case 20:
    if (payload[0] == 0x01) {
        // Your custom action
        LOG("Custom command received\n");
    }
    break;
```

---

## Step 7: Build and Test

```bash
cd my-counter
./build.sh
```

This produces `.eap` files for both architectures. Install on the camera via **Apps → Add app**.

### Development Tips

- **Syslog:** All `LOG()` output goes to the camera syslog. View with:
  ```
  ssh root@<camera-ip> tail -f /var/log/messages | grep mycounter
  ```
- **Fast iteration:** During development, you can build for only one architecture by modifying `build.sh`
- **Settings reset:** Delete `/usr/local/packages/<appName>/localdata/` on the camera to reset persisted state
- **Web UI debugging:** The HTML/JS/CSS files are served directly from the package — changes require a rebuild and reinstall

---

## Project Structure After Customization

```
my-counter/
├── build.sh              ← unchanged
├── Dockerfile            ← unchanged
├── translator.js         ← updated to match your payload format
├── README.md             ← describe your specific use case
└── app/
    ├── main.c            ← modified: events, payload, commands
    ├── B100.c / B100.h   ← unchanged (AI-B100 communication)
    ├── ACAP.c / ACAP.h   ← unchanged (Axis framework)
    ├── cJSON.c / cJSON.h ← unchanged
    ├── Makefile           ← unchanged (unless adding source files)
    ├── manifest.json      ← updated: appName, friendlyName, endpoints
    ├── settings/
    │   └── settings.json  ← updated to your needs
    ├── localdata/
    │   └── counters.json
    └── html/              ← customized web UI
```

---

## Common Customization Scenarios

### Scenario A: Count Only People, Ignore Vehicles

- Disable car/bike/bus/truck/other in settings (or remove from UI entirely)
- Simplify payload to a single 2-byte counter
- Update decoder accordingly

### Scenario B: Add Time-Windowed Counting

- Add `startHour` / `endHour` settings
- In the publish thread, only accumulate counts during active hours
- Reset delta counters at the start of each window

### Scenario C: Different Camera Analytics

Replace AOA CrosslineCounting subscriptions with:
- **AOA Occupancy** — subscribe to occupancy events instead
- **Motion Detection** — count motion triggers
- **Custom ACAP analytics** — subscribe to events from another ACAP

### Scenario D: Multiple AI-B100 Bridges

- Add a second `b100` configuration block in settings
- Create a second set of B100 functions or parameterize the existing ones
- Useful for redundancy or when cameras feed different LoRaWAN networks

---

## API Reference (for UI development)

The ACAP exposes these FastCGI endpoints at `/local/<appName>/`:

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/app` | GET | Package manifest + settings |
| `/settings` | GET/POST | Read or write settings.json |
| `/status` | GET | Real-time ACAP status |
| `/counters` | GET | Current counter values |
| `/publish` | POST | Trigger immediate uplink |
| `/join` | POST | Initiate LoRaWAN join |
| `/test` | POST | Test AI-B100 connection |
| `/restart` | POST | Restart AI-B100 bridge |
| `/send` | POST | Send a custom test uplink |
| `/translator` | GET | Download payload decoder JS |

---

## Dependencies

The build system resolves all dependencies via the Axis ACAP SDK Docker container:

| Library | Purpose |
|---------|---------|
| `glib-2.0`, `gio-2.0` | GLib event loop and I/O |
| `axevent` | Axis event subscription system |
| `axparameter` | Axis parameter/settings storage |
| `fcgi` | FastCGI HTTP server |
| `libcurl` | HTTP client for AI-B100 communication |

No additional packages are needed unless you add new functionality (e.g., OpenSSL for crypto, sqlite for local databases).
