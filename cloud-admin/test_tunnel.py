#!/usr/bin/env python3
"""Quick test: MQTT over WSS through Cloudflare Tunnel."""

import ssl
import sys
import time
import paho.mqtt.client as mqtt

# Usage: python test_tunnel.py [local|tunnel]
mode = sys.argv[1] if len(sys.argv) > 1 else "tunnel"

if mode == "local":
    BROKER, PORT, TLS = "localhost", 9001, False
    TRANSPORT_DESC = f"ws://{BROKER}:{PORT}"
else:
    BROKER, PORT, TLS = "mqtt.hoshinoht.dev", 443, True
    TRANSPORT_DESC = f"wss://{BROKER}:{PORT}"

TOPIC = "flp/test/tunnel"
MSG = "hello from tunnel test"
received = False

def on_connect(client, _ud, _flags, rc, _props=None):
    if rc == 0:
        print(f"[OK] Connected to {TRANSPORT_DESC}")
        client.subscribe(TOPIC, qos=1)
    else:
        print(f"[FAIL] Connect error code: {rc}")

def on_message(_client, _ud, msg):
    global received
    print(f"[OK] Received on '{msg.topic}': {msg.payload.decode()}")
    received = True

def on_subscribe(_client, _ud, mid, granted_qos, _props=None):
    print(f"[OK] Subscribed, publishing test message...")
    client.publish(TOPIC, MSG, qos=1)

client = mqtt.Client(
    callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
    client_id="tunnel-test",
    transport="websockets",
    protocol=mqtt.MQTTv311,
)
if TLS:
    client.tls_set(certfile=None, keyfile=None, cert_reqs=ssl.CERT_REQUIRED)

client.on_connect = on_connect
client.on_message = on_message
client.on_subscribe = on_subscribe

print(f"Connecting to {TRANSPORT_DESC} ...")
client.connect(BROKER, PORT)
client.loop_start()

deadline = time.time() + 10
while not received and time.time() < deadline:
    time.sleep(0.2)

client.loop_stop()
client.disconnect()

if received:
    print(f"\n[PASS] {'Tunnel' if TLS else 'Local WS'} round-trip OK")
else:
    print(f"\n[FAIL] No message received within 10s")
