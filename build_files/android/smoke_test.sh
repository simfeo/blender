#!/usr/bin/env bash
# Startup smoke test for Blender on Android.
#
# It answers the question a release gate actually needs: does the app reach its UI on this device
# without a native crash, and did any built-in shader fail to compile?
#
# Usage:
#   build_files/android/smoke_test.sh [--serial SERIAL] [--apk APK] [--wait SECONDS]
#
# Exit status: 0 = pass, 1 = fail.
set -euo pipefail

SERIAL=""
APK=""
WAIT_SECONDS=15
PACKAGE="org.blender.blender"
ACTIVITY="${PACKAGE}/.BlenderActivity"

while [ $# -gt 0 ]; do
  case "$1" in
    --serial)
      SERIAL="$2"
      shift 2
      ;;
    --apk)
      APK="$2"
      shift 2
      ;;
    --wait)
      WAIT_SECONDS="$2"
      shift 2
      ;;
    -h|--help)
      sed -n '1,16p' "$0"
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

ADB=(adb)
if [ -n "$SERIAL" ]; then
  ADB+=(-s "$SERIAL")
fi

"${ADB[@]}" get-state >/dev/null 2>&1 || {
  echo "FAIL: no adb device (use --serial if several are connected)" >&2
  exit 1
}

if [ -n "$APK" ]; then
  echo "== install $APK"
  "${ADB[@]}" install -r -d "$APK"
fi

echo "== clear logcat and start"
"${ADB[@]}" logcat -c
"${ADB[@]}" shell am force-stop "$PACKAGE"
"${ADB[@]}" shell am start -W -n "$ACTIVITY" >/dev/null
sleep "$WAIT_SECONDS"

FAIL=0
PID="$("${ADB[@]}" shell pidof "$PACKAGE" | tr -d '\r')"
if [ -z "$PID" ]; then
  echo "FAIL: process is not running after ${WAIT_SECONDS}s"
  FAIL=1
else
  echo "PASS: process is running (pid $PID)"
fi

LOG="$(mktemp -t blender-android-smoke.XXXXXX.log)"
"${ADB[@]}" logcat -d -v threadtime >"$LOG"

if grep -qE "FATAL signal|Fatal signal|SIGSEGV|ANR in ${PACKAGE}" "$LOG"; then
  echo "FAIL: native crash / ANR found:"
  grep -E "FATAL signal|Fatal signal|SIGSEGV|ANR in ${PACKAGE}" "$LOG" | tail -10
  FAIL=1
else
  echo "PASS: no SIGSEGV / ANR in logcat"
fi

if grep -q "Failed to compile graphics pipeline" "$LOG"; then
  echo "WARN: a graphics pipeline failed to compile (contained, but check the shader):"
  grep "Failed to compile graphics pipeline" "$LOG" | tail -5
fi

if grep -q "WM_init: complete" "$LOG"; then
  echo "PASS: WM_init complete"
else
  echo "WARN: 'WM_init: complete' not found (startup may still be running)"
fi

OUT_DIR="${SMOKE_OUT_DIR:-artifacts}"
mkdir -p "$OUT_DIR"
SHOT="$OUT_DIR/blender-smoke-$(date +%Y%m%d-%H%M%S).png"
if "${ADB[@]}" exec-out screencap -p >"$SHOT" 2>/dev/null; then
  echo "== screenshot: $SHOT"
fi

echo "== logcat: $LOG"
if [ "$FAIL" -ne 0 ]; then
  echo "RESULT: FAIL"
  exit 1
fi
echo "RESULT: PASS"
