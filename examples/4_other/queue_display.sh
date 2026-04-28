#!/usr/bin/env bash
# queue_display.sh
# ----------------
# Pure bash queue display driver for an Axis Display Speaker.
# Subscribes to restaurant/queue and restaurant/ready via mosquitto_sub,
# posts to the VAPIX Speaker Display Notification API with curl.
#
# Dependencies: mosquitto-clients (mosquitto_sub), curl, jq
# Install on Debian/Ubuntu: apt-get install mosquitto-clients curl jq
# Install on macOS:         brew install mosquitto curl jq
#
# Usage:
#   DISPLAY_HOST=10.129.174.195 DISPLAY_USER=root DISPLAY_PASS=pass \
#   MQTT_HOST=10.129.174.38 ./queue_display.sh
#   MQTT_USER=myuser MQTT_PASS=mypass MQTT_HOST=10.129.174.38 ./queue_display.sh

set -euo pipefail

MQTT_HOST="${MQTT_HOST:-10.129.174.38}"
MQTT_PORT="${MQTT_PORT:-1883}"
MQTT_USER="${MQTT_USER:-}"   # leave blank for anonymous brokers
MQTT_PASS="${MQTT_PASS:-}"
QUEUE_TOPIC="${QUEUE_TOPIC:-restaurant/queue}"
READY_TOPIC="${READY_TOPIC:-restaurant/ready}"

DISPLAY_HOST="${DISPLAY_HOST:-10.129.174.195}"
DISPLAY_USER="${DISPLAY_USER:-root}"
DISPLAY_PASS="${DISPLAY_PASS:-pass}"
READY_DURATION_MS="${READY_DURATION_MS:-8000}"

VAPIX_URL="http://${DISPLAY_HOST}/config/rest/speaker-display-notification/v1/simple"

# --------------------------------------------------------------------------
# Shared state via temp files (subshells can't share variables)
# --------------------------------------------------------------------------
STATE_DIR=$(mktemp -d)
QUEUE_FILE="${STATE_DIR}/queue.txt"
SHOWING_READY_FILE="${STATE_DIR}/showing_ready.flag"
printf '' > "${QUEUE_FILE}"
trap 'rm -rf "${STATE_DIR}"' EXIT

log() { printf '%s  %s\n' "$(date '+%H:%M:%S')" "$*" >&2; }

# --------------------------------------------------------------------------
# VAPIX helpers
# --------------------------------------------------------------------------
vapix_post() {
    local payload="$1"
    curl -s --digest -u "${DISPLAY_USER}:${DISPLAY_PASS}" \
         -H "Content-Type: application/json" \
         -d "${payload}" \
         "${VAPIX_URL}" > /dev/null
}

send_queue_view() {
    local raw_queue="$1"
    local message
    if [[ -z "${raw_queue}" ]] || [[ "${raw_queue}" == "[]" ]]; then
        message="No orders in queue"
    else
        # Convert JSON array [1,2,3] to "#1  #2  #3"
        message="PREPARING:  $(echo "${raw_queue}" | jq -r '[.[] | "#" + tostring] | join("  ")')"
    fi
    log "Display -> QUEUE  ${raw_queue}"
    vapix_post "$(jq -nc \
        --arg msg "${message}" \
        '{data:{message:$msg,textColor:"#FFFFFF",backgroundColor:"#1A1A2E",textSize:"large",scrollDirection:"fromRightToLeft",scrollSpeed:4}}')"
}

send_ready_view() {
    local raw_ready="$1"
    local message
    message="ORDER READY:  $(echo "${raw_ready}" | jq -r '[.[] | "#" + tostring] | join("  ")')"
    local duration_s=$(( READY_DURATION_MS / 1000 ))
    log "Display -> READY  ${raw_ready}"
    vapix_post "$(jq -nc \
        --arg msg "${message}" \
        --argjson dur "${READY_DURATION_MS}" \
        '{data:{message:$msg,textColor:"#000000",backgroundColor:"#00CC44",textSize:"large",scrollDirection:"fromRightToLeft",scrollSpeed:5,duration:{type:"time",value:$dur}}}')"
    # Wait the display duration then revert (run in background)
    ( sleep "${duration_s}"
      if [[ -f "${SHOWING_READY_FILE}" ]]; then
          rm -f "${SHOWING_READY_FILE}"
          send_queue_view "$(cat "${QUEUE_FILE}")"
      fi
    ) &
}

# --------------------------------------------------------------------------
# Main: subscribe to both topics and dispatch
# --------------------------------------------------------------------------
# Build optional mosquitto_sub auth args
MQTT_AUTH_ARGS=()
if [[ -n "${MQTT_USER}" ]]; then
    MQTT_AUTH_ARGS=(-u "${MQTT_USER}" -P "${MQTT_PASS}")
fi

log "Queue display driver starting"
log "MQTT: ${MQTT_HOST}:${MQTT_PORT}${MQTT_USER:+ (user: ${MQTT_USER})}"
log "Speaker: ${DISPLAY_HOST}"

mosquitto_sub \
    -h "${MQTT_HOST}" \
    -p "${MQTT_PORT}" \
    "${MQTT_AUTH_ARGS[@]+${MQTT_AUTH_ARGS[@]}}" \
    -t "${QUEUE_TOPIC}" \
    -t "${READY_TOPIC}" \
    --retained-only 2>/dev/null &   # prime with retained messages first

mosquitto_sub \
    -h "${MQTT_HOST}" \
    -p "${MQTT_PORT}" \
    "${MQTT_AUTH_ARGS[@]+${MQTT_AUTH_ARGS[@]}}" \
    -t "${QUEUE_TOPIC}" \
    -t "${READY_TOPIC}" \
    -F "%t %p" \
    -q 1 |
while IFS=' ' read -r topic payload; do
    case "${topic}" in
        "${QUEUE_TOPIC}")
            echo "${payload}" > "${QUEUE_FILE}"
            if [[ ! -f "${SHOWING_READY_FILE}" ]]; then
                send_queue_view "${payload}"
            fi
            ;;
        "${READY_TOPIC}")
            count=$(echo "${payload}" | jq 'length')
            if [[ "${count}" -gt 0 ]]; then
                touch "${SHOWING_READY_FILE}"
                send_ready_view "${payload}"
            else
                # Ready list cleared before timer - revert immediately
                rm -f "${SHOWING_READY_FILE}"
                send_queue_view "$(cat "${QUEUE_FILE}")"
            fi
            ;;
    esac
done
