#!/usr/bin/env python3
"""
Forest Link Protocol v3.7 — Python MQTT Admin

Handles cloud-side file transfer flow control (FR-MQTT1 through FR-MQTT4).
Runs alongside a Mosquitto broker to manage incoming file transfers from
mesh exit nodes, using Selective Repeat ARQ for reliable chunk delivery
and a FIFO queue to serialize concurrent transfer requests.
"""

import json
import struct
import sys
import threading
import time

import paho.mqtt.client as mqtt

from selective_repeat import CloudSelectiveRepeat
from file_reassembler import FileReassembler, CHUNK_SIZE
from transfer_queue import TransferQueue

BROKER_HOST = "localhost"
BROKER_PORT = 1883
CLIENT_ID = "flp-admin"


class FlpMqttAdmin:
    def __init__(self, broker_host=BROKER_HOST, broker_port=BROKER_PORT):
        self.broker_host = broker_host
        self.broker_port = broker_port

        # MQTT client (FR-MQTT1)
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1,
                                  client_id=CLIENT_ID, protocol=mqtt.MQTTv311)
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.on_disconnect = self._on_disconnect

        # Transfer management
        self.transfer_queue = TransferQueue()
        self.transfer_queue.cmd_callback = self._send_command

        self.sr = CloudSelectiveRepeat()
        self.sr.ack_callback = self._send_ack_nack

        self.reassembler = None  # created per transfer

        # Node tracking
        self.nodes = {}  # node_id -> last status

        self._running = False
        self._lock = threading.Lock()

    def _on_connect(self, client, userdata, flags, rc, properties=None):
        if rc == 0:
            print(
                f"[MQTT] Connected to broker at {self.broker_host}:{self.broker_port}")
            # FR-MQTT1: Subscribe to node topics
            client.subscribe("flp/+/file/meta", qos=1)
            client.subscribe("flp/+/file/data", qos=1)
            client.subscribe("flp/+/status", qos=0)
            print("[MQTT] Subscribed to flp/+/file/meta, flp/+/file/data, flp/+/status")
        else:
            print(f"[MQTT] Connection failed with rc={rc}")

    def _on_disconnect(self, client, userdata, rc, properties=None):
        print(f"[MQTT] Disconnected (rc={rc})")

    def _on_message(self, client, userdata, msg):
        topic = msg.topic
        payload = msg.payload

        # Parse node_id from topic: flp/<node_id>/...
        parts = topic.split("/")
        if len(parts) < 3:
            return

        node_id = parts[1]
        sub_topic = "/".join(parts[2:])

        if sub_topic == "status":
            self._handle_status(node_id, payload)
        elif sub_topic == "file/meta":
            self._handle_file_meta(node_id, payload)
        elif sub_topic == "file/data":
            self._handle_file_data(node_id, payload)

    def _handle_status(self, node_id, payload):
        """Handle node heartbeat/status."""
        try:
            status = json.loads(payload)
            self.nodes[node_id] = {**status, "last_seen": time.time()}
            print(f"[Status] Node {node_id}: {status}")
        except json.JSONDecodeError:
            print(
                f"[Status] Node {node_id}: {payload.decode('utf-8', errors='replace')}")

    def _handle_file_meta(self, node_id, payload):
        """Handle file transfer metadata — enqueue transfer."""
        try:
            meta = json.loads(payload)
            session_id = meta["session_id"]
            filename = meta["filename"]
            total_size = meta["total_size"]
            chunk_count = meta["chunk_count"]
            crc32 = meta.get("crc32", 0)

            print(
                f"[Meta] File transfer from {node_id}: {filename} ({total_size} bytes, {chunk_count} chunks)")

            with self._lock:
                already_active = (
                    self.transfer_queue.active_transfer is not None
                    and self.transfer_queue.active_transfer.session_id == session_id
                )
                session = self.transfer_queue.enqueue(
                    session_id, node_id, filename, total_size, chunk_count, crc32)

                # Set up reassembler and SR only when this session first becomes
                # active. If it was already active before enqueue() returned,
                # another exit node is just joining an in-progress transfer —
                # skip re-initialisation to avoid resetting receiver state.
                if (
                    not already_active
                    and self.transfer_queue.active_transfer is not None
                    and self.transfer_queue.active_transfer.session_id == session_id
                ):
                    self._setup_active_transfer(session)
        except (json.JSONDecodeError, KeyError) as e:
            print(f"[Meta] Error parsing metadata from {node_id}: {e}")

    def _setup_active_transfer(self, session):
        """Initialize reassembler and SR for active transfer."""
        self.reassembler = FileReassembler(
            session_id=session.session_id,
            filename=session.filename,
            total_size=session.total_size,
            chunk_count=session.chunk_count,
            expected_crc=session.crc32
        )
        self.sr.start_session(session.chunk_count)
        print(
            f"[Transfer] Active: {session.filename} — waiting for {session.chunk_count} chunks")

    def _handle_file_data(self, node_id, payload):
        """Handle file data chunk."""
        with self._lock:
            if not self.reassembler or not self.transfer_queue.active_transfer:
                return

            if len(payload) < 4:
                return

            # First 2 bytes = sequence number (little-endian), rest = data
            seq_num = struct.unpack("<H", payload[:2])[0]
            chunk_data = payload[2:]

            # Write to reassembler
            is_new = self.reassembler.write_chunk(seq_num, chunk_data)

            if is_new:
                # Notify selective repeat
                self.sr.on_chunk_received(seq_num)

                # Progress update
                progress = self.reassembler.progress()
                if int(progress) % 10 == 0 and int(progress) != 0:
                    print(
                        f"[Transfer] {self.reassembler.filename}: {progress:.1f}% ({self.reassembler.chunks_received}/{self.reassembler.chunk_count})")

            # Check completion
            if self.reassembler.is_complete():
                self._complete_transfer()

    def _complete_transfer(self):
        """Handle completed file transfer."""
        if self.reassembler.verify_crc():
            path = self.reassembler.save()
            print(
                f"[Transfer] SUCCESS: {self.reassembler.filename} saved to {path} (CRC verified)")
        else:
            path = self.reassembler.save()
            print(
                f"[Transfer] WARNING: {self.reassembler.filename} saved to {path} (CRC MISMATCH)")

        self.reassembler = None
        self.transfer_queue.complete_active()

        # Set up next transfer if queued
        if self.transfer_queue.active_transfer:
            self._setup_active_transfer(self.transfer_queue.active_transfer)

    def _send_ack_nack(self, msg_type, seq_num):
        """Publish ACK or NACK to flp/admin/ack (FR-MQTT2)."""
        payload = json.dumps({"type": msg_type, "seq": seq_num})
        self.client.publish("flp/admin/ack", payload, qos=1)

    def _send_command(self, node_id, command, session_id):
        """Publish control command to flp/admin/cmd."""
        payload = json.dumps(
            {"command": command, "node_id": node_id, "session_id": session_id})
        self.client.publish("flp/admin/cmd", payload, qos=1)
        print(
            f"[Cmd] Sent {command} to node {node_id} for session {session_id}")

    def run(self):
        """Main loop."""
        self._running = True

        print(
            f"[Admin] Connecting to MQTT broker at {self.broker_host}:{self.broker_port}...")
        self.client.connect(self.broker_host, self.broker_port, keepalive=60)
        self.client.loop_start()

        print("[Admin] FLP MQTT Admin running. Press Ctrl+C to stop.")

        try:
            while self._running:
                # Periodic SR timeout check
                with self._lock:
                    if self.sr.received_bitmap is not None:
                        self.sr.check_timeouts()

                # Print status every 30s
                time.sleep(1)
        except KeyboardInterrupt:
            print("\n[Admin] Shutting down...")
        finally:
            self.client.loop_stop()
            self.client.disconnect()
            self._print_summary()

    def _print_summary(self):
        """Print session summary."""
        status = self.transfer_queue.status()
        print(f"\n{'='*50}")
        print(f"Session Summary:")
        print(f"  Transfers completed: {status['completed']}")
        print(f"  Transfers pending:   {status['pending']}")
        print(f"  Active transfer:     {status['active'] or 'None'}")
        print(f"  Known nodes:         {len(self.nodes)}")
        print(f"{'='*50}")


def main():
    import argparse
    parser = argparse.ArgumentParser(
        description="FLP MQTT Admin — Cloud-side file transfer manager")
    parser.add_argument("--broker", "-b", default="localhost",
                        help="MQTT broker hostname (default: localhost)")
    parser.add_argument("--port", "-p", type=int, default=1883,
                        help="MQTT broker port (default: 1883)")
    args = parser.parse_args()

    admin = FlpMqttAdmin(broker_host=args.broker, broker_port=args.port)
    admin.run()


if __name__ == "__main__":
    main()
