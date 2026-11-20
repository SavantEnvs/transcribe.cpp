#!/usr/bin/env bash
#
# mayhem/test.sh — RUN this repo's OWN functional test suite (already built by mayhem/build.sh).
# exit 0 = pass. EDIT per repo. PATCH-grade oracle: after an agent patches the source, the grader
# rebuilds (build.sh) then runs this. DELETE this file if the repo has no meaningful tests.
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

# Upstream's CTest suite (unit tests + toy-GGUF smoke tests + CLI smokes),
# built by mayhem/build.sh in build-tests/. A test counts as passed only when
# CTest reports it passed AND its output carries the test's own success line
# (most print "...: ok" / "OK" / "PASS" / "0 failure(s)"; the rest a diagnostic in EXPECT) —
# a handful of tests that are silent by design are listed in QUIET. Tests
# CTest skips (they need a real model via an env var) count as skipped.
[ -f build-tests/CTestTestfile.cmake ] || { echo "build-tests/ missing: run mayhem/build.sh" >&2; exit 1; }
JUNIT="$(mktemp "${TMPDIR:-/tmp}/transcribe-ctest-XXXXXX.xml")"
(cd build-tests && ctest -j"$MAYHEM_JOBS" --timeout 300 --test-output-size-passed 1048576 --test-output-size-failed 1048576 --output-junit "$JUNIT") || true
read -r P F S < <(python3 -I - "$JUNIT" <<'PY'
import re, sys, xml.etree.ElementTree as ET
QUIET = {"transcribe_extension_umbrella_check", "transcribe_backend_classification_unit",
         "transcribe_backend_probe_order_unit", "transcribe_moss_diarize_parser_unit",
         "transcribe_granite_diarize_parser_unit", "transcribe_cli_output_smoke",
         "transcribe_cli_device_arg_smoke", "transcribe_cli_langid_smoke"}
# tests whose last line is a diagnostic they assert on, not a success line
EXPECT = {"transcribe_backend_init_unit": "test-device-foreign:",
          "transcribe_conv_pw_promote_unit": "promoted 2 conv pointwise weights",
          "transcribe_run_dispatch_unit": "is not accepted in a batch",
          "transcribe_stream_dispatch_unit": "is not accepted for streaming",
          "transcribe_log_unit": "message dropped",
          "transcribe_lint_teardown": "no raw ggml teardown calls"}
OK = re.compile(r"(?i)(\bok\b|\bpass|\b0 failure|^model: )", re.M)
p = f = s = 0
try:
    root = ET.parse(sys.argv[1]).getroot()
except Exception as e:
    print(0, 1, 0); sys.exit()
for tc in root.iter("testcase"):
    name, status = tc.get("name"), tc.get("status")
    out = tc.find("system-out"); text = (out.text or "") if out is not None else ""
    if status == "notrun": s += 1; continue
    good = status == "run" and tc.find("failure") is None and (name in QUIET or (EXPECT[name] in text if name in EXPECT else OK.search(text)))
    if good: p += 1
    else:
        f += 1; print("FAILED:", name, status, repr(text[-200:]), file=sys.stderr)
print(p, f, s)
PY
)
rm -f "$JUNIT"
[ "${P:-0}" -gt 0 ] || F=$(( ${F:-0} + 1 ))
emit_ctrf "cmake-ctest" "${P:-0}" "${F:-1}" "${S:-0}"
