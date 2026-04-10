#!/bin/bash
# Update version info before build

GIT_HASH=${1:-$(git rev-parse --short HEAD 2>/dev/null || echo "local")}
TIMESTAMP=$(date +%Y%m%d)
VERSION="1.1.0"

cat > src/version.h << EOF
#pragma once
#define BUILD_GIT "${GIT_HASH}"
#define BUILD_NUMBER ${TIMESTAMP}
#define FIRMWARE_VERSION "${VERSION}"
EOF

echo "Version: ${VERSION}, Build: ${TIMESTAMP} (${GIT_HASH})"