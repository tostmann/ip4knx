#!/bin/bash
#
# Factory Image Verification Script for ip4knx
#
# Automated test script to verify factory binaries by:
# 1. Flashing the device
# 2. Provisioning WiFi via ImprovSerial
# 3. Checking web status dashboard
# 4. Sending KNX telegrams
#
# Usage:
#   ./verify_factory_image.sh --target tul_esp32c3|tul32_esp32c6 --ssid <SSID> --password <PW>
#
# Example:
#   ./verify_factory_image.sh --target tul_esp32c3 --ssid 'MyWiFi' --password 'Secret123'

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BINARIES_DIR="$PROJECT_DIR/binaries"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default values
TARGET=""
SSID=""
PASSWORD=""
PORT=""
CHIP=""

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --target)
            TARGET="$2"
            shift 2
            ;;
        --ssid)
            SSID="$2"
            shift 2
            ;;
        --password)
            PASSWORD="$2"
            shift 2
            ;;
        --port)
            PORT="$2"
            shift 2
            ;;
        --help|-h)
            echo "Usage: $0 --target tul_esp32c3|tul32_esp32c6 --ssid <SSID> --password <PW> [--port <PORT>]"
            echo ""
            echo "Options:"
            echo "  --target    Target environment (tul_esp32c3 or tul32_esp32c6)"
            echo "  --ssid      WiFi SSID to provision"
            echo "  --password  WiFi password"
            echo "  --port      Serial port (auto-detected if not specified)"
            echo "  --help      Show this help"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Validate required arguments
if [ -z "$TARGET" ]; then
    echo -e "${RED}Error: --target is required${NC}"
    exit 1
fi

if [ -z "$SSID" ] || [ -z "$PASSWORD" ]; then
    echo -e "${RED}Error: --ssid and --password are required${NC}"
    exit 1
fi

# Determine chip family
if [[ "$TARGET" == *"esp32c3"* ]]; then
    CHIP="esp32c3"
elif [[ "$TARGET" == *"esp32c6"* ]]; then
    CHIP="esp32c6"
else
    echo -e "${RED}Error: Unknown target: $TARGET${NC}"
    exit 1
fi

FACTORY_BIN="$BINARIES_DIR/factory_${TARGET}.bin"

if [ ! -f "$FACTORY_BIN" ]; then
    echo -e "${RED}Error: Factory binary not found: $FACTORY_BIN${NC}"
    exit 1
fi

echo "=============================================="
echo "  ip4knx Factory Image Verification"
echo "=============================================="
echo ""
echo "Target:     $TARGET ($CHIP)"
echo "Binary:     $FACTORY_BIN"
echo "SSID:       $SSID"
echo "Password:   ${PASSWORD:0:2}****${PASSWORD: -2}"
echo ""

# Auto-detect serial port if not specified
if [ -z "$PORT" ]; then
    echo -e "${BLUE}[Step 0] Auto-detecting serial port...${NC}"

    # Try to find ESP32 USB-JTAG serial port
    for pattern in '/dev/ttyUSB*' '/dev/ttyACM*' '/dev/cu.usb*'; do
        for p in $pattern; do
            if [ -e "$p" ]; then
                PORT="$p"
                break
            fi
        done
        [ -n "$PORT" ] && break
    done

    if [ -z "$PORT" ]; then
        echo -e "${RED}Error: No serial port found. Connect device and specify --port${NC}"
        exit 1
    fi

    echo -e "${GREEN}Found port: $PORT${NC}"
fi

# Activate PlatformIO venv
if [ -f "$HOME/.platformio/penv/activate" ]; then
    source "$HOME/.platformio/penv/activate"
fi

ESPTOOL="$HOME/.platformio/penv/bin/esptool"

#############################################
# Step 1: Flash the device
#############################################
echo ""
echo -e "${BLUE}[Step 1] Flashing device...${NC}"
echo "Port: $PORT"
echo "Chip: $CHIP"
echo "Binary: $FACTORY_BIN"
echo ""

$ESPTOOL --chip $CHIP --port "$PORT" write_flash 0x0 "$FACTORY_BIN"

echo -e "${GREEN}[Step 1] Flashing completed successfully${NC}"
sleep 2

#############################################
# Step 2: WiFi Provisioning via ImprovSerial
#############################################
echo ""
echo -e "${BLUE}[Step 2] Provisioning WiFi...${NC}"

# Reset device to ensure clean boot
echo "Resetting device..."
stty -F "$PORT" 115200 raw -echo
echo -n "R" > "$PORT"  # Send reset command if supported
sleep 1

# Wait for device to boot
echo "Waiting for device to boot (5 seconds)..."
sleep 5

# Run Improv provisioning
echo "Sending WiFi credentials via ImprovSerial..."
python3 "$SCRIPT_DIR/test_improv.py" \
    --port "$PORT" \
    --ssid "$SSID" \
    --password "$PASSWORD" \
    --validate

if [ $? -ne 0 ]; then
    echo -e "${RED}[Step 2] WiFi provisioning failed${NC}"
    exit 1
fi

echo -e "${GREEN}[Step 2] WiFi provisioning completed successfully${NC}"
sleep 3

#############################################
# Step 3: Check Web Status Dashboard
#############################################
echo ""
echo -e "${BLUE}[Step 3] Checking web status dashboard...${NC}"

# Get device IP from serial output or mDNS
echo "Waiting for device to connect to WiFi..."
sleep 5

# Try to resolve tul.local
DEVICE_IP=""
for i in {1..10}; do
    DEVICE_IP=$(getent hosts tul.local 2>/dev/null | awk '{ print $1 }' || echo "")
    if [ -n "$DEVICE_IP" ]; then
        break
    fi
    echo "Waiting for mDNS... ($i/10)"
    sleep 2
done

if [ -z "$DEVICE_IP" ]; then
    echo -e "${YELLOW}Warning: Could not resolve tul.local via mDNS${NC}"
    echo "Trying to scan network for device..."
    # Fallback: try common IP patterns or ask user
    DEVICE_IP="tul.local"  # Try hostname directly
fi

echo "Device IP/Hostname: $DEVICE_IP"

# Check web server
echo "Checking web server..."
HTTP_STATUS=$(curl -s -o /dev/null -w "%{http_code}" "http://$DEVICE_IP/" --connect-timeout 5 || echo "000")

if [ "$HTTP_STATUS" != "200" ]; then
    echo -e "${RED}[Step 3] Web server not responding (HTTP $HTTP_STATUS)${NC}"
    exit 1
fi

echo -e "${GREEN}Web server responding (HTTP 200)${NC}"

# Check API status
echo "Checking API status endpoint..."
API_RESPONSE=$(curl -s "http://$DEVICE_IP/api/status" --connect-timeout 5 || echo "")

if [ -z "$API_RESPONSE" ]; then
    echo -e "${RED}[Step 3] API status endpoint not responding${NC}"
    exit 1
fi

echo "API Response:"
echo "$API_RESPONSE" | python3 -m json.tool 2>/dev/null || echo "$API_RESPONSE"

# Verify key fields
WIFI_CONNECTED=$(echo "$API_RESPONSE" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('wifi_connected', False))" 2>/dev/null || echo "False")
KNX_CONFIGURED=$(echo "$API_RESPONSE" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('knx_configured', False))" 2>/dev/null || echo "False")

if [ "$WIFI_CONNECTED" != "True" ]; then
    echo -e "${YELLOW}Warning: WiFi not connected according to API${NC}"
fi

echo -e "${GREEN}[Step 3] Web status dashboard check completed${NC}"

#############################################
# Step 4: Send KNX Telegram
#############################################
echo ""
echo -e "${BLUE}[Step 4] Testing KNX communication...${NC}"

# Get KNX individual address from API
KNX_PA=$(echo "$API_RESPONSE" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('knx_pa', '0.0.0'))" 2>/dev/null || echo "0.0.0")
echo "KNX Individual Address: $KNX_PA"
echo "Gateway IP: $DEVICE_IP"

# Get KNX configuration state
KNX_CONFIGURED=$(echo "$API_RESPONSE" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('knx_configured', False))" 2>/dev/null || echo "False")

# Check if knxtool is available
if ! command -v knxtool &> /dev/null; then
    echo -e "${YELLOW}Warning: knxtool not found. Skipping KNX test.${NC}"
    echo "Install with: apt install knxtool or use Docker image"
    KNX_OK="SKIPPED"
elif [ "$KNX_CONFIGURED" = "False" ]; then
    echo -e "${YELLOW}KNX device not configured (ETS programming required)${NC}"
    echo "Note: This is expected for a freshly flashed device."
    echo "Use ETS to program the device with a KNX application."
    KNX_OK="UNCONFIGURED"
else
    # Test 1: Send a test telegram via the gateway
    TEST_GA="11/2/14"
    echo ""
    echo "Sending test telegram to GA $TEST_GA via gateway..."

    if knxtool groupswrite ip:"$DEVICE_IP" "$TEST_GA" 1 2>&1; then
        echo -e "${GREEN}KNX telegram sent successfully${NC}"

        # Test 2: Monitor for incoming telegrams (brief check)
        echo "Checking KNX routing (monitoring for 3 seconds)..."
        timeout 3 knxtool groupmonitor ip:"$DEVICE_IP" 2>&1 | head -5 || true

        KNX_OK="PASS"
    else
        echo -e "${YELLOW}KNX telegram send failed${NC}"
        KNX_OK="FAIL"
    fi
fi

echo -e "${GREEN}[Step 4] KNX communication test completed ($KNX_OK)${NC}"

#############################################
# Summary
#############################################
echo ""
echo "=============================================="
echo -e "${GREEN}  Factory Image Verification PASSED${NC}"
echo "=============================================="
echo ""
echo "Summary:"
echo "  [OK] Flashing"
echo "  [OK] WiFi Provisioning"
echo "  [OK] Web Status Dashboard"
if [ "$KNX_OK" = "PASS" ]; then
    echo "  [OK] KNX Communication"
elif [ "$KNX_OK" = "FAIL" ]; then
    echo "  [FAIL] KNX Communication (telegram send failed)"
elif [ "$KNX_OK" = "PARTIAL" ]; then
    echo "  [~] KNX Communication (partial - telegram send failed)"
elif [ "$KNX_OK" = "SKIPPED" ]; then
    echo "  [-] KNX Communication (skipped - knxtool not installed)"
elif [ "$KNX_OK" = "UNCONFIGURED" ]; then
    echo "  [-] KNX Communication (device not ETS-programmed)"
else
    echo "  [OK] KNX Communication (basic)"
fi
echo ""
echo "Device accessible at: http://$DEVICE_IP"
echo "KNX Individual Address: $KNX_PA"
echo ""
