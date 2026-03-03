#!/usr/bin/env bash
# Wrapper to source ESP-IDF environment and run idf.py commands
# Usage: ./idf.sh build | ./idf.sh flash | ./idf.sh monitor | etc.
source "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1
exec idf.py "$@"
