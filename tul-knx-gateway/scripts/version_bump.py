"""
PlatformIO pre-build hook implementing the MAJOR.MINOR.BUILD versioning +
per-build git-snapshot pattern from the global CLAUDE.md.

Order on each build:
  1. If the working tree is dirty, commit it as "build snapshot v<PREV>"
     (the rollback target representing the state about to be built).
  2. Increment build_number.txt by 1.
  3. Regenerate src/version.h with current MAJOR.MINOR.BUILD + git hash + timestamp.

version.txt holds MAJOR.MINOR (manual). build_number.txt holds the monotone
build counter (auto-incremented; never reset on MAJOR/MINOR change).
"""
Import("env")  # type: ignore
import datetime
import os
import pathlib
import subprocess
import time

from SCons.Script import COMMAND_LINE_TARGETS  # type: ignore

# Skip on pure upload / monitor / clean / size targets — those re-flash or
# inspect the artifact without producing a new binary, so bumping the build
# counter would falsely advance the version. Bumping happens only on real
# compile iterations (`pio run` without --target, or with `--target build`).
_SKIP_TARGETS = {"upload", "uploadfs", "uploadfsota", "nobuild",
                 "monitor", "clean", "size", "erase", "checkprogsize"}
if any(t in _SKIP_TARGETS for t in COMMAND_LINE_TARGETS):
    print(f"[version_bump] target {COMMAND_LINE_TARGETS} — skipping bump")
    Return()  # type: ignore   (PlatformIO/SCons early-out from a pre-script)

PROJECT_DIR = pathlib.Path(env["PROJECT_DIR"])
VERSION_TXT = PROJECT_DIR / "version.txt"
BUILD_TXT   = PROJECT_DIR / "build_number.txt"
HEADER      = PROJECT_DIR / "src" / "version.h"


def git(*args, cwd):
    return subprocess.run(
        ["git", *args], cwd=cwd, capture_output=True, text=True
    )


def _atomic_write_text(path, text):
    """NFS-safe write: tmp + fsync(file) + atomic replace + fsync(dir).
    pathlib's write_text() flushes only to the NFS client cache; a reader
    (next build step, git, mklittlefs) can then see NUL-prefixed blocks.
    Incident class: build_number.txt once got overwritten with pure NULs."""
    path = pathlib.Path(path)
    tmp = path.parent / (path.name + ".tmp")
    with open(tmp, "w") as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    dfd = os.open(str(path.parent) or ".", os.O_RDONLY)
    try:
        os.fsync(dfd)
    finally:
        os.close(dfd)


git_root_p = git("rev-parse", "--show-toplevel", cwd=PROJECT_DIR)
git_root = git_root_p.stdout.strip() if git_root_p.returncode == 0 else None

ver = VERSION_TXT.read_text().strip()
try:
    major, minor = (int(x) for x in ver.split("."))
except ValueError:
    raise SystemExit(f"[version_bump] version.txt malformed: {ver!r} (expected MAJOR.MINOR)")

try:
    build = int(BUILD_TXT.read_text().strip())
except (FileNotFoundError, ValueError):
    build = 0

prev_string = f"{major}.{minor}.{build}"

# Detect "second env in a multi-env pio run" so build_number doesn't bump
# twice (which would land C3 and C6 on different versions in the same deploy).
# The signature is: the only dirty files are our own auto-generated artifacts
# from the bump that just happened for the first env.
_OWN_FILES = ("build_number.txt", "src/version.h")

if git_root:
    status = git("status", "--porcelain", cwd=git_root)
    dirty = [l for l in status.stdout.splitlines() if l.strip()]
    user_dirty = [l for l in dirty if not any(p in l for p in _OWN_FILES)]
    if user_dirty:
        git("add", "-A", cwd=git_root)
        # NFS-Flaky-Workaround: a commit-msg hook makes git re-read
        # .git/COMMIT_EDITMSG from the NFS-backed repo after the hook; the NFS
        # client occasionally serves a stale/NUL-padded read -> "a NUL byte in
        # commit log message not allowed" / "failed to write commit object".
        # Message is clean ASCII and no object is written (retry can't dupe) ->
        # transient, so retry before giving up.
        committed = False
        last_err = ""
        for attempt in range(8):
            commit = git("commit", "-m", f"build snapshot v{prev_string}", cwd=git_root)
            if commit.returncode == 0:
                committed = True
                break
            last_err = ((commit.stderr or "") + (commit.stdout or "")).strip()
            if not ("NUL byte" in last_err or "failed to write commit object" in last_err):
                break
            time.sleep(0.25 * (attempt + 1))
        if committed:
            print(f"[version_bump] snapshot: build snapshot v{prev_string}")
        else:
            print(f"[version_bump] snapshot commit failed: {last_err}")
        build += 1
        _atomic_write_text(BUILD_TXT, f"{build}\n")
    elif dirty:
        # Only our own bump artifacts are dirty — same deploy cycle, share version
        print(f"[version_bump] only auto files dirty, reusing v{prev_string}")
    else:
        # Truly clean working tree — bump anyway so consecutive builds advance
        print("[version_bump] working tree clean, bumping counter")
        build += 1
        _atomic_write_text(BUILD_TXT, f"{build}\n")
else:
    print("[version_bump] no git repo, bumping counter only")
    build += 1
    _atomic_write_text(BUILD_TXT, f"{build}\n")

if git_root:
    h = git("rev-parse", "--short=7", "HEAD", cwd=git_root)
    git_hash = h.stdout.strip() if h.returncode == 0 else "unknown"
else:
    git_hash = "no-git"

ts = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
new_string = f"{major}.{minor}.{build}"

_atomic_write_text(HEADER, f"""#pragma once
// Auto-generated by scripts/version_bump.py — do not edit by hand.
// Regenerated on every PlatformIO build.

#define FW_VERSION_MAJOR  {major}
#define FW_VERSION_MINOR  {minor}
#define FW_VERSION_BUILD  {build}
#define FW_VERSION_STRING "{new_string}"
#define FW_BUILD_DATE     "{ts}"

// Backwards-compatibility aliases for existing consumers in main.cpp.
#define FIRMWARE_VERSION  FW_VERSION_STRING
#define BUILD_NUMBER      FW_VERSION_BUILD
#define BUILD_GIT         "{git_hash}"
""")

print(f"[version_bump] v{new_string}  git={git_hash}  date={ts}")
