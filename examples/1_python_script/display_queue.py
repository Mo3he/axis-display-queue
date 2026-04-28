"""
Queue Display Driver
--------------------
Subscribes to restaurant/queue and restaurant/ready via MQTT.
Sends notifications to an Axis Display Speaker via the VAPIX
Speaker Display Notification API.

Display logic
  - Normally shows the current "PREPARING" list as a static
    indefinite notification (scrolling left).
  - When the ready list becomes non-empty, interrupts with a
    timed "ORDER READY" alert (green background).
  - After the ready duration elapses the driver reverts to the
    queue view automatically.
  - If the queue is empty the screen shows a "No orders" message.

Configuration: config.yaml in the same directory.
"""

import json
import logging
import os
import threading
import time

import paho.mqtt.client as mqtt
import requests
from requests.auth import HTTPDigestAuth
import yaml

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger(__name__)

VAPIX_NOTIFY = "/config/rest/speaker-display-notification/v1/simple"
VAPIX_STOP   = "/config/rest/speaker-display-notification/v1/stop"

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def load_config(path: str | None = None) -> dict:
    if path is None:
        path = os.path.join(_SCRIPT_DIR, "config.yaml")
    with open(path, encoding="utf-8") as f:
        return yaml.safe_load(f)


class DisplayDriver:
    """Manages what is shown on the Axis Display Speaker."""

    def __init__(self, host: str, username: str, password: str,
                 ready_duration_ms: int = 8000) -> None:
        self._base = f"http://{host}"
        self._auth = HTTPDigestAuth(username, password)
        self._ready_ms = ready_duration_ms
        self._lock = threading.Lock()
        self._showing_ready = False
        self._revert_timer: threading.Timer | None = None
        self._queue: list[int] = []
        self._ready: list[int] = []

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _post(self, path: str, payload: dict) -> None:
        url = self._base + path
        try:
            r = requests.post(url, auth=self._auth, json=payload, timeout=5)
            r.raise_for_status()
        except requests.RequestException as exc:
            log.warning("VAPIX call failed (%s): %s", url, exc)

    def _send_queue_view(self) -> None:
        """Show the preparing list, indefinite duration."""
        if self._queue:
            numbers = "  ".join(f"#{n}" for n in self._queue)
            message = f"PREPARING:  {numbers}"
        else:
            message = "No orders in queue"

        self._post(VAPIX_NOTIFY, {
            "data": {
                "message": message,
                "textColor": "#FFFFFF",
                "backgroundColor": "#1A1A2E",
                "textSize": "large",
                "scrollDirection": "fromRightToLeft",
                "scrollSpeed": 4,
            }
        })
        log.info("Display -> QUEUE  %s", self._queue)

    def _send_ready_view(self) -> None:
        """Show the ready list with a timed green alert."""
        numbers = "  ".join(f"#{n}" for n in self._ready)
        message = f"ORDER READY:  {numbers}"

        self._post(VAPIX_NOTIFY, {
            "data": {
                "message": message,
                "textColor": "#000000",
                "backgroundColor": "#00CC44",
                "textSize": "large",
                "scrollDirection": "fromRightToLeft",
                "scrollSpeed": 5,
                "duration": {
                    "type": "time",
                    "value": self._ready_ms,
                },
            }
        })
        log.info("Display -> READY  %s", self._ready)

    def _arm_revert(self) -> None:
        """Cancel any pending revert and schedule a new one."""
        if self._revert_timer:
            self._revert_timer.cancel()

        delay = self._ready_ms / 1000.0

        def _revert() -> None:
            with self._lock:
                self._showing_ready = False
                self._send_queue_view()

        self._revert_timer = threading.Timer(delay, _revert)
        self._revert_timer.daemon = True
        self._revert_timer.start()

    # ------------------------------------------------------------------
    # Public callbacks (called from the MQTT thread)
    # ------------------------------------------------------------------

    def on_queue(self, numbers: list[int]) -> None:
        with self._lock:
            self._queue = numbers
            if not self._showing_ready:
                self._send_queue_view()

    def on_ready(self, numbers: list[int]) -> None:
        with self._lock:
            self._ready = numbers
            if numbers:
                self._showing_ready = True
                self._send_ready_view()
                self._arm_revert()
            else:
                # Ready list cleared before timer fired - go back immediately.
                self._showing_ready = False
                if self._revert_timer:
                    self._revert_timer.cancel()
                self._send_queue_view()


# ----------------------------------------------------------------------
# MQTT wiring
# ----------------------------------------------------------------------

def main() -> None:
    config = load_config()

    disp_cfg = config["display"]
    driver = DisplayDriver(
        host=disp_cfg["host"],
        username=disp_cfg["username"],
        password=disp_cfg["password"],
        ready_duration_ms=disp_cfg.get("ready_duration_ms", 8000),
    )

    broker_host: str = config["mqtt"]["host"]
    broker_port: int = config["mqtt"]["port"]
    queue_topic: str = config["topics"]["queue"]
    ready_topic: str = config["topics"]["ready"]

    def on_connect(client, userdata, flags, rc):
        if rc == 0:
            log.info("Connected to MQTT broker %s:%d", broker_host, broker_port)
            client.subscribe([(queue_topic, 1), (ready_topic, 1)])
            log.info("Subscribed to %s and %s", queue_topic, ready_topic)
        else:
            log.error("MQTT connect failed, rc=%d", rc)

    def on_message(client, userdata, msg):
        try:
            numbers = json.loads(msg.payload)
        except (json.JSONDecodeError, ValueError):
            log.warning("Bad payload on %s: %r", msg.topic, msg.payload)
            return
        if msg.topic == queue_topic:
            driver.on_queue(numbers)
        elif msg.topic == ready_topic:
            driver.on_ready(numbers)

    client = mqtt.Client(client_id="queue-display-driver", clean_session=True)
    client.on_connect = on_connect
    client.on_message = on_message
    mqtt_user = config["mqtt"].get("username") or ""
    mqtt_pass = config["mqtt"].get("password") or ""
    if mqtt_user:
        client.username_pw_set(mqtt_user, mqtt_pass or None)
    client.connect(broker_host, broker_port, keepalive=60)

    log.info("Display driver starting - press Ctrl+C to stop")
    try:
        client.loop_forever()
    except KeyboardInterrupt:
        log.info("Stopping")


if __name__ == "__main__":
    main()
