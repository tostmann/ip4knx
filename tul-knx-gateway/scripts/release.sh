#!/usr/bin/env bash
# Tag-only release helper for ip4knx — see global CLAUDE.md "Tag-only-Push"
# convention.  Creates an annotated tag on the current HEAD and pushes ONLY
# that tag to origin.  origin/main is intentionally NOT advanced — local main
# accumulates auto-snapshot commits from version_bump.py that should never
# leak to GitHub.
#
# Usage:  scripts/release.sh <version>
#   e.g.  scripts/release.sh 1.3.42
#
# Run from the project root or anywhere — it cd's to the git toplevel.

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <version>   (e.g. 1.3.42)" >&2
    exit 64
fi

VERSION="$1"
TAG="v${VERSION}"

if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "ERROR: version must be MAJOR.MINOR.BUILD (got: $VERSION)" >&2
    exit 64
fi

cd "$(git rev-parse --show-toplevel)"

# Refuse to release with a dirty working tree — the snapshot hook would
# create a new auto-snapshot commit on the next build anyway, so any uncommitted
# work belongs in a proper commit before the tag is set.
if [[ -n "$(git status --porcelain)" ]]; then
    echo "ERROR: working tree dirty — commit or stash before releasing" >&2
    git status -s
    exit 1
fi

if git rev-parse --verify --quiet "refs/tags/${TAG}" >/dev/null; then
    echo "ERROR: tag ${TAG} already exists" >&2
    exit 1
fi

# Verify the firmware version baked into the build matches the requested
# release version.  This catches the common mistake of forgetting to run a
# fresh `pio run` after editing version.txt.
HEADER="tul-knx-gateway/src/version.h"
if [[ -f "$HEADER" ]]; then
    BUILT_VERSION=$(awk -F'"' '/FW_VERSION_STRING/ {print $2}' "$HEADER")
    if [[ "$BUILT_VERSION" != "$VERSION" ]]; then
        echo "ERROR: $HEADER says version $BUILT_VERSION, but release is for $VERSION" >&2
        echo "       run 'pio run -e <env>' to regenerate version.h, then retry" >&2
        exit 1
    fi
fi

HEAD_SHA=$(git rev-parse --short=7 HEAD)
echo "Creating annotated tag $TAG on $HEAD_SHA …"
git tag -a "$TAG" -m "Release $VERSION"

echo "Pushing $TAG to origin (origin/main intentionally NOT advanced) …"
git push origin "$TAG"

echo
echo "Done.  Released $TAG → $HEAD_SHA on GitHub."
echo "Local main has $(git rev-list --count origin/main..HEAD) commits ahead of origin/main"
echo "(snapshots and infra commits — these stay local by design)."
