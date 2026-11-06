#!/bin/bash
set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
echo "Building dog-editor-inputleap in $PROJECT_DIR..."

flatpak run --filesystem=/home/kali/Projects/inputleap --command=bash org.freedesktop.Sdk//26.08 -c "
    cd '$PROJECT_DIR'
    cmake -B build -DINPUTLEAP_BUILD_GUI=OFF -DINPUTLEAP_BUILD_TESTS=ON
    cmake --build build -j\$(nproc)
"

echo "Build complete! Executables available in $PROJECT_DIR/build/bin/"
