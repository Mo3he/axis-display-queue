# 2. ACAP - Queue Display

An ACAP (Axis Camera Application Platform) that runs directly on the Axis
Display Speaker. It subscribes to the restaurant MQTT topics and drives the
built-in Speaker Display Notification API through local VAPIX calls.

Because the ACAP runs on the device itself, there is no external service to
keep running. All settings are configured through the ACAP web UI on the device.

## Prerequisites

- Docker or Podman (for the SDK build container)
- An Axis Display Speaker with ACAP support

## Build

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

## Install

Upload the `.eap` file via the device web interface at
`http://<device-ip>/#settings/apps`.

## Configure

After installation, open the ACAP settings page on the device:

- **Settings tab** - queue topic, ready topic, ready screen duration
- **MQTT tab** - broker host, port, username, password

## Display logic

| Event | Screen | Style |
|-------|--------|-------|
| Queue changes | `PREPARING: #3 #7 #12` | White on dark blue, scrolling |
| Ready ticket | `ORDER READY: #2 #5` | Black on green, scrolling, 8 s |
| Timer expires | Back to queue view | - |

The ACAP uses libcurl to call the local Speaker Display Notification API
(`/config/rest/speaker-display-notification/v1/simple`) via the VAPIX
service account on `127.0.0.12`. A GLib one-shot timeout reverts the
display back to the queue view after the ready alert finishes.

## Key files

| File | Purpose |
|------|------|
| `app/main.c` | All application logic (self-contained, no external libraries) |
| `app/manifest.json` | ACAP package metadata and parameter defaults |
| `app/Makefile` | Build config |
