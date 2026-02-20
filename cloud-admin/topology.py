"""Topology Aggregator — Parses binary neighbor reports, builds network graph."""
import struct
import time


class TopologyAggregator:
    def __init__(self):
        self.nodes = {}  # node_id -> {last_seen, neighbors}

    def update(self, node_id: str, payload: bytes):
        """Parse binary topology report and update node state."""
        if len(payload) < 1:
            return
        count = payload[0]
        neighbors = []
        for i in range(count):
            off = 1 + i * 6
            if off + 6 > len(payload):
                break
            addr = struct.unpack_from("<H", payload, off)[0]
            rssi = struct.unpack_from("b", payload, off + 2)[0]
            hops = payload[off + 3]
            hops_inet = payload[off + 4]
            flags = payload[off + 5]
            neighbors.append({
                "addr": f"{addr:04X}",
                "rssi": rssi,
                "hops": hops,
                "hops_to_internet": hops_inet,
                "ble": bool(flags & 0x01),
                "lora": bool(flags & 0x02),
                "has_internet": bool(flags & 0x04),
            })
        existing = self.nodes.get(node_id, {})
        self.nodes[node_id] = {
            "last_seen": time.time(),
            "neighbors": neighbors,
            "heap": existing.get("heap"),
        }

    def to_json(self) -> dict:
        """Export graph as JSON for vis.js visualization."""
        now = time.time()
        nodes = []
        edges = []
        seen_edges = set()

        for nid, info in self.nodes.items():
            status = "online" if (now - info["last_seen"]) < 60 else "offline"
            min_hops = 255
            for n in info["neighbors"]:
                if n["hops_to_internet"] < min_hops:
                    min_hops = n["hops_to_internet"]
            node_entry = {"id": nid, "status": status, "hops_to_internet": min_hops}
            if info.get("heap"):
                node_entry["heap"] = info["heap"]
            nodes.append(node_entry)

            for n in info["neighbors"]:
                edge_key = tuple(sorted([nid, n["addr"]]))
                if edge_key not in seen_edges:
                    seen_edges.add(edge_key)
                    transport = "BLE" if n["ble"] else ("LoRa" if n["lora"] else "unknown")
                    edges.append({
                        "source": nid, "target": n["addr"],
                        "rssi": n["rssi"], "transport": transport,
                    })

        return {"nodes": nodes, "edges": edges}

    def update_heap(self, node_id: str, payload: bytes):
        """Parse 20-byte binary heap report and attach to node state."""
        if len(payload) < 20:
            return
        free_int, free_ps, min_int, min_ps, largest = struct.unpack_from("<5I", payload)
        heap = {
            "free_internal": free_int,
            "free_psram": free_ps,
            "min_internal": min_int,
            "min_psram": min_ps,
            "largest_block": largest,
        }
        if node_id in self.nodes:
            self.nodes[node_id]["heap"] = heap
        else:
            self.nodes[node_id] = {"last_seen": time.time(), "neighbors": [], "heap": heap}
