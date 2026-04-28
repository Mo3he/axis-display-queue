# 1. Python Script - Queue Display Driver

Subscribes to the restaurant MQTT queue and posts notifications to an Axis
Display Speaker using the VAPIX Speaker Display Notification API.

## Prerequisites

- Python 3.10+
- An Axis Display Speaker reachable on the network
- The queue system MQTT broker running

## Setup

```bash
pip install -r requirements.txt
```

Edit `config.yaml` and set:
- `mqtt.host` - broker IP
- `display.host` - IP of the Axis Display Speaker
- `display.username` / `display.password` - device credentials
- `display.ready_duration_ms` - how long the "ORDER READY" screen stays (ms)

## Run

```bash
python display_queue.py
```

## Display logic

| State | Screen | Colors |
|-------|--------|--------|
| Queue updates | `PREPARING: #3 #7 #12` scrolling left | White on dark blue |
| Ready ticket arrives | `ORDER READY: #2 #5` scrolling left | Black on green |
| Ready timer expires | Reverts to queue view automatically | - |

The ready notification uses the VAPIX `duration.type = "time"` field so the
device itself counts down. The Python timer mirrors that duration and then
sends the queue view again.

## API reference

`POST /config/rest/speaker-display-notification/v1/simple`
https://developer.axis.com/vapix/device-configuration/speaker-display-notification/
