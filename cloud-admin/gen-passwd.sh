#!/usr/bin/env bash
# Generate Mosquitto password file from .env credentials.
# Usage: ./gen-passwd.sh
set -euo pipefail
cd "$(dirname "$0")"

if [ ! -f .env ]; then
  echo "Error: .env not found. Copy .env.example and fill in values." >&2
  exit 1
fi

# shellcheck disable=SC1091
source .env

: "${MQTT_NODE_PASS:?Set MQTT_NODE_PASS in .env}"
: "${MQTT_ADMIN_PASS:?Set MQTT_ADMIN_PASS in .env}"

PASSWD_FILE="$(pwd)/passwd"
rm -f "$PASSWD_FILE"
touch "$PASSWD_FILE"
chmod 0700 "$PASSWD_FILE"

docker run --rm -v "$PASSWD_FILE:/tmp/passwd" eclipse-mosquitto:2 \
  mosquitto_passwd -b /tmp/passwd flp-node "$MQTT_NODE_PASS"

docker run --rm -v "$PASSWD_FILE:/tmp/passwd" eclipse-mosquitto:2 \
  mosquitto_passwd -b /tmp/passwd flp-admin "$MQTT_ADMIN_PASS"

echo "Generated passwd with users: flp-node, flp-admin"
