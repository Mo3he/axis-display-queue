"""
Restaurant Queue System
-----------------------
Two MQTT topics, two lists of order numbers. That's it.

  restaurant/queue  ->  [3, 7, 12]   orders being prepared
  restaurant/ready  ->  [2, 5]       orders ready for pickup

Each order:
  1. Arrives -> added to queue
  2. After a random cook time -> moves from queue to ready
  3. After a random pickup time -> removed from ready
"""

import json
import logging
import random
import signal
import sys
import threading
import time

import paho.mqtt.client as mqtt
import yaml

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger(__name__)


def load_config(path: str = "config.yaml") -> dict:
    with open(path, encoding="utf-8") as f:
        return yaml.safe_load(f)


def run(config: dict) -> None:
    broker_host: str = config["mqtt"]["host"]
    broker_port: int = config["mqtt"]["port"]
    queue_topic: str = config["topics"]["queue"]
    ready_topic: str = config["topics"]["ready"]

    max_number: int = config["orders"]["max_number"]
    arrival_min: int = config["orders"]["arrival_min"]
    arrival_max: int = config["orders"]["arrival_max"]
    cook_min: int = config["orders"]["cook_time_min"]
    cook_max: int = config["orders"]["cook_time_max"]
    pickup_min: int = config["orders"]["pickup_time_min"]
    pickup_max: int = config["orders"]["pickup_time_max"]

    client = mqtt.Client(client_id="restaurant-queue", clean_session=True)
    client.connect(broker_host, broker_port, keepalive=60)
    client.loop_start()
    time.sleep(1)

    queue: list[int] = []
    ready: list[int] = []
    lock = threading.Lock()
    stop_event = threading.Event()
    next_number = [1]

    def publish_queue():
        """Publish only the queue topic. Must be called with lock held."""
        client.publish(queue_topic, json.dumps(queue), qos=1, retain=True)

    def publish_ready():
        """Publish only the ready topic. Must be called with lock held."""
        client.publish(ready_topic, json.dumps(ready), qos=1, retain=True)

    def shutdown(sig, frame):
        log.info("Stopping - clearing topics ...")
        stop_event.set()
        client.publish(queue_topic, json.dumps([]), qos=1, retain=True)
        client.publish(ready_topic, json.dumps([]), qos=1, retain=True)
        time.sleep(1)
        client.loop_stop()
        client.disconnect()
        sys.exit(0)

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    def process_order(number: int):
        """Cook then make ready then remove."""
        cook_time = random.randint(cook_min, cook_max)
        stop_event.wait(cook_time)
        if stop_event.is_set():
            return
        with lock:
            if number in queue:
                queue.remove(number)
            ready.append(number)
            log.info("Order %02d READY  | queue=%s ready=%s", number, queue, ready)
            publish_queue()   # queue shrank
            publish_ready()   # ready grew

        pickup_time = random.randint(pickup_min, pickup_max)
        stop_event.wait(pickup_time)
        if stop_event.is_set():
            return
        with lock:
            if number in ready:
                ready.remove(number)
            log.info("Order %02d done   | queue=%s ready=%s", number, queue, ready)
            publish_ready()   # ready shrank, queue unchanged

    log.info("Restaurant open. Broker %s:%d", broker_host, broker_port)

    while not stop_event.is_set():
        wait = random.randint(arrival_min, arrival_max)
        stop_event.wait(wait)
        if stop_event.is_set():
            break
        with lock:
            number = next_number[0]
            next_number[0] = (number % max_number) + 1
            queue.append(number)
            log.info("Order %02d placed | queue=%s ready=%s", number, queue, ready)
            publish_queue()   # queue grew, ready unchanged
        threading.Thread(target=process_order, args=(number,), daemon=True).start()


if __name__ == "__main__":
    run(load_config())
