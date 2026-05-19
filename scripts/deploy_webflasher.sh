#!/bin/bash
#
# Assemble the webflasher/ staging directory and rsync it to the live host.
#
# Inputs (versioned):
#   scripts/webflasher/index.html         — landing page
#   scripts/webflasher/manifest.json      — template with __VERSION__/__MD5_*__ placeholders
#   scripts/webflasher/busware.png        — logo
# Inputs (build artifacts, gitignored):
#   binaries/factory_tul_esp32c3.bin      — full image for WebFlasher
#   binaries/factory_tul32_esp32c6.bin
#   tul-knx-gateway/.pio/build/<env>/firmware.bin — raw app for over-HTTP OTA
# Output (gitignored):
#   webflasher/
# Default target:
#   10.10.22.1:/var/www/install/ip4knx/   →  https://install.busware.de/ip4knx/
#
# Usage:
#   ./scripts/deploy_webflasher.sh                # build + rsync to default host
#   ./scripts/deploy_webflasher.sh <user@host:/path/>
#   DRY_RUN=1     ./scripts/deploy_webflasher.sh  # preview rsync without sending
#   STAGE_ONLY=1  ./scripts/deploy_webflasher.sh  # build webflasher/ but don't rsync

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
TEMPLATES_DIR="$SCRIPT_DIR/webflasher"
BINARIES_DIR="$PROJECT_DIR/binaries"
BUILD_DIR="$PROJECT_DIR/tul-knx-gateway/.pio/build"
STAGE_DIR="$PROJECT_DIR/webflasher"

TARGET="${1:-10.10.22.1:/var/www/install/ip4knx/}"

# --- Sanity ----------------------------------------------------------------
for f in "$TEMPLATES_DIR/index.html" "$TEMPLATES_DIR/manifest.json" "$TEMPLATES_DIR/busware.png"; do
    [ -f "$f" ] || { echo "ERROR: missing template $f"; exit 1; }
done

missing=0
for bin in factory_tul_esp32c3.bin factory_tul32_esp32c6.bin; do
    [ -f "$BINARIES_DIR/$bin" ] || { echo "ERROR: missing $BINARIES_DIR/$bin"; missing=1; }
done
for env in tul_esp32c3 tul32_esp32c6; do
    [ -f "$BUILD_DIR/$env/firmware.bin" ] || { echo "ERROR: missing $BUILD_DIR/$env/firmware.bin"; missing=1; }
done
if [ $missing -ne 0 ]; then
    echo ""
    echo "Hint: ./scripts/build_factory.sh tul_esp32c3  and  tul32_esp32c6  first."
    exit 1
fi

# --- Version + MD5 --------------------------------------------------------
VER_MAJOR_MINOR="$(cat "$PROJECT_DIR/tul-knx-gateway/version.txt" | tr -d '[:space:]')"
VER_BUILD="$(cat "$PROJECT_DIR/tul-knx-gateway/build_number.txt" | tr -d '[:space:]')"
VERSION="${VER_MAJOR_MINOR}.${VER_BUILD}"

MD5_C3="$(md5sum "$BUILD_DIR/tul_esp32c3/firmware.bin"   | awk '{print $1}')"
MD5_C6="$(md5sum "$BUILD_DIR/tul32_esp32c6/firmware.bin" | awk '{print $1}')"

echo "Version: $VERSION"
echo "MD5 C3:  $MD5_C3"
echo "MD5 C6:  $MD5_C6"

# --- Assemble staging dir -------------------------------------------------
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

cp "$TEMPLATES_DIR/index.html"    "$STAGE_DIR/"
cp "$TEMPLATES_DIR/busware.png"   "$STAGE_DIR/"
cp "$BINARIES_DIR/factory_tul_esp32c3.bin"      "$STAGE_DIR/"
cp "$BINARIES_DIR/factory_tul32_esp32c6.bin"    "$STAGE_DIR/"
cp "$BUILD_DIR/tul_esp32c3/firmware.bin"        "$STAGE_DIR/firmware_tul_esp32c3.bin"
cp "$BUILD_DIR/tul32_esp32c6/firmware.bin"      "$STAGE_DIR/firmware_tul32_esp32c6.bin"

# Generate manifest.json from template
sed -e "s/__VERSION__/$VERSION/g" \
    -e "s/__MD5_C3__/$MD5_C3/g" \
    -e "s/__MD5_C6__/$MD5_C6/g" \
    "$TEMPLATES_DIR/manifest.json" > "$STAGE_DIR/manifest.json"

echo ""
echo "Staged into: $STAGE_DIR/"
ls -lh "$STAGE_DIR"

if [ -n "$STAGE_ONLY" ]; then
    echo ""
    echo "STAGE_ONLY=1 — skipping rsync."
    exit 0
fi

# --- Push -----------------------------------------------------------------
DRY=""
if [ -n "$DRY_RUN" ]; then
    DRY="--dry-run"
    echo ""
    echo "=== DRY RUN — no files will be transferred ==="
fi

echo ""
echo "Target: $TARGET"
echo ""
rsync -avr $DRY "$STAGE_DIR/" "$TARGET"

if [ -z "$DRY_RUN" ]; then
    echo ""
    echo "=== Deploy complete ==="
    echo "Live URL:  https://install.busware.de/ip4knx/"
    echo "Manifest:  https://install.busware.de/ip4knx/manifest.json"
fi
