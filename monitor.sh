#!/usr/bin/env bash
# monitor.sh - Auto-detect ESP32 USB serial ports and monitor side-by-side
#
# Usage:
#   ./monitor.sh              Monitor all detected devices
#   ./monitor.sh exit relay   Label pane 1 as "exit", pane 2 as "relay"
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FLP_NODE_DIR="$SCRIPT_DIR/flp-node"

# ── Detect ports ────────────────────────────────────────────────
ports=()
for p in /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.SLAB*; do
  [ -e "$p" ] && ports+=("$p")
done

if [ ${#ports[@]} -eq 0 ]; then
  echo "Error: No ESP32 USB serial ports found."
  echo "       Make sure devices are plugged in."
  exit 1
fi

if [ ${#ports[@]} -eq 1 ]; then
  LABEL="${1:-device}"
  echo "[$LABEL] ${ports[0]}"
  cd "$FLP_NODE_DIR"
  exec "$FLP_NODE_DIR/idf" monitor -p "${ports[0]}"
fi

# Labels from args, or default to port basenames
LABEL1="${1:-$(basename "${ports[0]}")}"
LABEL2="${2:-$(basename "${ports[1]}")}"
PORT1="${ports[0]}"
PORT2="${ports[1]}"

echo "Detected ports:"
echo "  $LABEL1 -> $PORT1"
echo "  $LABEL2 -> $PORT2"
echo ""

# ── Launch monitors ─────────────────────────────────────────────
# Build the command strings with variables expanded now
CMD1="cd \"$FLP_NODE_DIR\" && echo '── $LABEL1: $PORT1 ──' && \"$FLP_NODE_DIR/idf\" monitor -p \"$PORT1\"; read"
CMD2="cd \"$FLP_NODE_DIR\" && echo '── $LABEL2: $PORT2 ──' && \"$FLP_NODE_DIR/idf\" monitor -p \"$PORT2\"; read"

if command -v tmux &>/dev/null; then
  SESSION="flp-monitor"
  tmux kill-session -t "$SESSION" 2>/dev/null || true

  tmux new-session -d -s "$SESSION" -x "$(tput cols)" -y "$(tput lines)" "$CMD1"
  tmux split-window -h -t "$SESSION" "$CMD2"

  tmux select-layout -t "$SESSION" even-horizontal
  tmux attach -t "$SESSION"
else
  echo "tmux not found. Opening two Terminal.app windows instead..."
  osascript <<EOF
    tell application "Terminal"
      activate
      do script "$CMD1"
      do script "$CMD2"
    end tell
EOF
  echo "Opened two Terminal windows."
fi
