#!/bin/sh
# Build the macOS peer of mwb (protocol-compatible with src/mwb.c).
# Run on a Mac with Xcode command-line tools installed:  ./build_mac.sh
# Attachments: CoreFoundation + CoreGraphics (event taps, display bounds),
#              Carbon (Pasteboard C API, kVK_* keycodes), IOKit (ev_keymap.h).
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
clang -O2 -Wall -Wextra -o "$DIR/mwb-mac" "$DIR/src/mwb_mac.c" \
    -framework CoreFoundation \
    -framework CoreGraphics \
    -framework Carbon \
    -framework IOKit
echo "built $DIR/mwb-mac"
echo
echo "first run: mwb-mac will ask for the Accessibility permission"
echo "(System Settings > Privacy & Security > Accessibility > add mwb-mac)"
