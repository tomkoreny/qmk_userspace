#!/usr/bin/env bash
set -euo pipefail

script=$(readlink -f -- "${BASH_SOURCE[0]}")
script_dir=${script%/*}

if command -v python3 >/dev/null 2>&1; then
    exec python3 "$script_dir/oled-status.py" "$@"
elif command -v nix >/dev/null 2>&1; then
    exec nix shell nixpkgs#python3 -c python3 "$script_dir/oled-status.py" "$@"
else
    printf '%s\n' 'corne-oled: Python 3 is required; install python3 or provide nix for nix shell nixpkgs#python3.' >&2
    exit 1
fi
