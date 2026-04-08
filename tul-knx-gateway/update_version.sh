#!/bin/bash
# Update version info before build
# Usage: ./update_version.sh [git-hash]

GIT_HASH=${1:-$(git rev-parse --short HEAD 2>/dev/null || echo "local")}
TIMESTAMP=$(date +%Y%m%d)

cat > src/version.h << EOF
#define BUILD_GIT "${GIT_HASH}"
#define BUILD_NUMBER ${TIMESTAMP}
EOF

echo "Version: ${TIMESTAMP} (${GIT_HASH})"
