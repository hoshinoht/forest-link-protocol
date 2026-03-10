#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────
# Forest Link Protocol — Deployment Script
#
# Deploys:
#   1. Mosquitto MQTT broker (listening on port 1883)
#   2. MQTT-SN Gateway (paho.mqtt-sn.embedded-c, UDP 1885 → MQTT 1883)
#   3. FLP MQTT Admin (Python cloud-side file transfer manager)
#
# Tested on: Raspberry Pi OS (Bookworm), Ubuntu 22.04+, Ultramarine (Fedora), macOS
# Usage:    bash deploy.sh [--install | --start | --stop | --status | --uninstall]
# ─────────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MQTTSN_GW_DIR="${SCRIPT_DIR}/mqtt-sn-gateway"
MQTTSN_GW_REPO="https://github.com/eclipse/paho.mqtt-sn.embedded-c.git"

# Ports
MQTT_PORT=1883
MQTTSN_PORT=1885

# Set by detect_distro
OS_TYPE=""           # "macos" or "linux"
DISTRO_FAMILY=""     # "debian", "fedora", or "macos"
MOSQUITTO_CONF=""
MQTTSN_GW_CONF=""
BREW_PREFIX=""

# LaunchAgent plist paths (macOS)
PLIST_MQTTSN="$HOME/Library/LaunchAgents/com.flp.mqtt-sn-gateway.plist"
PLIST_ADMIN="$HOME/Library/LaunchAgents/com.flp.mqtt-admin.plist"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }

# ── Checks ──────────────────────────────────────────────────────────
require_root() {
    if [[ "$OS_TYPE" == "macos" ]]; then
        if [[ $EUID -eq 0 ]]; then
            error "Do not run this script as root on macOS."
            error "Homebrew refuses to run as root. Run without sudo:"
            error "  bash deploy.sh --install"
            exit 1
        fi
        return 0
    fi
    if [[ $EUID -ne 0 ]]; then
        error "This command must be run as root (use sudo)."
        exit 1
    fi
}

detect_distro() {
    if [[ "$(uname)" == "Darwin" ]]; then
        OS_TYPE="macos"
        DISTRO_FAMILY="macos"
        if ! command -v brew &>/dev/null; then
            error "Homebrew is required on macOS. Install from https://brew.sh"
            exit 1
        fi
        BREW_PREFIX="$(brew --prefix)"
        MOSQUITTO_CONF="${BREW_PREFIX}/etc/mosquitto/conf.d/flp.conf"
        MQTTSN_GW_CONF="/usr/local/etc/mqtt-sn-gateway.conf"
        info "Detected OS: macOS ($(sw_vers -productVersion))"
        return
    fi

    OS_TYPE="linux"
    MOSQUITTO_CONF="/etc/mosquitto/conf.d/flp.conf"
    MQTTSN_GW_CONF="/etc/mqtt-sn-gateway.conf"

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
            if [[ "${ID_LIKE:-}" == *"debian"* || "${ID_LIKE:-}" == *"ubuntu"* ]]; then
                DISTRO_FAMILY="debian"
            elif [[ "${ID_LIKE:-}" == *"fedora"* ]]; then
                DISTRO_FAMILY="fedora"
            else
                error "Unsupported OS: $PRETTY_NAME"
                error "This script supports Debian/Ubuntu, Fedora/Ultramarine, and macOS."
                exit 1
            fi
            ;;
    esac
    info "Detected OS: $PRETTY_NAME (${DISTRO_FAMILY} family)"
}

pkg_update() {
    case "$DISTRO_FAMILY" in
        debian) apt-get update -qq ;;
        fedora) dnf check-update -q || true ;;
        macos)  brew update --quiet ;;
    esac
}

pkg_install() {
    case "$DISTRO_FAMILY" in
        debian) apt-get install -y -qq "$@" ;;
        fedora) dnf install -y -q "$@" ;;
        macos)  brew install "$@" ;;
    esac
}

# Helper: number of CPU cores
ncpu() {
    if [[ "$OS_TYPE" == "macos" ]]; then
        sysctl -n hw.ncpu
    else
        nproc
    fi
}

# ── Install ─────────────────────────────────────────────────────────
cmd_install() {
    detect_distro
    require_root

    info "Updating package lists..."
    pkg_update

    # ── 1. Mosquitto ────────────────────────────────────────────────
    info "Installing Mosquitto MQTT broker..."
    case "$DISTRO_FAMILY" in
        debian) pkg_install mosquitto mosquitto-clients ;;
        fedora) pkg_install mosquitto ;;
        macos)  pkg_install mosquitto ;;
    esac

    mkdir -p "$(dirname "${MOSQUITTO_CONF}")"

    if [[ "$OS_TYPE" == "macos" ]]; then
        local mosq_conf="${BREW_PREFIX}/etc/mosquitto/mosquitto.conf"
        if ! grep -q "include_dir ${BREW_PREFIX}/etc/mosquitto/conf.d" "$mosq_conf" 2>/dev/null; then
            echo "include_dir ${BREW_PREFIX}/etc/mosquitto/conf.d" >> "$mosq_conf"
        fi
    else
        if [[ -f /etc/mosquitto/mosquitto.conf ]] && \
           ! grep -q "include_dir /etc/mosquitto/conf.d" /etc/mosquitto/mosquitto.conf; then
            echo "include_dir /etc/mosquitto/conf.d" >> /etc/mosquitto/mosquitto.conf
        fi
    fi

    info "Writing FLP Mosquitto config to ${MOSQUITTO_CONF}..."
    cat > "${MOSQUITTO_CONF}" <<'EOF'
# Forest Link Protocol — Mosquitto configuration
listener 1883 0.0.0.0
allow_anonymous true
max_queued_messages 1000
EOF

    if [[ "$OS_TYPE" == "macos" ]]; then
        brew services restart mosquitto
    else
        systemctl enable mosquitto
        systemctl restart mosquitto
    fi
    info "Mosquitto installed and running on port ${MQTT_PORT}."

    # ── 2. MQTT-SN Gateway (Eclipse Paho) ──────────────────────────
    info "Installing build dependencies for MQTT-SN gateway..."
    case "$DISTRO_FAMILY" in
        debian) pkg_install git cmake g++ libssl-dev ;;
        fedora) pkg_install git cmake gcc-c++ openssl-devel ;;
        macos)  pkg_install cmake openssl ;;
    esac

    if [[ -d "${MQTTSN_GW_DIR}/.git" ]]; then
        info "MQTT-SN gateway source already exists, pulling latest..."
        git -C "${MQTTSN_GW_DIR}" pull --ff-only || true
    else
        info "Cloning MQTT-SN gateway from ${MQTTSN_GW_REPO}..."
        rm -rf "${MQTTSN_GW_DIR}"
        git clone "${MQTTSN_GW_REPO}" "${MQTTSN_GW_DIR}"
    fi

    # Patch upstream CMakeLists.txt for CMake 4.x compatibility
    for cml in "${MQTTSN_GW_DIR}"/MQTT*/CMakeLists.txt; do
        if ! grep -q 'cmake_minimum_required' "$cml" 2>/dev/null; then
            printf '%s\n%s' "cmake_minimum_required(VERSION 3.5)" "$(cat "$cml")" > "$cml"
        fi
    done

    # Patch hardcoded OpenSSL paths for Homebrew on Apple Silicon
    if [[ "$OS_TYPE" == "macos" ]]; then
        local openssl_prefix
        openssl_prefix="$(brew --prefix openssl 2>/dev/null)"
        local gw_cmake="${MQTTSN_GW_DIR}/MQTTSNGateway/src/CMakeLists.txt"
        if [[ -n "$openssl_prefix" ]] && ! grep -q "$openssl_prefix" "$gw_cmake"; then
            sed -i '' "s|/usr/local/opt/openssl/include|${openssl_prefix}/include|g" "$gw_cmake"
            sed -i '' "s|/usr/local/opt/openssl/lib|${openssl_prefix}/lib|g" "$gw_cmake"
        fi
    fi

    # Build MQTTSNPacket library first (gateway links against it)
    info "Building MQTTSNPacket library..."
    cd "${MQTTSN_GW_DIR}/MQTTSNPacket"
    rm -rf build && mkdir build && cd build
    cmake .. -DBUILD_TESTING=OFF
    make -j"$(ncpu)" MQTTSNPacket

    # Install library to /usr/local/lib where the gateway's cmake looks
    if [[ "$OS_TYPE" == "macos" ]]; then
        sudo mkdir -p /usr/local/lib
        sudo cp -f "${MQTTSN_GW_DIR}"/MQTTSNPacket/build/src/libMQTTSNPacket.* \
              /usr/local/lib/ 2>/dev/null \
        || sudo cp -f "${MQTTSN_GW_DIR}"/MQTTSNPacket/build/libMQTTSNPacket.* \
                 /usr/local/lib/
    else
        cp -f "${MQTTSN_GW_DIR}"/MQTTSNPacket/build/src/libMQTTSNPacket.* \
              /usr/local/lib/ 2>/dev/null \
        || cp -f "${MQTTSN_GW_DIR}"/MQTTSNPacket/build/libMQTTSNPacket.* \
                 /usr/local/lib/
        ldconfig
    fi

    info "Building MQTT-SN gateway..."
    cd "${MQTTSN_GW_DIR}/MQTTSNGateway"
    rm -rf build && mkdir build && cd build
    cmake .. -DUDP=ON -DDTLS=OFF
    make -j"$(ncpu)"
    info "MQTT-SN gateway built successfully."

    # Install binary
    if [[ "$OS_TYPE" == "macos" ]]; then
        sudo cp -f "${MQTTSN_GW_DIR}/MQTTSNGateway/bin/MQTT-SNGateway" /usr/local/bin/mqtt-sn-gateway
        sudo chmod +x /usr/local/bin/mqtt-sn-gateway
    else
        cp -f "${MQTTSN_GW_DIR}/MQTTSNGateway/bin/MQTT-SNGateway" /usr/local/bin/mqtt-sn-gateway
        chmod +x /usr/local/bin/mqtt-sn-gateway
    fi

    # Write config
    info "Writing MQTT-SN gateway config to ${MQTTSN_GW_CONF}..."
    if [[ "$OS_TYPE" == "macos" ]]; then
        sudo mkdir -p "$(dirname "${MQTTSN_GW_CONF}")"
        sudo tee "${MQTTSN_GW_CONF}" > /dev/null <<EOF
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
    else
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
    fi

    # Create service
    if [[ "$OS_TYPE" == "macos" ]]; then
        mkdir -p "$HOME/Library/LaunchAgents"
        cat > "${PLIST_MQTTSN}" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.flp.mqtt-sn-gateway</string>
    <key>ProgramArguments</key>
    <array>
        <string>/usr/local/bin/mqtt-sn-gateway</string>
        <string>-f</string>
        <string>${MQTTSN_GW_CONF}</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>/tmp/mqtt-sn-gateway.log</string>
    <key>StandardErrorPath</key>
    <string>/tmp/mqtt-sn-gateway.err</string>
</dict>
</plist>
EOF
        launchctl unload "${PLIST_MQTTSN}" 2>/dev/null || true
        launchctl load "${PLIST_MQTTSN}"
    else
        cat > /etc/systemd/system/mqtt-sn-gateway.service <<EOF
[Unit]
Description=MQTT-SN Gateway for Forest Link Protocol
After=network.target mosquitto.service
Requires=mosquitto.service

[Service]
Type=simple
ExecStart=/usr/local/bin/mqtt-sn-gateway -f ${MQTTSN_GW_CONF}
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF
        systemctl daemon-reload
        systemctl enable mqtt-sn-gateway
        systemctl restart mqtt-sn-gateway
    fi
    info "MQTT-SN gateway installed and running (UDP ${MQTTSN_PORT} → MQTT ${MQTT_PORT})."

    # ── 3. Python dependencies for MQTT Admin ──────────────────────
    info "Installing Python 3 and pip..."
    case "$DISTRO_FAMILY" in
        debian) pkg_install python3 python3-pip python3-venv ;;
        fedora) pkg_install python3 python3-pip ;;
        macos)  pkg_install python3 ;;
    esac

    info "Setting up Python virtual environment..."
    if [[ ! -d "${SCRIPT_DIR}/.venv" ]]; then
        python3 -m venv "${SCRIPT_DIR}/.venv"
    fi
    "${SCRIPT_DIR}/.venv/bin/pip" install --quiet -r "${SCRIPT_DIR}/requirements.txt"

    # Create service for MQTT Admin
    if [[ "$OS_TYPE" == "macos" ]]; then
        mkdir -p "$HOME/Library/LaunchAgents"
        cat > "${PLIST_ADMIN}" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.flp.mqtt-admin</string>
    <key>ProgramArguments</key>
    <array>
        <string>${SCRIPT_DIR}/.venv/bin/python3</string>
        <string>${SCRIPT_DIR}/mqtt_admin.py</string>
        <string>--broker</string>
        <string>localhost</string>
        <string>--port</string>
        <string>${MQTT_PORT}</string>
    </array>
    <key>WorkingDirectory</key>
    <string>${SCRIPT_DIR}</string>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>/tmp/flp-mqtt-admin.log</string>
    <key>StandardErrorPath</key>
    <string>/tmp/flp-mqtt-admin.err</string>
</dict>
</plist>
EOF
        launchctl unload "${PLIST_ADMIN}" 2>/dev/null || true
        launchctl load "${PLIST_ADMIN}"
    else
        cat > /etc/systemd/system/flp-mqtt-admin.service <<EOF
[Unit]
Description=FLP MQTT Admin — Cloud-side file transfer manager
After=network.target mosquitto.service mqtt-sn-gateway.service
Requires=mosquitto.service

[Service]
Type=simple
WorkingDirectory=${SCRIPT_DIR}
ExecStart=/usr/bin/python3 ${SCRIPT_DIR}/mqtt_admin.py --broker localhost --port ${MQTT_PORT}
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF
        systemctl daemon-reload
        systemctl enable flp-mqtt-admin
        systemctl restart flp-mqtt-admin
    fi
    info "FLP MQTT Admin installed and running."

    echo ""
    info "═══════════════════════════════════════════════════════"
    info " Deployment complete!"
    info ""
    info "  Mosquitto broker:     port ${MQTT_PORT} (TCP)"
    info "  MQTT-SN gateway:      port ${MQTTSN_PORT} (UDP) → ${MQTT_PORT} (TCP)"
    info "  FLP MQTT Admin:       connected to localhost:${MQTT_PORT}"
    info ""
    if [[ "$OS_TYPE" == "macos" ]]; then
        info " Manage services:"
        info "   brew services {start|stop|restart} mosquitto"
        info "   launchctl {load|unload} ${PLIST_MQTTSN}"
        info "   launchctl {load|unload} ${PLIST_ADMIN}"
        info ""
        info " View logs:"
        info "   tail -f /tmp/mqtt-sn-gateway.log"
        info "   tail -f /tmp/flp-mqtt-admin.log"
        info "   brew services list"
    else
        info " Manage services:"
        info "   sudo systemctl {start|stop|status} mosquitto"
        info "   sudo systemctl {start|stop|status} mqtt-sn-gateway"
        info "   sudo systemctl {start|stop|status} flp-mqtt-admin"
        info ""
        info " View logs:"
        info "   journalctl -u mosquitto -f"
        info "   journalctl -u mqtt-sn-gateway -f"
        info "   journalctl -u flp-mqtt-admin -f"
    fi
    info "═══════════════════════════════════════════════════════"
}

# ── Start / Stop / Status ───────────────────────────────────────────
cmd_start() {
    detect_distro
    require_root
    info "Starting all FLP services..."
    if [[ "$OS_TYPE" == "macos" ]]; then
        brew services restart mosquitto
        launchctl unload "${PLIST_MQTTSN}" 2>/dev/null
        launchctl load "${PLIST_MQTTSN}" 2>/dev/null || warn "mqtt-sn-gateway not installed, skipping"
        launchctl unload "${PLIST_ADMIN}" 2>/dev/null
        launchctl load "${PLIST_ADMIN}" 2>/dev/null || warn "flp-mqtt-admin not installed, skipping"
    else
        systemctl start mosquitto
        systemctl start mqtt-sn-gateway 2>/dev/null || warn "mqtt-sn-gateway not installed, skipping"
        systemctl start flp-mqtt-admin
    fi
    info "All services started."
}

cmd_stop() {
    detect_distro
    require_root
    info "Stopping all FLP services..."
    if [[ "$OS_TYPE" == "macos" ]]; then
        launchctl unload "${PLIST_ADMIN}" 2>/dev/null || true
        launchctl unload "${PLIST_MQTTSN}" 2>/dev/null || true
        brew services stop mosquitto 2>/dev/null || true
    else
        systemctl stop flp-mqtt-admin || true
        systemctl stop mqtt-sn-gateway || true
        systemctl stop mosquitto || true
    fi
    info "All services stopped."
}

cmd_status() {
    detect_distro
    if [[ "$OS_TYPE" == "macos" ]]; then
        echo "── Mosquitto ──"
        brew services list 2>/dev/null | grep -E "^mosquitto|^Name" || echo "  Not installed"
        echo ""
        echo "── MQTT-SN Gateway ──"
        if launchctl list com.flp.mqtt-sn-gateway &>/dev/null; then
            echo "  Loaded (launchd)"
            launchctl list com.flp.mqtt-sn-gateway 2>/dev/null
        else
            echo "  Not installed"
        fi
        echo ""
        echo "── FLP MQTT Admin ──"
        if launchctl list com.flp.mqtt-admin &>/dev/null; then
            echo "  Loaded (launchd)"
            launchctl list com.flp.mqtt-admin 2>/dev/null
        else
            echo "  Not installed"
        fi
    else
        echo "── Mosquitto ──"
        systemctl status mosquitto --no-pager -l 2>/dev/null || echo "  Not installed"
        echo ""
        echo "── MQTT-SN Gateway ──"
        systemctl status mqtt-sn-gateway --no-pager -l 2>/dev/null || echo "  Not installed"
        echo ""
        echo "── FLP MQTT Admin ──"
        systemctl status flp-mqtt-admin --no-pager -l 2>/dev/null || echo "  Not installed"
    fi
}

# ── Uninstall ───────────────────────────────────────────────────────
cmd_uninstall() {
    detect_distro
    require_root
    warn "This will stop and remove all FLP services."
    read -rp "Continue? [y/N] " confirm
    [[ "$confirm" =~ ^[Yy]$ ]] || exit 0

    cmd_stop

    if [[ "$OS_TYPE" == "macos" ]]; then
        rm -f "${PLIST_ADMIN}"
        rm -f "${PLIST_MQTTSN}"
        sudo rm -f /usr/local/bin/mqtt-sn-gateway
        sudo rm -f "${MQTTSN_GW_CONF}"
        rm -f "${MOSQUITTO_CONF}"
        sudo rm -rf "${MQTTSN_GW_DIR}"
        rm -rf "${SCRIPT_DIR}/.venv"
        info "FLP services removed. Mosquitto package left installed (remove with: brew uninstall mosquitto)."
    else
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
    fi
}

# ── Main ────────────────────────────────────────────────────────────
usage() {
    echo "Usage: bash deploy.sh <command>  (use sudo on Linux, not on macOS)"
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
