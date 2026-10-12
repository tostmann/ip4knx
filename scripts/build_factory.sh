#!/bin/bash
#
# Build Factory Binary Script for ip4knx
#
# Creates factory binaries for ESP WebFlashTools deployment.
# The factory binary contains: bootloader, partition table, and firmware.
#
# Usage:
#   ./build_factory.sh [tul_esp32c3|tul32_esp32c6|tulx32_esp32c6]
#
# Output:
#   binaries/factory_tul_esp32c3.bin
#   binaries/factory_tul32_esp32c6.bin
#   binaries/factory_tulx32_esp32c6.bin
#
# The TULX32 image is NOT the same shape as the TUL/TUL32 one. It ships the
# recovery system, so it carries the recovery bootloader (with the S1 check)
# instead of the framework bootloader, an otadata that selects ota_0, and the
# recovery app in the factory partition at 0x2B0000. Those binaries are built
# outside this repository and are expected in tul-knx-gateway/tulx32_recovery/.

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

# Assemble the image list
echo "[2/3] Collecting flash images..."

# ESP32-C3/C6 typically use:
# - Bootloader: 0x0000
# - Partition Table: 0x8000
# - Firmware: 0x10000
BOOTLOADER_ADDR="0x0000"
PARTITIONS_ADDR="0x8000"
FIRMWARE_ADDR="0x10000"

PIO_OUT="$BUILD_DIR/.pio/build/$TARGET"

if [ "$TARGET" = "tulx32_esp32c6" ]; then
    # Shop TULX32: the delivered bootloader is the one with the S1 recovery
    # check, not the framework bootloader that PlatformIO just built. Both are
    # frozen at delivery (no USB in the field), so getting this wrong is not
    # repairable — refuse rather than merge a plausible-looking wrong image.
    REC_DIR="$BUILD_DIR/tulx32_recovery"
    BOOT_APP0="$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
    OTADATA_ADDR="0xE000"
    RECOVERY_ADDR="0x2B0000"

    for f in "$REC_DIR/bootloader.bin" "$REC_DIR/tulx32_recovery.bin" "$BOOT_APP0"; do
        [ -f "$f" ] || { echo "[Error] missing $f"; exit 1; }
    done

    # Existence is not enough: tulx32_recovery/ is untracked, so a stale or
    # foreign recovery build would be frozen into the delivered image without
    # anyone noticing. Bind it to the hashes the repository expects.
    PIN_FILE="$BUILD_DIR/tulx32_recovery.sha256"
    [ -f "$PIN_FILE" ] || { echo "[Error] missing $PIN_FILE"; exit 1; }
    if ! ( cd "$REC_DIR" && sha256sum -c --quiet "$PIN_FILE" ); then
        echo "[Error] recovery binaries do not match $PIN_FILE"
        echo "        Bootloader and recovery cannot be changed after delivery."
        echo "        Update the pin file only together with a new recovery release."
        exit 1
    fi
    echo "      recovery binaries match $PIN_FILE"

    IMAGES=(
        "${BOOTLOADER_ADDR}" "$REC_DIR/bootloader.bin"
        "${PARTITIONS_ADDR}" "$PIO_OUT/partitions.bin"
        "${OTADATA_ADDR}"    "$BOOT_APP0"
        "${FIRMWARE_ADDR}"   "$PIO_OUT/firmware.bin"
        "${RECOVERY_ADDR}"   "$REC_DIR/tulx32_recovery.bin"
    )
    echo "      recovery bootloader: $REC_DIR/bootloader.bin"
    echo "      recovery app @ $RECOVERY_ADDR, otadata @ $OTADATA_ADDR -> ota_0"
else
    IMAGES=(
        "${BOOTLOADER_ADDR}" "$PIO_OUT/bootloader.bin"
        "${PARTITIONS_ADDR}" "$PIO_OUT/partitions.bin"
        "${FIRMWARE_ADDR}"   "$PIO_OUT/firmware.bin"
    )
fi

# Create factory binary
echo "[3/3] Creating factory binary..."
$ESPTOOL_CMD --chip $CHIP merge-bin \
    -o "$BINARIES_DIR/factory_${TARGET}.bin" \
    "${IMAGES[@]}"

# esp-web-tools 10.4.0 aborts at 99 % when the compressed image ends in a
# 1-byte block (see webflash_last_block.py) — pad such an image with 0xFF.
# Web-flasher images only; the TULX32 image goes over the debug adapter.
if [ "$TARGET" != "tulx32_esp32c6" ]; then
    python3 "$SCRIPT_DIR/webflash_last_block.py" --fix "$BINARIES_DIR/factory_${TARGET}.bin"
fi

# Verify output
if [ -f "$BINARIES_DIR/factory_${TARGET}.bin" ]; then
    SIZE=$(stat -c%s "$BINARIES_DIR/factory_${TARGET}.bin" 2>/dev/null || stat -f%z "$BINARIES_DIR/factory_${TARGET}.bin" 2>/dev/null)
    echo ""
    echo "=== Build Complete ==="
    echo "Factory binary: $BINARIES_DIR/factory_${TARGET}.bin"
    echo "Size: $SIZE bytes"
    echo ""
    if [ "$TARGET" = "tulx32_esp32c6" ]; then
        echo "TULX32 image — flash on the test bench over the debug adapter:"
        echo "  esptool --chip esp32c6 -p <port> write-flash 0x0 \\"
        echo "      $BINARIES_DIR/factory_${TARGET}.bin"
        echo ""
        echo "Contains the recovery system. Do NOT flash this onto a TUL32."
        echo ""
    else
        echo "Flash via ESP WebFlashTools:"
        echo "  1. Open https://espressif.github.io/esp-webflasher/"
        echo "  2. Connect your TUL/TUL32 USB stick"
        echo "  3. Select the factory binary and flash"
        echo ""
    fi
else
    echo "[Error] Failed to create factory binary"
    exit 1
fi
