#!/bin/sh
# Runs the DRI3 suite against a GPU-backed X server.
#
# Unlike run_with_xvfb.sh this deliberately does NOT provision its own
# server: Xvfb implements neither DRI3 nor Present, so a throwaway server
# would make the entire suite untestable. The zero-copy path can only be
# exercised against a real one, which in practice means the developer's own
# session.
#
# That makes the preflight below the important part. The suite is skipped
# (exit 77, CTest's SKIP_RETURN_CODE) rather than failed whenever the
# environment cannot support it, so it degrades to "skipped" on CI and on
# headless machines while still running for real on a workstation. It is
# never silently reported as passed.
#
# These tests are safe to run on a live desktop: they create small
# override-redirect windows at the top-left corner, present to them, and
# destroy them. They take no selection ownership and do not touch any
# existing window.

set -eu

BINARY="$1"

if [ -z "${DISPLAY:-}" ]; then
    echo "DISPLAY is not set -- skipping DRI3 tests."
    echo "These need a real GPU-backed X server; run them from a desktop session."
    exit 77
fi

if [ ! -d /dev/dri ]; then
    echo "No /dev/dri -- skipping DRI3 tests (no DRM device available)."
    exit 77
fi

# Is there a node this user can actually open? Being in /dev/dri is not the
# same as being permitted to use it (the 'render'/'video' group question).
usable_node=""
for node in /dev/dri/renderD* /dev/dri/card*; do
    [ -e "$node" ] || continue
    if [ -r "$node" ] && [ -w "$node" ]; then
        usable_node="$node"
        break
    fi
done

if [ -z "$usable_node" ]; then
    echo "No readable/writable node in /dev/dri -- skipping DRI3 tests."
    echo "Usually means this user is not in the 'render' group."
    exit 77
fi

# Ask the server itself whether it speaks DRI3 and Present, rather than
# inferring it from the presence of a DRM node -- a remote or software X
# server can have neither while /dev/dri is perfectly healthy.
if command -v xdpyinfo >/dev/null 2>&1; then
    extensions="$(xdpyinfo 2>/dev/null || true)"
    for ext in DRI3 Present; do
        if ! printf '%s\n' "$extensions" | grep -q "  *${ext}$"; then
            echo "X server on $DISPLAY does not advertise $ext -- skipping."
            echo "(Xvfb and most remote/software servers do not implement it.)"
            exit 77
        fi
    done
else
    echo "xdpyinfo not found; relying on the suite's own DRI3 preflight."
fi

exec "$BINARY"
