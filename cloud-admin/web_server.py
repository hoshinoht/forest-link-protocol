"""Flask web server for FLP dashboard."""
import threading
from flask import Flask, jsonify, send_from_directory

app = Flask(__name__, static_folder="static")

# References set by mqtt_admin before start
_topology = None
_metrics_store = None
_admin = None  # FlpMqttAdmin instance for sending commands


def init(topology_agg, metrics_store=None, admin=None):
    global _topology, _metrics_store, _admin
    _topology = topology_agg
    _metrics_store = metrics_store
    _admin = admin


@app.route("/")
def index():
    return send_from_directory(app.static_folder, "index.html")


@app.route("/api/topology")
def api_topology():
    if _topology:
        return jsonify(_topology.to_json())
    return jsonify({"nodes": [], "edges": []})


@app.route("/api/metrics/<node_id>")
def api_metrics(node_id):
    from flask import request
    range_sec = int(request.args.get("range", 3600))
    if _metrics_store:
        return jsonify(_metrics_store.query_node(node_id, range_sec))
    return jsonify([])


@app.route("/api/transfers")
def api_transfers():
    if _metrics_store:
        return jsonify(_metrics_store.query_transfers())
    return jsonify([])


@app.route("/api/heap/<node_id>")
def api_heap(node_id):
    from flask import request
    range_sec = int(request.args.get("range", 3600))
    if _metrics_store:
        return jsonify(_metrics_store.query_heap(node_id, range_sec))
    return jsonify([])


@app.route("/api/cmd/<node_id>", methods=["POST"])
def api_send_cmd(node_id):
    """Send a command to a mesh node. JSON body: {"cmd": int, "data": "hex"}"""
    from flask import request
    if not _admin:
        return jsonify({"error": "admin not available"}), 503
    body = request.get_json(silent=True) or {}
    cmd_id = int(body.get("cmd", 1))
    data_hex = body.get("data", "")
    data = bytes.fromhex(data_hex) if data_hex else b''
    _admin.send_node_command(node_id, cmd_id, data)
    return jsonify({"ok": True, "target": node_id, "cmd": cmd_id})


@app.route("/api/topic_msg/<node_id>", methods=["POST"])
def api_send_topic_msg(node_id):
    """Send a topic-addressed message to a mesh node.
    JSON body: {"topic": "config", "data": "hex"}
    """
    from flask import request
    if not _admin:
        return jsonify({"error": "admin not available"}), 503
    body = request.get_json(silent=True) or {}
    topic = body.get("topic", "")
    if not topic:
        return jsonify({"error": "topic required"}), 400
    data_hex = body.get("data", "")
    data = bytes.fromhex(data_hex) if data_hex else b''
    _admin.send_topic_msg(node_id, topic, data)
    return jsonify({"ok": True, "target": node_id, "topic": topic})


@app.route("/api/comparison")
def api_comparison():
    import json, os
    benchmarks = []
    bench_dir = os.path.join(os.path.dirname(__file__), "benchmarks")
    if os.path.isdir(bench_dir):
        for fname in sorted(os.listdir(bench_dir)):
            if fname.endswith(".json"):
                with open(os.path.join(bench_dir, fname)) as f:
                    benchmarks.append(json.load(f))
    # Add FLP derived metrics if available
    if _metrics_store:
        flp = _metrics_store.derived_metrics()
        if flp:
            benchmarks.insert(0, flp)
    return jsonify(benchmarks)


def start(host="0.0.0.0", port=5000):
    t = threading.Thread(target=lambda: app.run(host=host, port=port, debug=False), daemon=True)
    t.start()
    print(f"[Web] Dashboard running at http://{host}:{port}/")
