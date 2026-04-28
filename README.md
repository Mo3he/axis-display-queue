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

## Display examples

### 1. Python script (`examples/1_python_script/`)

The simplest way to get started. Runs on any machine with Python 3.10+.

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

**Tested live** against `10.129.174.195` — confirmed full queue-to-ready-to-queue cycle working.

---

### 2. ACAP (`examples/2_acap/`)

An ACAP (Axis Camera Application Platform) application that runs directly **on the Axis Display Speaker**. Because it runs on the device, VAPIX calls go to localhost — no external machine needed after installation.

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
docker cp "$CONTAINER_ID":/opt/app/queue_display_1_0_0_aarch64.eap .
docker rm "$CONTAINER_ID"
```

**Install**

Upload `queue_display_1_0_0_aarch64.eap` via the device web interface at `http://<device-ip>/#settings/apps`.

**Configure**

After installation, open the ACAP settings page on the device (`http://<device-ip>/#settings/apps` -> Queue Display -> Settings). The parameters (MQTT host/port, topic names, ready display duration) are declared in `manifest.json` and appear automatically in the device web UI.

---

### 3. Node-RED (`examples/3_node_red/`)

A ready-to-import Node-RED flow. No coding required — just configure credentials in the UI.

**Import**

1. Open Node-RED (`http://localhost:1880` by default)
2. Hamburger menu -> **Import** -> paste the contents of `queue_display_flow.json`
3. Double-click the **mqtt-broker** node and set your broker host/port
4. Double-click the **POST to speaker display** node and set the speaker IP and credentials
5. Click **Deploy**

The flow handles the queue view, ready alert (8 s timed), and automatic revert using the VAPIX `duration.type = "time"` field.

---

### 4. Shell script (`examples/4_other/queue_display.sh`)

Pure bash — useful on constrained Linux hosts or for scripting pipelines.

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
