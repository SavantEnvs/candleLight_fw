#!/usr/bin/env bash
#
# mayhem/test.sh — known-answer oracle for the gs_usb firmware core. mayhem/build.sh builds
# /mayhem/kat_probe (the firmware's USB class driver and CAN glue with normal flags, on host stand-ins for
# the USB and CAN peripherals); it replays a fixed conversation and prints what the device sent to the host.
# Every answer below is compared with the value the gs_usb protocol and the board's configuration dictate.
#
# IMPORTANT:
#  * Must assert BEHAVIOR/OUTPUT, not just exit status. The oracle has to check asserted values /
#    golden-output diffs / known-answer results — so a PATCH that "fixes" a bug by making the program
#    exit(0) (or any no-op) FAILS here. Running inputs and checking only "exit 0 / didn't crash" is
#    NOT a functional test (it's trivially reward-hackable) — use the project's real assertion suite.
#  * Do NOT build here — mayhem/build.sh already compiled the test suite (with the project's normal
#    flags). This script only RUNS the pre-built tests and reports counts. If the test runner is
#    missing, that's a build.sh bug — fail loudly rather than silently rebuilding.
#  * REQUIRED OUTPUT — a CTRF (https://ctrf.io) summary so Mayhem/the PATCH grader reads the counts:
#      - writes a CTRF JSON report to ${CTRF_REPORT:-$SRC/ctrf-report.json}, and
#      - prints a one-line `CTRF {...}` marker to stdout (same JSON, compact).
#    Only `results.summary` (with tests/passed/failed/pending/skipped/other) is required.
#    Use the emit_ctrf helper below; it computes tests = passed+failed+skipped and sets the exit
#    code (0 iff failed==0). Map your framework's output to passed/failed/skipped.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc (use -j"$MAYHEM_JOBS")
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
# Writes a CTRF report (file + stdout `CTRF {...}` marker) and returns non-zero iff failed>0.
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

PROBE=/mayhem/kat_probe
if [ ! -x "$PROBE" ]; then
  echo "missing $PROBE (mayhem/build.sh should have built it)" >&2
  emit_ctrf "candlelight-gs-usb-kat" 0 1 0
  exit $?
fi

out="$("$PROBE" 2>&1)"
rc=$?
printf '%s\n' "$out"

passed=0
failed=0
[ "$rc" -eq 0 ] && passed=$((passed + 1)) || { failed=$((failed + 1)); echo "FAIL: probe exited $rc" >&2; }

# check <name> <expected line>
check() {
  if printf '%s\n' "$out" | grep -qxF -- "$2"; then
    passed=$((passed + 1))
  else
    failed=$((failed + 1))
    echo "FAIL: $1: expected '$2'" >&2
  fi
}

# USB device descriptor: bLength 18, DEVICE, USB 2.00, class 0, EP0 64 bytes, VID 1d50, PID 606f, 3 strings, 1 config
check device_descriptor "device_descriptor: 12 01 00 02 00 00 00 40 50 1d 6f 60 00 00 01 02 03 01"
# GS_USB_BREQ_DEVICE_CONFIG: one channel (icount 0), sw_version 2, hw_version 1
check device_config "device_config: 00 00 00 00 02 00 00 00 01 00 00 00"
# GS_USB_BREQ_BT_CONST (bxcan board): feature 0x000530bb, fclk 48 MHz, tseg1 1..16, tseg2 1..8, sjw 4, brp 1..1024, brp_inc 1
check bt_const "bt_const: bb 30 05 00 00 6c dc 02 01 00 00 00 10 00 00 00 01 00 00 00 08 00 00 00 04 00 00 00 01 00 00 00 00 04 00 00 01 00 00 00"
# GS_USB_BREQ_MODE start with valid bit timing enables the channel
check mode_start "can_enabled: 1"
# frame sent by the host (echo_id 7, id 0x123, 4 bytes) comes back as its own echo
check echo_frame "echo_frame: 07 00 00 00 23 01 00 00 04 00 00 00 de ad be ef 00 00 00 00"
# frame received from the bus (extended id 0x321) is forwarded with echo_id 0xffffffff
check rx_frame "rx_frame: ff ff ff ff 21 03 00 80 03 00 00 00 11 22 33 00 00 00 00 00"

emit_ctrf "candlelight-gs-usb-kat" "$passed" "$failed" 0
