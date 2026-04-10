#!/bin/bash
#
# Build Factory Binary Script for ip4knx
#
# Creates factory binaries for ESP WebFlashTools deployment.
# The factory binary contains: bootloader, partition table, and firmware.
#
# Usage:
#   ./build_factory.sh [tul_esp32c3|tul32_esp32c6]
#
# Output:
#   binaries/factory_tul_esp32c3.bin
#   binaries/factory_tul32_esp32c6.bin

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/tul-knx-gateway"
BINARIES_DIR="$PROJECT_DIR/binaries"

# Default target if not specified
TARGET="${1:-tul_esp32c3}"

echo "=== ip4knx Factory Binary Builder ==="
echo "Target: $TARGET"
echo "Output: $BINARIES_DIR/factory_${TARGET}.bin"
echo ""

# Create binaries directory
mkdir -p "$BINARIES_DIR"

# Change to build directory
cd "$BUILD_DIR"

# Update version info
bash update_version.sh

# Build the firmware
echo "[1/3] Building firmware with PlatformIO..."
$HOME/.platformio/penv/bin/pio run -e "$TARGET"

# Use esptool from PlatformIO venv directly
ESPTOOL_CMD="$HOME/.platformio/penv/bin/esptool"

# Detect chip family for --chip argument
if [[ "$TARGET" == *"esp32c3"* ]]; then
    CHIP="esp32c3"
elif [[ "$TARGET" == *"esp32c6"* ]]; then
    CHIP="esp32c6"
else
    echo "[Error] Unknown target: $TARGET"
    exit 1
fi

# Find partition addresses from partition table
echo "[2/3] Reading partition addresses..."

# ESP32-C3/C6 typically use:
# - Bootloader: 0x0000
# - Partition Table: 0x8000
# - Firmware: 0x10000
BOOTLOADER_ADDR="0x0000"
PARTITIONS_ADDR="0x8000"
FIRMWARE_ADDR="0x10000"

# Create factory binary
echo "[3/3] Creating factory binary..."
$ESPTOOL_CMD --chip $CHIP merge-bin \
    -o "$BINARIES_DIR/factory_${TARGET}.bin" \
    "${BOOTLOADER_ADDR}" "$BUILD_DIR/.pio/build/$TARGET/bootloader.bin" \
    "${PARTITIONS_ADDR}" "$BUILD_DIR/.pio/build/$TARGET/partitions.bin" \
    "${FIRMWARE_ADDR}" "$BUILD_DIR/.pio/build/$TARGET/firmware.bin"

# Verify output
if [ -f "$BINARIES_DIR/factory_${TARGET}.bin" ]; then
    SIZE=$(stat -c%s "$BINARIES_DIR/factory_${TARGET}.bin" 2>/dev/null || stat -f%z "$BINARIES_DIR/factory_${TARGET}.bin" 2>/dev/null)
    echo ""
    echo "=== Build Complete ==="
    echo "Factory binary: $BINARIES_DIR/factory_${TARGET}.bin"
    echo "Size: $SIZE bytes"
    echo ""
    echo "Flash via ESP WebFlashTools:"
    echo "  1. Open https://espressif.github.io/esp-webflasher/"
    echo "  2. Connect your TUL/TUL32 USB stick"
    echo "  3. Select the factory binary and flash"
    echo ""
else
    echo "[Error] Failed to create factory binary"
    exit 1
fi
