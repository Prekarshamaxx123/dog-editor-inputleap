#!/bin/bash
set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
echo "Building dog-editor-inputleap with full GUI in $PROJECT_DIR..."

flatpak run --user --filesystem=/home/kali/Projects/inputleap --filesystem=/tmp --command=bash org.kde.Sdk//6.10 -c "
    cd '$PROJECT_DIR'
    export PKG_CONFIG_PATH=/tmp/avahi-dev/usr/lib/x86_64-linux-gnu/pkgconfig:\$PKG_CONFIG_PATH
    cmake -B build -DINPUTLEAP_BUILD_GUI=ON -DINPUTLEAP_BUILD_TESTS=ON
    cmake --build build -j\$(nproc)
"

# Copy libdns_sd runtime library to build/bin so input-leap GUI is self-contained
if [ -f /tmp/avahi-dev/usr/lib/x86_64-linux-gnu/libdns_sd.so.1 ]; then
    cp -d /tmp/avahi-dev/usr/lib/x86_64-linux-gnu/libdns_sd.so* "$PROJECT_DIR/build/bin/" 2>/dev/null || true
fi

echo "Build complete! Executables available in $PROJECT_DIR/build/bin/:"
ls -lh "$PROJECT_DIR/build/bin/"
