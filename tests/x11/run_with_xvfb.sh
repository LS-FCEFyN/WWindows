#!/bin/sh
# Runs a test binary against a private, throwaway X server.
#
# The X11 adapter is almost entirely a conversation with an X server, so its
# tests need a real one. Provisioning it here (rather than expecting the
# developer to have DISPLAY set) keeps the suite self-contained and keeps it
# off whatever session the developer is actually using -- these tests take
# selection ownership and create override-redirect windows, which would be
# rude on a real desktop.
#
# Exits 77 (CTest SKIP_RETURN_CODE) when Xvfb is not installed, so the suite
# degrades to "skipped" rather than "failed" on a machine without it.

set -eu

BINARY="$1"

if ! command -v Xvfb >/dev/null 2>&1; then
    echo "Xvfb not found -- skipping X11 integration tests."
    echo "Install it with: apt install xvfb"
    exit 77
fi

# Pick a display number nobody is using. -displayfd would be tidier but is
# not available on every Xvfb build we care about.
DISPLAY_NUM=""
for n in $(seq 90 120); do
    if [ ! -e "/tmp/.X11-unix/X${n}" ] && [ ! -e "/tmp/.X${n}-lock" ]; then
        DISPLAY_NUM="$n"
        break
    fi
done

if [ -z "$DISPLAY_NUM" ]; then
    echo "No free X display number in 90-120 -- skipping."
    exit 77
fi

XVFB_LOG="$(mktemp)"
setsid Xvfb ":${DISPLAY_NUM}" -screen 0 1280x1024x24 -nolisten tcp > "$XVFB_LOG" 2>&1 &
XVFB_PID=$!

cleanup() {
    kill "$XVFB_PID" 2>/dev/null || true
    wait "$XVFB_PID" 2>/dev/null || true
    rm -f "$XVFB_LOG"
}
trap cleanup EXIT INT TERM

# Wait for the server to accept connections rather than sleeping a fixed
# amount: a fixed sleep is either slow or flaky, usually both.
ready=0
for _ in $(seq 1 50); do
    if [ -e "/tmp/.X11-unix/X${DISPLAY_NUM}" ]; then
        ready=1
        break
    fi
    if ! kill -0 "$XVFB_PID" 2>/dev/null; then
        echo "Xvfb exited during startup:"
        cat "$XVFB_LOG"
        exit 1
    fi
    sleep 0.1
done

if [ "$ready" -ne 1 ]; then
    echo "Xvfb did not become ready in time:"
    cat "$XVFB_LOG"
    exit 1
fi

DISPLAY=":${DISPLAY_NUM}"
export DISPLAY
"$BINARY"
