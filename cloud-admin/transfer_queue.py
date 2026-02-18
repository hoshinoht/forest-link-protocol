"""
Transfer Queue — FIFO queue with single active transfer (FR-MQTT3).

Ensures only one file transfer is active at a time. Queued transfers
are started automatically when the active transfer completes.
"""

import collections
import dataclasses
import time


@dataclasses.dataclass
class TransferSession:
    session_id: str
    node_id: str
    filename: str
    total_size: int
    chunk_count: int
    crc32: int
    started_at: float = 0.0
    completed_at: float = 0.0


class TransferQueue:
    def __init__(self):
        self.queue = collections.deque()
        self.active_transfer = None
        self.completed = []
        self.cmd_callback = None  # set by mqtt_admin to publish commands

    def enqueue(self, session_id, node_id, filename, total_size, chunk_count, crc32):
        """Add a transfer to the queue. Starts immediately if no active transfer."""
        session = TransferSession(
            session_id=session_id, node_id=node_id, filename=filename,
            total_size=total_size, chunk_count=chunk_count, crc32=crc32
        )
        self.queue.append(session)
        print(
            f"[Queue] Enqueued transfer: {filename} from node {node_id} ({total_size} bytes, {chunk_count} chunks)")
        if self.active_transfer is None:
            self.start_next()
        return session

    def start_next(self):
        """Start the next queued transfer if none is active."""
        if self.active_transfer is not None or not self.queue:
            return None
        self.active_transfer = self.queue.popleft()
        self.active_transfer.started_at = time.time()
        print(
            f"[Queue] Starting transfer: {self.active_transfer.filename} from {self.active_transfer.node_id}")
        # Send START command to node
        if self.cmd_callback:
            self.cmd_callback(self.active_transfer.node_id,
                              "START", self.active_transfer.session_id)
        return self.active_transfer

    def complete_active(self):
        """Mark the active transfer as complete and start the next one."""
        if self.active_transfer:
            self.active_transfer.completed_at = time.time()
            elapsed = self.active_transfer.completed_at - self.active_transfer.started_at
            print(
                f"[Queue] Transfer complete: {self.active_transfer.filename} in {elapsed:.1f}s")
            self.completed.append(self.active_transfer)
            self.active_transfer = None
            self.start_next()

    def pending_count(self):
        """Return the number of pending transfers."""
        return len(self.queue)

    def status(self):
        """Return a summary of queue state."""
        return {
            "active": self.active_transfer.filename if self.active_transfer else None,
            "pending": len(self.queue),
            "completed": len(self.completed),
        }
