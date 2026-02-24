#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────
# Forest Link Protocol — Raspberry Pi Deployment Script
#
# Deploys:
#   1. Mosquitto MQTT broker (listening on port 1883)
#   2. MQTT-SN Gateway (paho.mqtt-sn.embedded-c, UDP 1885 → MQTT 1883)
#   3. FLP MQTT Admin (Python cloud-side file transfer manager)
#
# Tested on: Raspberry Pi OS (Bookworm), Ubuntu 22.04+, Ultramarine (Fedora)
# Usage:    bash deploy.sh [--install | --start | --stop | --status]
# ─────────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MQTTSN_GW_DIR="/opt/mqtt-sn-gateway"
MQTTSN_GW_REPO="https://github.com/eclipse/paho.mqtt-sn.embedded-c.git"
MQTTSN_GW_CONF="/etc/mqtt-sn-gateway.conf"
MOSQUITTO_CONF="/etc/mosquitto/conf.d/flp.conf"

# Ports
MQTT_PORT=1883
MQTTSN_PORT=1885

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }

# ── Checks ──────────────────────────────────────────────────────────
require_root() {
    if [[ $EUID -ne 0 ]]; then
        error "This command must be run as root (use sudo)."
        exit 1
    fi
}

DISTRO_FAMILY=""  # "debian" or "fedora"

detect_distro() {
    if [[ ! -f /etc/os-release ]]; then
        error "Unsupported OS — /etc/os-release not found."
        exit 1
    fi
    . /etc/os-release

    case "$ID" in
        raspbian|debian|ubuntu)
            DISTRO_FAMILY="debian" ;;
        fedora|ultramarine)
            DISTRO_FAMILY="fedora" ;;
        *)
            # Check ID_LIKE for derivatives
            if [[ "${ID_LIKE:-}" == *"debian"* || "${ID_LIKE:-}" == *"ubuntu"* ]]; then
                DISTRO_FAMILY="debian"
            elif [[ "${ID_LIKE:-}" == *"fedora"* ]]; then
                DISTRO_FAMILY="fedora"
            else
                error "Unsupported OS: $PRETTY_NAME"
                error "This script supports Debian/Ubuntu and Fedora/Ultramarine-based systems."
                exit 1
            fi
            ;;
    esac
    info "Detected OS: $PRETTY_NAME (${DISTRO_FAMILY} family)"
}

pkg_update() {
    case "$DISTRO_FAMILY" in
        debian) apt-get update -qq ;;
        fedora) dnf check-update -q || true ;;  # returns 100 when updates available
    esac
}

pkg_install() {
    case "$DISTRO_FAMILY" in
        debian) apt-get install -y -qq "$@" ;;
        fedora) dnf install -y -q "$@" ;;
    esac
}

# ── Install ─────────────────────────────────────────────────────────
cmd_install() {
    require_root
    detect_distro

    info "Updating package lists..."
    pkg_update

    # ── 1. Mosquitto ────────────────────────────────────────────────
    info "Installing Mosquitto MQTT broker..."
    case "$DISTRO_FAMILY" in
        debian) pkg_install mosquitto mosquitto-clients ;;
        fedora) pkg_install mosquitto ;;  # clients included in main package
    esac

    mkdir -p "$(dirname "${MOSQUITTO_CONF}")"
    # Ensure mosquitto.conf includes the conf.d/ directory
    if [[ -f /etc/mosquitto/mosquitto.conf ]] && \
       ! grep -q "include_dir /etc/mosquitto/conf.d" /etc/mosquitto/mosquitto.conf; then
        echo "include_dir /etc/mosquitto/conf.d" >> /etc/mosquitto/mosquitto.conf
    fi
    info "Writing FLP Mosquitto config to ${MOSQUITTO_CONF}..."
    cat > "${MOSQUITTO_CONF}" <<'EOF'
# Forest Link Protocol — Mosquitto configuration
listener 1883 0.0.0.0
allow_anonymous true
max_queued_messages 1000
EOF

    systemctl enable mosquitto
    systemctl restart mosquitto
    info "Mosquitto installed and running on port ${MQTT_PORT}."

    # ── 2. MQTT-SN Gateway (Eclipse Paho) ──────────────────────────
    info "Installing build dependencies for MQTT-SN gateway..."
    case "$DISTRO_FAMILY" in
        debian) pkg_install git cmake g++ libssl-dev ;;
        fedora) pkg_install git cmake gcc-c++ openssl-devel ;;
    esac

    if [[ -d "${MQTTSN_GW_DIR}" ]]; then
        info "MQTT-SN gateway source already exists at ${MQTTSN_GW_DIR}, pulling latest..."
        git -C "${MQTTSN_GW_DIR}" pull --ff-only || true
    else
        info "Cloning MQTT-SN gateway from ${MQTTSN_GW_REPO}..."
        git clone "${MQTTSN_GW_REPO}" "${MQTTSN_GW_DIR}"
    fi

    # Build MQTTSNPacket library first (gateway links against it)
    info "Building MQTTSNPacket library..."
    cd "${MQTTSN_GW_DIR}/MQTTSNPacket"
    rm -rf build && mkdir build && cd build
    cmake .. -DBUILD_TESTING=OFF
    make -j"$(nproc)" MQTTSNPacket

    info "Building MQTT-SN gateway..."
    cd "${MQTTSN_GW_DIR}/MQTTSNGateway"
    rm -rf build && mkdir build && cd build
    cmake .. -DUDP=ON -DDTLS=OFF
    make -j"$(nproc)"
    info "MQTT-SN gateway built successfully."

    # Install binary
    cp -f MQTT-SNGateway /usr/local/bin/mqtt-sn-gateway
    chmod +x /usr/local/bin/mqtt-sn-gateway

    # Write config
    info "Writing MQTT-SN gateway config to ${MQTTSN_GW_CONF}..."
    cat > "${MQTTSN_GW_CONF}" <<EOF
# MQTT-SN Gateway configuration for FLP
BrokerName=localhost
BrokerPortNo=${MQTT_PORT}
BrokerSecurePortNo=8883

GatewayID=1
GatewayName=FLP-MQTTSN-GW
MaxNumberOfClients=30

GatewayPortNo=${MQTTSN_PORT}
MulticastIP=225.1.1.1
MulticastPortNo=1886

# Keepalive (seconds)
KeepAlive=60
EOF

    # Create systemd unit
    cat > /etc/systemd/system/mqtt-sn-gateway.service <<EOF
[Unit]
Description=MQTT-SN Gateway for Forest Link Protocol
After=network.target mosquitto.service
Requires=mosquitto.service

[Service]
Type=simple
ExecStart=/usr/local/bin/mqtt-sn-gateway ${MQTTSN_GW_CONF}
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

    systemctl daemon-reload
    systemctl enable mqtt-sn-gateway
    systemctl restart mqtt-sn-gateway
    info "MQTT-SN gateway installed and running (UDP ${MQTTSN_PORT} → MQTT ${MQTT_PORT})."

    # ── 3. Python dependencies for MQTT Admin ──────────────────────
    info "Installing Python 3 and pip..."
    case "$DISTRO_FAMILY" in
        debian) pkg_install python3 python3-pip python3-venv ;;
        fedora) pkg_install python3 python3-pip ;;  # venv included in python3
    esac

    info "Setting up Python virtual environment..."
    if [[ ! -d "${SCRIPT_DIR}/.venv" ]]; then
        python3 -m venv "${SCRIPT_DIR}/.venv"
    fi
    "${SCRIPT_DIR}/.venv/bin/pip" install --quiet -r "${SCRIPT_DIR}/requirements.txt"

    # Create systemd unit for MQTT Admin
    cat > /etc/systemd/system/flp-mqtt-admin.service <<EOF
[Unit]
Description=FLP MQTT Admin — Cloud-side file transfer manager
After=network.target mosquitto.service mqtt-sn-gateway.service
Requires=mosquitto.service

[Service]
Type=simple
WorkingDirectory=${SCRIPT_DIR}
ExecStart=${SCRIPT_DIR}/.venv/bin/python3 ${SCRIPT_DIR}/mqtt_admin.py --broker localhost --port ${MQTT_PORT}
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

    systemctl daemon-reload
    systemctl enable flp-mqtt-admin
    systemctl restart flp-mqtt-admin
    info "FLP MQTT Admin installed and running."

    echo ""
    info "═══════════════════════════════════════════════════════"
    info " Deployment complete!"
    info ""
    info "  Mosquitto broker:     port ${MQTT_PORT} (TCP)"
    info "  MQTT-SN gateway:      port ${MQTTSN_PORT} (UDP) → ${MQTT_PORT} (TCP)"
    info "  FLP MQTT Admin:       connected to localhost:${MQTT_PORT}"
    info ""
    info " Manage services:"
    info "   sudo systemctl {start|stop|status} mosquitto"
    info "   sudo systemctl {start|stop|status} mqtt-sn-gateway"
    info "   sudo systemctl {start|stop|status} flp-mqtt-admin"
    info ""
    info " View logs:"
    info "   journalctl -u mosquitto -f"
    info "   journalctl -u mqtt-sn-gateway -f"
    info "   journalctl -u flp-mqtt-admin -f"
    info "═══════════════════════════════════════════════════════"
}

# ── Start / Stop / Status ───────────────────────────────────────────
cmd_start() {
    require_root
    info "Starting all FLP services..."
    systemctl start mosquitto
    systemctl start mqtt-sn-gateway 2>/dev/null || warn "mqtt-sn-gateway not installed, skipping"
    systemctl start flp-mqtt-admin
    info "All services started."
}

cmd_stop() {
    require_root
    info "Stopping all FLP services..."
    systemctl stop flp-mqtt-admin || true
    systemctl stop mqtt-sn-gateway || true
    systemctl stop mosquitto || true
    info "All services stopped."
}

cmd_status() {
    echo "── Mosquitto ──"
    systemctl status mosquitto --no-pager -l 2>/dev/null || echo "  Not installed"
    echo ""
    echo "── MQTT-SN Gateway ──"
    systemctl status mqtt-sn-gateway --no-pager -l 2>/dev/null || echo "  Not installed"
    echo ""
    echo "── FLP MQTT Admin ──"
    systemctl status flp-mqtt-admin --no-pager -l 2>/dev/null || echo "  Not installed"
}

# ── Uninstall ───────────────────────────────────────────────────────
cmd_uninstall() {
    require_root
    warn "This will stop and remove all FLP services."
    read -rp "Continue? [y/N] " confirm
    [[ "$confirm" =~ ^[Yy]$ ]] || exit 0

    cmd_stop

    systemctl disable flp-mqtt-admin 2>/dev/null || true
    systemctl disable mqtt-sn-gateway 2>/dev/null || true
    rm -f /etc/systemd/system/flp-mqtt-admin.service
    rm -f /etc/systemd/system/mqtt-sn-gateway.service
    rm -f /usr/local/bin/mqtt-sn-gateway
    rm -f "${MQTTSN_GW_CONF}"
    rm -f "${MOSQUITTO_CONF}"
    rm -rf "${MQTTSN_GW_DIR}"
    rm -rf "${SCRIPT_DIR}/.venv"
    systemctl daemon-reload

    info "FLP services removed. Mosquitto package left installed (remove with apt-get remove mosquitto)."
}

# ── Main ────────────────────────────────────────────────────────────
usage() {
    echo "Usage: sudo bash deploy.sh <command>"
    echo ""
    echo "Commands:"
    echo "  --install     Install and start Mosquitto, MQTT-SN gateway, and FLP Admin"
    echo "  --start       Start all FLP services"
    echo "  --stop        Stop all FLP services"
    echo "  --status      Show status of all FLP services"
    echo "  --uninstall   Remove all FLP services and configs"
}

case "${1:-}" in
    --install)   cmd_install ;;
    --start)     cmd_start ;;
    --stop)      cmd_stop ;;
    --status)    cmd_status ;;
    --uninstall) cmd_uninstall ;;
    *)           usage; exit 1 ;;
esac
