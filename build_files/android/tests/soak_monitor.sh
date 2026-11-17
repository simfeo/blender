#!/usr/bin/env bash
# 40+ minute industrial-workflow soak test with simulated external input.
#
# Usage:
#   build_files/android/tests/soak_monitor.sh [--duration SECONDS] [--serial SERIAL] [--apk APK]
#
# It pushes soak_workflow.py, starts it through --factory-startup, then injects keyboard, mouse,
# stylus and touchpad events with `adb shell input <source>` while sampling process/memory/crash
# state and taking screenshots. At the end it pulls the JSON/log/screenshots into
# artifacts/soak-<timestamp>/.
set -uo pipefail

DURATION=2700
SERIAL=""
APK=""
PACKAGE="org.blender.blender"
ACTIVITY="${PACKAGE}/.BlenderActivity"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$REPO_ROOT/artifacts/soak-$STAMP"
DEVICE_DIR="/sdcard/Download/Blender/soak"
ARGS_FILE="/sdcard/Download/blender_args.txt"

while [ $# -gt 0 ]; do
  case "$1" in
    --duration) DURATION="$2"; shift 2 ;;
    --serial) SERIAL="$2"; shift 2 ;;
    --apk) APK="$2"; shift 2 ;;
    -h|--help) sed -n '1,14p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done

ADB=(adb)
[ -n "$SERIAL" ] && ADB+=(-s "$SERIAL")
"${ADB[@]}" get-state >/dev/null 2>&1 || { echo "FAIL: no adb device" >&2; exit 1; }
mkdir -p "$OUT"

if [ -n "$APK" ]; then
  "${ADB[@]}" install -r "$APK"
fi

echo "== prepare (${DURATION}s)"
"${ADB[@]}" shell svc power stayon true
"${ADB[@]}" shell settings put system screen_off_timeout 2147483647
"${ADB[@]}" shell mkdir -p /sdcard/Download/Blender/tests
"${ADB[@]}" push "$SCRIPT_DIR/soak_workflow.py" /sdcard/Download/Blender/tests/soak_workflow.py >/dev/null
echo "$DURATION" | "${ADB[@]}" shell "cat > /sdcard/Download/Blender/tests/soak_duration.txt"
"${ADB[@]}" shell "printf '%s\n' '--factory-startup' '--python' '/sdcard/Download/Blender/tests/soak_workflow.py' > $ARGS_FILE"
"${ADB[@]}" shell rm -rf "$DEVICE_DIR"
"${ADB[@]}" shell mkdir -p "$DEVICE_DIR"
"${ADB[@]}" shell setprop debug.blender.testlog 1
"${ADB[@]}" shell setprop debug.blender.testargs 1
"${ADB[@]}" logcat -c
"${ADB[@]}" shell am force-stop "$PACKAGE"
"${ADB[@]}" shell am start -W -n "$ACTIVITY" >/dev/null
sleep 6
# Dismiss a first-run popup or an on-screen keyboard left from a previous session.
"${ADB[@]}" shell input keyevent 4 >/dev/null 2>&1 || true

START=$(date +%s)
DEADLINE=$((START + DURATION + 120))
LOOP=0
CRASH=0
: > "$OUT/memory.csv"
echo "timestamp,pid,pss_kb" > "$OUT/memory.csv"

# One external-device event per minute, rotating through the hardware sources adb can inject.
inject_event() {
  case $((LOOP % 7)) in
    0) "${ADB[@]}" shell input keyboard keyevent 32 >/dev/null 2>&1 ;;          # play/pause
    1) "${ADB[@]}" shell input keyboard keycombination 113 29 >/dev/null 2>&1 ;; # Ctrl+Z
    2) "${ADB[@]}" shell input mouse tap 900 800 >/dev/null 2>&1 ;;
    3) "${ADB[@]}" shell input stylus swipe 900 800 1050 700 300 >/dev/null 2>&1 ;;
    4) "${ADB[@]}" shell input touchpad swipe 900 800 1050 700 300 >/dev/null 2>&1 ;;
    5) "${ADB[@]}" shell input mouse swipe 900 800 1050 700 300 >/dev/null 2>&1 ;;
    6) "${ADB[@]}" shell input stylus tap 900 800 >/dev/null 2>&1 ;;
  esac
}

while [ "$(date +%s)" -lt "$DEADLINE" ]; do
  LOOP=$((LOOP + 1))
  PID="$("${ADB[@]}" shell pidof "$PACKAGE" | tr -d '\r')"
  if [ -z "$PID" ]; then
    echo "CRASH: process is gone at loop $LOOP" | tee -a "$OUT/monitor.log"
    CRASH=1
    break
  fi

  if [ $((LOOP % 4)) -eq 0 ]; then
    PSS="$("${ADB[@]}" shell dumpsys meminfo "$PACKAGE" 2>/dev/null | grep -m1 'TOTAL PSS' | tr -s ' ' | sed 's/^ //' || true)"
    echo "$(date +%H:%M:%S),$PID,$PSS" >> "$OUT/memory.csv"
    inject_event || true
    echo "tick $LOOP pid=$PID $PSS" >> "$OUT/monitor.log"
  fi

  if [ $((LOOP % 20)) -eq 0 ]; then
    "${ADB[@]}" exec-out screencap -p > "$OUT/visual-$(printf '%02d' $((LOOP / 20))).png" 2>/dev/null || true
    FATAL="$("${ADB[@]}" logcat -d -v brief 2>/dev/null | grep -E 'FATAL|SIGSEGV|ANR in org.blender' | tail -1 || true)"
    if [ -n "$FATAL" ]; then echo "FATAL: $FATAL" >> "$OUT/monitor.log"; fi
  fi

  if "${ADB[@]}" shell cat "$DEVICE_DIR/soak.json" 2>/dev/null | grep -q '"done": true'; then
    echo "DONE: soak reported done at loop $LOOP" | tee -a "$OUT/monitor.log"
    break
  fi
  sleep 15
done

ELAPSED=$(( $(date +%s) - START ))
echo "== collect after ${ELAPSED}s"
"${ADB[@]}" pull "$DEVICE_DIR" "$OUT/soak" >/dev/null 2>&1 || true
"${ADB[@]}" pull /sdcard/Download/Blender/blender-test.log "$OUT/blender-test.log" >/dev/null 2>&1 || true
"${ADB[@]}" pull /sdcard/Download/blender-startup.log "$OUT/blender-startup.log" >/dev/null 2>&1 || true
"${ADB[@]}" logcat -d -v threadtime > "$OUT/logcat.txt" 2>/dev/null || true
"${ADB[@]}" exec-out screencap -p > "$OUT/final.png" 2>/dev/null || true

"${ADB[@]}" shell rm -f "$ARGS_FILE"
"${ADB[@]}" shell setprop debug.blender.testargs 0
"${ADB[@]}" shell svc power stayon false

cat > "$OUT/summary.md" <<EOF
# Soak run $STAMP

- requested duration: ${DURATION}s
- observed duration: ${ELAPSED}s
- crash: ${CRASH}
- loop: ${LOOP}
- device: $("${ADB[@]}" shell getprop ro.product.model | tr -d '\r')
- android: $("${ADB[@]}" shell getprop ro.build.version.release | tr -d '\r')
EOF
if [ -f "$OUT/soak/soak.json" ]; then
  python3 - "$OUT/soak/soak.json" >> "$OUT/summary.md" <<'PY'
import json, sys
data = json.load(open(sys.argv[1]))
print("- done: %s" % data.get("done"))
print("- cycles: %s" % data.get("cycle"))
print("- ops: %s" % data.get("ops"))
print("- renders: %s" % data.get("renders"))
print("- saves: %s" % data.get("saves"))
print("- maxrss_kb: %s" % data.get("maxrss_kb"))
print("- errors: %s" % len(data.get("errors", [])))
for error in data.get("errors", [])[-10:]:
    print("  - %s" % error)
PY
fi

echo "== evidence: $OUT"
# The soak timer and this loop sample the clock independently; a few seconds short of the
# requested duration is still a full run. A real shortfall is more than 30 s.
if [ "$CRASH" -eq 1 ] || [ "$ELAPSED" -lt $((DURATION - 30)) ]; then
  exit 1
fi
exit 0
