#!/usr/bin/env bash
# Run the bpy functional suite inside the installed Android build and collect its evidence.
#
# Usage:
#   build_files/android/tests/run_functional_tests.sh [--serial SERIAL] [--apk APK] [--timeout S]
#
# The app must be the build from this tree (debug.blender.testargs / testlog support). The script
# writes artifacts/functional-run-<timestamp>/ with functional.json, functional.log, logcat,
# startup log, session log and a screenshot.
set -euo pipefail

SERIAL=""
APK=""
TIMEOUT=600
ONLY=""
PACKAGE="org.blender.blender"
ACTIVITY="${PACKAGE}/.BlenderActivity"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$REPO_ROOT/artifacts/functional-run-$STAMP"

while [ $# -gt 0 ]; do
  case "$1" in
    --serial) SERIAL="$2"; shift 2 ;;
    --apk) APK="$2"; shift 2 ;;
    --timeout) TIMEOUT="$2"; shift 2 ;;
    --only) ONLY="$2"; shift 2 ;;
    -h|--help) sed -n '1,12p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done

ADB=(adb)
if [ -n "$SERIAL" ]; then ADB+=(-s "$SERIAL"); fi
"${ADB[@]}" get-state >/dev/null 2>&1 || { echo "FAIL: no adb device" >&2; exit 1; }

if [ -n "$APK" ]; then
  echo "== install $APK"
  "${ADB[@]}" install -r "$APK"
fi

mkdir -p "$OUT"
DEVICE_DIR="/sdcard/Download/Blender/tests/functional"
ARGS_FILE="/sdcard/Download/blender_args.txt"

echo "== push test suite"
"${ADB[@]}" shell mkdir -p "$DEVICE_DIR"
"${ADB[@]}" push "$SCRIPT_DIR/functional_test.py" /sdcard/Download/Blender/tests/functional_test.py >/dev/null
"${ADB[@]}" shell "printf '%s\n' '--factory-startup' '--python' '/sdcard/Download/Blender/tests/functional_test.py' > $ARGS_FILE"
"${ADB[@]}" shell rm -f /sdcard/Download/Blender/tests/only.txt
if [ -n "$ONLY" ]; then
  echo "$ONLY" | tr ',' '\n' > /tmp/blender-only.txt
  "${ADB[@]}" push /tmp/blender-only.txt /sdcard/Download/Blender/tests/only.txt >/dev/null
fi

echo "== clear old evidence"
"${ADB[@]}" shell rm -rf "$DEVICE_DIR"
"${ADB[@]}" shell mkdir -p "$DEVICE_DIR"
"${ADB[@]}" shell rm -f /sdcard/Download/Blender/blender-test.log
"${ADB[@]}" logcat -c

echo "== start suite (timeout ${TIMEOUT}s)"
"${ADB[@]}" shell setprop debug.blender.testlog 1
"${ADB[@]}" shell setprop debug.blender.testargs 1
"${ADB[@]}" shell am force-stop "$PACKAGE"
"${ADB[@]}" shell am start -W -n "$ACTIVITY" >/dev/null

RESULT=""
for _ in $(seq 1 "$TIMEOUT"); do
  if "${ADB[@]}" shell cat "$DEVICE_DIR/functional.json" 2>/dev/null | grep -q '"done": true'; then
    RESULT="done"
    break
  fi
  PID="$("${ADB[@]}" shell pidof "$PACKAGE" | tr -d '\r')"
  if [ -z "$PID" ]; then
    echo "WARN: process died while the suite was running"
    break
  fi
  sleep 1
done

if [ "$RESULT" != "done" ]; then
  echo "WARN: suite did not report done within ${TIMEOUT}s"
fi

echo "== collect evidence"
mkdir -p "$OUT/functional"
"${ADB[@]}" pull "$DEVICE_DIR/." "$OUT/functional/" >/dev/null 2>&1 || true
"${ADB[@]}" pull /sdcard/Download/Blender/blender-test.log "$OUT/blender-test.log" >/dev/null 2>&1 || true
"${ADB[@]}" pull /sdcard/Download/blender-startup.log "$OUT/blender-startup.log" >/dev/null 2>&1 || true
"${ADB[@]}" logcat -d -v threadtime > "$OUT/logcat.txt" 2>/dev/null || true
"${ADB[@]}" exec-out screencap -p > "$OUT/screen.png" 2>/dev/null || true

echo "== summary"
STATUS=0
python3 - "$OUT/functional/functional.json" <<'PY' || STATUS=$?
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
if not path.exists():
    print("FAIL: no functional.json")
    raise SystemExit(2)
data = json.loads(path.read_text())
counts = {}
for test in data.get("tests", []):
    counts[test["status"]] = counts.get(test["status"], 0) + 1
print("blender:", data.get("blender"))
print("done:", data.get("done"))
print("counts:", counts)
print()
print("%-5s %-6s %-36s %s" % ("id", "status", "name", "detail"))
print("-" * 110)
for test in data.get("tests", []):
    detail = str(test.get("detail", "")).replace("\n", " ")[:60]
    print("%-5s %-6s %-36s %s" % (test.get("id", "?"), test.get("status", "?"), test.get("name", "")[:36], detail))
failed = counts.get("FAIL", 0)
raise SystemExit(1 if failed else 0)
PY
echo "== evidence: $OUT"
exit "$STATUS"
