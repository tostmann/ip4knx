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

# At release time the tree always carries one kind of change: the generated
# version artifacts.  The pre-build hook rewrites src/version.h on every build
# (fresh BUILD_GIT and timestamp), and the factory build that produces the
# shipped images runs after the release commit — so demanding a spotless tree
# made this helper unusable exactly when it was wanted.
#
# Everything else uncommitted is still refused: the tag has to point at a
# commit that contains the sources the firmware was built from.
AUTO_FILES='^(tul-knx-gateway/build_number\.txt|tul-knx-gateway/src/version\.h)$'
user_dirty=$(git status --porcelain | cut -c4- | grep -Ev "$AUTO_FILES" || true)
if [[ -n "$user_dirty" ]]; then
    echo "ERROR: uncommitted changes outside the generated version files:" >&2
    echo "$user_dirty" | sed 's/^/  /' >&2
    echo "       commit or stash them — a tag must match what was built" >&2
    exit 1
fi

if git rev-parse --verify --quiet "refs/tags/${TAG}" >/dev/null; then
    echo "ERROR: tag ${TAG} already exists" >&2
    exit 1
fi

# The firmware version baked into the build.  It is NOT expected to equal the
# tag: the tag is a curated release counter (v1.4.15 shipped FW 1.4.121), while
# the firmware carries the monotone build number.  What has to line up is the
# smoke artifact and the firmware actually being shipped, so that is what the
# gate below is asked about.
HEADER="tul-knx-gateway/src/version.h"
FW_VERSION=""
if [[ -f "$HEADER" ]]; then
    FW_VERSION=$(awk -F'"' '/FW_VERSION_STRING/ {print $2; exit}' "$HEADER")
fi
if [[ -z "$FW_VERSION" ]]; then
    echo "ERROR: no FW_VERSION_STRING in $HEADER" >&2
    echo "       run 'pio run -e <env>' to regenerate version.h, then retry" >&2
    exit 1
fi
echo "Tag $TAG ships firmware $FW_VERSION"

# C3-vs-C6 smoke-load gate (release condition, decision 2026-06-09).  Every
# release must be preceded by a passing concurrent-tunnel smoke test on BOTH
# chip families running this exact version.  scripts/loadtest_c3_c6.py writes
# the artifact this check consumes.  Deliberate exception (hardware detached,
# emergency hotfix):  ALLOW_UNTESTED_RELEASE=1 scripts/release.sh <version>
if [[ "${ALLOW_UNTESTED_RELEASE:-0}" == "1" ]]; then
    echo "WARNING: ALLOW_UNTESTED_RELEASE=1 — skipping C3/C6 smoke-load gate"
else
    python3 scripts/check_loadtest_gate.py --version "$FW_VERSION" || exit 1
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
