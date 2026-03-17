#!/usr/bin/env bash
# Quick start: runs mosquitto + FLP admin (Go) via Docker Compose
# Dashboard available at http://localhost:5050
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

mkdir -p received_files

case "${1:-up}" in
  up)     docker compose up --build -d && echo "Dashboard: http://localhost:5050" ;;
  down)   docker compose down ;;
  logs)   docker compose logs -f admin ;;
  build)  go build -o flp-admin . && echo "Built ./flp-admin" ;;
  *)      echo "Usage: ./run.sh [up|down|logs|build]" ;;
esac
