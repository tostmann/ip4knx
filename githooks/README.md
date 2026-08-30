# Repository git hooks

These are the hooks this project relies on. Git does not install hooks from a
clone, so activate them once per working copy:

    git config core.hooksPath githooks

`git config --get core.hooksPath` should then print `githooks`. Setting it
makes git ignore `.git/hooks/` entirely, so every hook you want has to live
here.

| Hook | What it enforces |
|------|------------------|
| `pre-push` | GitHub receives consolidated release commits only, never the local `build snapshot v*` commits from the pre-build version bump. A release tag additionally needs a passing C3/C6 smoke-load artifact for the firmware version in `src/version.h` of the tagged commit (`scripts/loadtest_c3_c6.py` writes it). |
| `commit-msg` | Rejects `Co-Authored-By` trailers naming an AI assistant. |

Each hook documents its own override environment variable in its header. The
overrides exist for genuine exceptions — detached hardware, a deliberate
history rewrite — not as a way past a hook that is doing its job.
