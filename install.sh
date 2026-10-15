#!/usr/bin/env bash
# Build and install geany-ai-qt.
#   ./install.sh          -> install for the current user only (~/.config/geany/plugins), no sudo
#   ./install.sh --system -> install system-wide (needs sudo)
set -euo pipefail
cd "$(dirname "$0")"

missing=()
command -v cmake      >/dev/null || missing+=(cmake)
command -v g++        >/dev/null || missing+=(build-essential)
command -v pkg-config >/dev/null || missing+=(pkg-config)
pkg-config --exists geany      2>/dev/null || missing+=(geany geany-common)
pkg-config --exists gtk+-3.0   2>/dev/null || missing+=(libgtk-3-dev)
pkg-config --exists Qt6Widgets 2>/dev/null || missing+=(qt6-base-dev)
if (( ${#missing[@]} )); then
    echo "Missing build dependencies. Install them with:"
    echo "  sudo apt install ${missing[*]}"
    exit 1
fi

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

if [[ "${1:-}" == "--system" ]]; then
    sudo cmake --install build
    echo "Installed system-wide. Restart Geany, then enable it in Tools -> Plugin Manager."
else
    dest="${XDG_CONFIG_HOME:-$HOME/.config}/geany/plugins"
    mkdir -p "$dest"
    install -m 755 build/geany-ai-qt.so "$dest/"
    echo "Installed to $dest/geany-ai-qt.so"
    echo "Restart Geany, then enable it in Tools -> Plugin Manager."
fi
