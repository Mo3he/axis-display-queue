# Restaurant Queue System

A simulated restaurant order queue that publishes live state over MQTT, plus four different ways to display that state on an **Axis Display Speaker** using the VAPIX Speaker Display Notification API.

## How it works

Orders move through two stages:

```
Arrives --> restaurant/queue --> (cooked) --> restaurant/ready --> (picked up) --> removed
```

Both topics always carry the full current list as a JSON array:

```
restaurant/queue  →  [3, 7, 12]    orders being prepared
restaurant/ready  →  [2, 5]        orders ready for pickup
```

The display driver (whichever implementation you choose) subscribes to both topics and drives the speaker screen:

| State | Display | Colors |
|-------|---------|--------|
| Idle / queue updates | `PREPARING:  #3  #7  #12` scrolling | White on dark blue |
| Ticket ready | `ORDER READY:  #2  #5` scrolling | Black on green |
| Ready timer expires | Reverts to queue view | - |

---

## Project structure

```
queue system/
├── queue_system.py          # MQTT queue simulator
├── config.yaml              # Simulator configuration
├── requirements.txt         # Python dependencies for the simulator
└── examples/
    ├── 1_python_script/     # Python display driver (tested, recommended)
    ├── 2_acap/              # ACAP C application (runs on the speaker itself)
    ├── 3_node_red/          # Node-RED flow (import and go)
    └── 4_other/             # Shell script (bash + mosquitto_sub + curl)
```

---

## Quick start

### 1. Run the simulator

The simulator generates orders at random intervals and publishes them to MQTT.

```bash
python -m venv .venv
source .venv/bin/activate      # Windows: .venv\Scripts\activate
pip install -r requirements.txt

python queue_system.py
```

Edit `config.yaml` to change the broker address, timing, or topic names.

### 2. Pick a display driver and run it alongside the simulator

See the sections below.

---

## Configuration (`config.yaml`)

```yaml
mqtt:
  host: 10.129.174.38      # MQTT broker IP
  port: 1883

topics:
  queue: restaurant/queue
  ready: restaurant/ready

orders:
  max_number: 99           # highest order number before wrapping
  arrival_min: 60          # min seconds between new orders
  arrival_max: 120
  cook_time_min: 120       # min seconds an order spends in the queue
  cook_time_max: 360
  pickup_time_min: 30      # min seconds an order stays in ready
  pickup_time_max: 120
```

---

## Choosing a display driver

| | Python script | ACAP | Node-RED | Shell script |
|---|---|---|---|---|
| Runs on | Any machine with Python | The Axis device itself | Any machine with Node-RED | Any Linux/macOS host |
| External host required | Yes | **No** | Yes | Yes |
| Setup effort | Low | Medium (build + install) | Low | Low |
| Customisation | Edit Python | Edit C, rebuild | Drag-and-drop UI | Edit bash |
| Best for | Dev/testing, always-on servers | Production, standalone | Non-developers, rapid prototyping | Embedded Linux, scripting pipelines |

---

## Display examples

### 1. Python script (`examples/1_python_script/`)

**How it works**

`display_queue.py` connects to the MQTT broker using `paho-mqtt` and subscribes to both the queue and ready topics. When a message arrives it parses the JSON array and calls the VAPIX Speaker Display Notification API via HTTP Digest using the `requests` library. A `threading.Timer` fires after the configured ready duration and reverts the display back to the queue view.

**Pros**
- Easiest to read, modify, and debug
- Runs on any OS with Python 3.10+
- Config is a plain YAML file — no recompile needed
- Good starting point for adapting to other display content or logic

**Cons**
- Requires a host machine to be running continuously alongside the speaker
- Python runtime and dependencies must be installed

**Setup**

```bash
cd examples/1_python_script
pip install -r requirements.txt
```

Edit `config.yaml` and set `display.host`, `display.username`, `display.password`.

**Run**

```bash
python display_queue.py
```

**Tested live** against a real Axis Display Speaker — confirmed full queue-to-ready-to-queue cycle working.

---

### 2. ACAP (`examples/2_acap/`)

**How it works**

An ACAP (Axis Camera Application Platform) native C application that runs directly **on the Axis Display Speaker** itself. It uses a raw POSIX socket MQTT client (no external library) to subscribe to the broker, and libcurl to POST to the VAPIX Speaker Display API at `127.0.0.12` (the on-device VAPIX service address). VAPIX credentials are fetched at startup via D-Bus from the device's own credential store. Settings (broker host, port, topics, ready duration) are stored via `axparameter` and appear automatically in the device web UI under Apps > Queue Display > Settings.

The pre-built `.eap` installer is included in `examples/2_acap/` for aarch64 devices.

**Pros**
- Fully self-contained — no external host required after installation
- Runs persistently as a supervised process (auto-restarts on crash)
- Settings manageable from the device web UI without touching any files
- VAPIX calls stay on-device (loopback) — no network hop for display updates
- Smallest possible footprint: a single C binary, no interpreter or runtime

**Cons**
- Requires Docker or Podman to rebuild from source
- Changes to logic require a C recompile and reinstall
- Only runs on Axis devices with ACAP support (aarch64 firmware 12.x+)

**Build** (requires Docker or Podman)

```bash
cd examples/2_acap

# Docker
docker build --build-arg ARCH=aarch64 --tag queue-display-acap .

# Podman on Apple Silicon (Rosetta)
podman build --platform linux/amd64 --build-arg ARCH=aarch64 --tag queue-display-acap .
```

Extract the built `.eap`:

```bash
CONTAINER_ID=$(docker create queue-display-acap)
docker cp "$CONTAINER_ID":/opt/app/Queue_Display_1_0_0_aarch64.eap .
docker rm "$CONTAINER_ID"
```

**Install**

Upload `Queue_Display_1_0_0_aarch64.eap` via the device web interface at `http://<device-ip>/#settings/apps`, or use curl:

```bash
curl --digest -u root:pass \
  -F "upload=@Queue_Display_1_0_0_aarch64.eap" \
  http://<device-ip>/axis-cgi/applications/upload.cgi
```

**Configure**

After installation, open `http://<device-ip>/#settings/apps` -> Queue Display -> Settings. The MQTT host, port, topic names, and ready display duration are all configurable from there without rebuilding.

---

### 3. Node-RED (`examples/3_node_red/`)

**How it works**

A Node-RED flow with two `mqtt-in` nodes (one per topic) feeding into function nodes that format the display message and manage the ready-to-queue revert timer using `setTimeout`. The formatted JSON is sent to an `http request` node that POSTs to the VAPIX API with HTTP Digest authentication. All wiring is visual — no code files to edit.

**Pros**
- No coding required — everything is configured through the Node-RED UI
- Easy to extend: add new nodes (email alerts, dashboards, logging) without touching existing logic
- Visual flow makes the data path obvious
- Good fit for teams already running Node-RED for other integrations

**Cons**
- Requires a running Node-RED instance (typically a separate server or Raspberry Pi)
- The ready-to-queue revert timer lives inside a function node, which can be lost on flow redeploy
- Digest auth configuration is less obvious than basic auth in Node-RED

**Import**

1. Open Node-RED (`http://localhost:1880` by default)
2. Hamburger menu -> **Import** -> paste the contents of `queue_display_flow.json`
3. Double-click the **mqtt-broker** node and set your broker host/port
4. Double-click the **POST to speaker display** node and set the speaker IP and credentials
5. Click **Deploy**

The flow handles the queue view, ready alert (8 s timed), and automatic revert using the VAPIX `duration.type = "time"` field.

---

### 4. Shell script (`examples/4_other/queue_display.sh`)

**How it works**

`mosquitto_sub` runs in a loop and pipes each incoming MQTT message to the main logic. `jq` parses the JSON array and formats the display string. `curl --digest` sends the VAPIX POST request. The ready-to-queue revert is handled by a background `sleep` + kill pattern using bash job control.

**Pros**
- Zero dependencies beyond `mosquitto-clients`, `curl`, and `jq` — available on almost any Linux system
- Single file, easy to audit
- Simple to embed in cron jobs, systemd services, or other shell pipelines
- No runtime to install or maintain

**Cons**
- Bash's background job handling for the revert timer is fragile — rapid ready updates can leave stale background jobs
- No persistent settings: all config is environment variables or edited inline
- Harder to extend cleanly compared to a proper language

**Dependencies**

```bash
# Debian/Ubuntu
apt-get install mosquitto-clients curl jq

# macOS
brew install mosquitto curl jq
```

**Run**

```bash
DISPLAY_HOST=10.129.174.195 \
DISPLAY_USER=root \
DISPLAY_PASS=pass \
MQTT_HOST=10.129.174.38 \
./examples/4_other/queue_display.sh
```

All settings can also be edited at the top of the script.

---

## VAPIX Speaker Display Notification API

All display drivers call the same endpoint:

```
POST /config/rest/speaker-display-notification/v1/simple
```

Authentication: **HTTP Digest**

Key fields:

| Field | Description |
|-------|-------------|
| `message` | Text to display (max 1000 chars) |
| `textColor` | RGB hex, e.g. `#FFFFFF` |
| `backgroundColor` | RGB hex, e.g. `#1A1A2E` |
| `textSize` | `small`, `medium`, or `large` |
| `scrollDirection` | `fromRightToLeft`, `fromBottomToTop`, `fromLeftToRight` |
| `scrollSpeed` | 0 (static) to 10 |
| `duration.type` | `time` (ms), `repetitions`, or `timeCompleteMessage` |

Full reference: https://developer.axis.com/vapix/device-configuration/speaker-display-notification/

---

## Dependencies

| Component | Language | Key libraries |
|-----------|----------|---------------|
| Simulator | Python 3.10+ | paho-mqtt, pyyaml |
| Python driver | Python 3.10+ | paho-mqtt, requests, pyyaml |
| ACAP | C | ACAP Native SDK 12.2.0, GLib, libcurl, axparameter |
| Node-RED flow | - | Node-RED built-ins |
| Shell script | bash | mosquitto-clients, curl, jq |
