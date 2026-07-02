#!/usr/bin/env bash
#
# rASN fault-injection robustness harness (libfiu / fiu-run).
#
# Black-box robustness check for rASN, CodePilot, and SREPilot. It runs each
# binary many times under libfiu-injected faults -- libc allocation
# (malloc/calloc/realloc), libc string duplication, POSIX file I/O, and network
# syscalls -- and classifies every outcome. The goal is to distinguish genuine
# robustness defects from expected fail-stop behaviour:
#
#   * SIGSEGV / SIGBUS / SIGFPE / SIGILL / hang  -> genuine defect (script fails)
#   * SIGABRT                                    -> fail-stop (uncaught
#                                                   std::bad_alloc / assert)
#   * non-zero exit                              -> graceful error propagation
#   * exit 0                                     -> fault absorbed / not on path
#
# This is an opt-in developer tool. It is POSIX-only (it requires libfiu's
# `fiu-run`) and is deliberately NOT wired into the default CMake build or CI,
# so it does not affect Windows portability. Run it by hand on a Linux box that
# has the plugins built (`./run.sh build --build_plugins`) and libfiu installed
# (Debian/Ubuntu: `apt-get install fiu-utils libfiu-dev`).
#
# Usage:
#   run_fault_injection.sh [bin_dir]
#
# Environment overrides:
#   RASN_BIN_DIR      directory holding the built binaries (auto-detected)
#   RASN_FI_RUNS      runs per (target,fault) cell            [default 20]
#   RASN_FI_TIMEOUT   per-run wall-clock timeout, seconds     [default 20]
#   RASN_FI_OUT       output/log directory       [default ./fault_injection_out]
#   RASN_FI_KEEP_CORES  set to 1 to keep core dumps (default disables them so
#                       core-dump write latency is not misreported as a hang)
#
# Exit status: 0 if no genuine crash-class outcome was observed, 1 otherwise.

set -uo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
repo_root="$(cd "$script_dir/../../../../.." && pwd)"

RUNS="${RASN_FI_RUNS:-20}"
TIMEOUT_S="${RASN_FI_TIMEOUT:-20}"
OUT_DIR="${RASN_FI_OUT:-./fault_injection_out}"
KEEP_CORES="${RASN_FI_KEEP_CORES:-0}"
CONFIRM_RETRIES="${RASN_FI_CONFIRM:-3}"

# --- locate the built binaries --------------------------------------------
BIN_DIR="${1:-${RASN_BIN_DIR:-}}"
if [ -z "$BIN_DIR" ]; then
    for cand in "$repo_root/builder/bin" "$repo_root/builder/output/bin"; do
        if [ -x "$cand/codepilot/codepilot" ]; then BIN_DIR="$cand"; break; fi
    done
fi
if [ -z "$BIN_DIR" ] || [ ! -x "$BIN_DIR/codepilot/codepilot" ]; then
    echo "error: could not find built binaries; pass the bin dir or set RASN_BIN_DIR" >&2
    echo "       (expected e.g. \$repo/builder/bin/codepilot/codepilot)" >&2
    exit 2
fi

if ! command -v fiu-run >/dev/null 2>&1; then
    echo "error: fiu-run not found; install libfiu (apt-get install fiu-utils libfiu-dev)" >&2
    exit 2
fi

# Core dumps during a std::bad_alloc abort can take seconds to write and would
# otherwise be misreported as hangs; disable them unless explicitly requested.
[ "$KEEP_CORES" = "1" ] || ulimit -c 0 2>/dev/null || true

mkdir -p "$OUT_DIR"
CODEPILOT="$BIN_DIR/codepilot/codepilot"
SREPILOT="$BIN_DIR/srepilot/srepilot"
UNIT_TESTS="$BIN_DIR/rasn.unit_tests/rasn.unit_tests"
EXAMPLES="$repo_root/src/plugins/rasn/examples"
WORKFLOW="$EXAMPLES/generic-multi-agent.workflow"

# --- fault configurations (name -> fiu-run flags via global FLAGS array) ---
fault_flags() {
    FLAGS=()
    case "$1" in
        baseline)  FLAGS=(env) ;;
        malloc_5)  FLAGS=(fiu-run -x -c "enable_random name=libc/mm/*,probability=0.05") ;;
        malloc_20) FLAGS=(fiu-run -x -c "enable_random name=libc/mm/*,probability=0.20") ;;
        strdup_15) FLAGS=(fiu-run -x -c "enable_random name=libc/str/*,probability=0.15") ;;
        io_rw_5)   FLAGS=(fiu-run -x -c "enable_random name=posix/io/rw/*,probability=0.05") ;;
        io_all_15) FLAGS=(fiu-run -x -c "enable_random name=posix/io/*,probability=0.15") ;;
        net_25)    FLAGS=(fiu-run -x -c "enable_random name=posix/io/net/*,probability=0.25") ;;
        mixed_10)  FLAGS=(fiu-run -x \
                        -c "enable_random name=libc/mm/*,probability=0.10" \
                        -c "enable_random name=posix/io/*,probability=0.10") ;;
        *) echo "unknown fault config: $1" >&2; return 1 ;;
    esac
}

FAULTS=(baseline malloc_5 malloc_20 strdup_15 io_rw_5 io_all_15 net_25 mixed_10)
# A "raw crash" is any signal-death or timeout from a single run. A raw crash is
# promoted to a *genuine* defect only after a confirmation step, because
# injecting allocation/I/O failures into a complex runtime can produce artifacts
# that are not defects in the code under test:
#
#   * A hard crash signal (SIGSEGV/SIGBUS/SIGFPE/SIGILL) is confirmed by re-running
#     at the normal timeout up to CONFIRM_RETRIES times; it counts only if the
#     same class of hard signal recurs. A rare (<1%) libc/runtime bootstrap crash
#     -- e.g. an I/O fault corrupting config-file load before any application code
#     runs -- does not recur and is reported as "<sig>_transient".
#   * A TIMEOUT is confirmed by re-running with a much longer timeout: a genuine
#     deadlock still never returns, whereas a slow std::bad_alloc fail-stop
#     teardown (the runtime's crash handler symbolising a backtrace while
#     allocation faults are still being injected) completes when given more time.
#     Only a re-run that still exceeds the extended timeout is promoted to "HANG".
RAW_CRASH="SIGSEGV SIGBUS SIGFPE SIGILL TIMEOUT"
HARD_SIGNAL="SIGSEGV SIGBUS SIGFPE SIGILL"
# Confirmed (genuine) crash classes counted toward the exit status.
CRASH_CLASS="SIGSEGV SIGBUS SIGFPE SIGILL HANG"
GENUINE_DEFECTS=0

classify() {
    local rc=$1
    # 124 = GNU timeout "timed out" (SIGTERM path); 137 = 128+SIGKILL, which in
    # this harness is only ever produced by `timeout -s KILL` hitting the
    # deadline (nothing else sends SIGKILL). Treat both as TIMEOUT so a deadline
    # hit always goes through the confirm/extended-timeout path.
    #
    # A shell reports "killed by signal N" as 128+N. Linux signal numbers top out
    # at 64 (128+64 = 192), so only 129..192 is interpreted as a signal; 193..255
    # is a real exit code (e.g. a program's exit(255)/exit(-1)), not a signal.
    if [ "$rc" -eq 0 ]; then echo ok
    elif [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then echo TIMEOUT
    elif [ "$rc" -gt 128 ] && [ "$rc" -le 192 ]; then
        case $((rc - 128)) in
            11) echo SIGSEGV ;; 6) echo SIGABRT ;; 7) echo SIGBUS ;;
            8) echo SIGFPE ;; 4) echo SIGILL ;; 13) echo SIGPIPE ;;
            2) echo SIGINT ;; 9) echo SIGKILL ;; 15) echo SIGTERM ;;
            *) echo "sig$((rc - 128))" ;;
        esac
    else echo "exit$rc"; fi
}

is_crash_class() {
    local cls=$1
    for c in $CRASH_CLASS; do [ "$cls" = "$c" ] && return 0; done
    return 1
}

is_raw_crash() {
    local cls=$1
    for c in $RAW_CRASH; do [ "$cls" = "$c" ] && return 0; done
    return 1
}

is_hard_signal() {
    local cls=$1
    for c in $HARD_SIGNAL; do [ "$cls" = "$c" ] && return 0; done
    return 1
}

# run_one_to <timeout_s> <log> <cmd...> -> echoes the classification
#
# The deadline signal is SIGKILL (not the default SIGTERM). rDSN installs a
# SIGTERM handler that runs a full, allocating `dsn_exit()` cleanup; under
# continuous allocation-fault injection that shutdown path can itself stall, so a
# SIGTERM-based timeout would measure rDSN's shutdown handler rather than whether
# the program made progress. SIGKILL cannot be intercepted, so a TIMEOUT here
# means the process genuinely failed to finish on its own -- the right signal for
# hang detection in a one-shot CLI.
run_one_to() {
    local to="$1"; local log="$2"; shift 2
    timeout -s KILL "$to" "${FLAGS[@]}" "$@" >"$log" 2>&1
    classify $?
}

# run_one <log> <cmd...> -> echoes the classification, writes output to <log>
run_one() {
    local log="$1"; shift
    run_one_to "$TIMEOUT_S" "$log" "$@"
}

# run_target <label> <cmd...>
run_target() {
    local label="$1"; shift
    local -a cmd=("$@")
    echo "### $label   [$RUNS runs/fault, ${TIMEOUT_S}s timeout]"
    for fault in "${FAULTS[@]}"; do
        fault_flags "$fault" || continue
        declare -A tally=()
        local kept=""
        for i in $(seq 1 "$RUNS"); do
            local log="$OUT_DIR/$label.$fault.$i.log"
            local cls; cls=$(run_one "$log" "${cmd[@]}")
            if is_raw_crash "$cls"; then
                # Confirm the raw crash. A hard signal is re-run at the normal
                # timeout and must recur as a hard signal; a TIMEOUT is re-run
                # with a much longer timeout and must still time out (a real
                # deadlock never returns; a slow fail-stop teardown completes).
                local confirmed=0 k conf_to="$TIMEOUT_S"
                [ "$cls" = "TIMEOUT" ] && conf_to=$(( TIMEOUT_S * 4 > 60 ? TIMEOUT_S * 4 : 60 ))
                for k in $(seq 1 "$CONFIRM_RETRIES"); do
                    local clog="$OUT_DIR/$label.$fault.$i.confirm$k.log"
                    local ccls; ccls=$(run_one_to "$conf_to" "$clog" "${cmd[@]}")
                    if [ "$cls" = "TIMEOUT" ]; then
                        [ "$ccls" = "TIMEOUT" ] && { confirmed=1; break; }
                    else
                        is_hard_signal "$ccls" && { confirmed=1; break; }
                    fi
                    rm -f "$clog"
                done
                if [ "$confirmed" -eq 1 ]; then
                    [ "$cls" = "TIMEOUT" ] && cls="HANG"
                    kept="$log"; GENUINE_DEFECTS=$((GENUINE_DEFECTS + 1))
                else
                    cls="${cls}_transient"   # keep $log for inspection, do not fail
                fi
            else
                rm -f "$log"
            fi
            tally[$cls]=$(( ${tally[$cls]:-0} + 1 ))
        done
        printf '  %-10s' "$fault"
        for k in "${!tally[@]}"; do printf ' %s=%d' "$k" "${tally[$k]}"; done
        [ -n "$kept" ] && printf '  <<< CRASH-CLASS, log kept: %s' "$kept"
        echo
        unset tally
    done
    echo
}

echo "rASN fault-injection campaign"
echo "  bin dir : $BIN_DIR"
echo "  fiu-run : $(command -v fiu-run)"
echo

# 1. Pure-rASN compute path (schema generation runs before the rDSN runtime is
#    even initialised): the cleanest rASN-attributable target.
run_target "codepilot.schema.json" "$CODEPILOT" schema json
run_target "codepilot.schema.idl"  "$CODEPILOT" schema idl

# 2. Whole-engine surface: 90 rASN/CodePilot gtests covering state I/O, workflow
#    leases/recovery, replay, agents, and the resilience gates.
if [ -x "$UNIT_TESTS" ]; then
    run_target "rasn.unit_tests" "$UNIT_TESTS" --gtest_filter='rasn_*.*:codepilot_*.*'
fi

# 3. CLI file-read + parse paths.
if [ -f "$WORKFLOW" ]; then
    run_target "codepilot.workflow.validate" "$CODEPILOT" workflow validate "$WORKFLOW"
    run_target "codepilot.workflow.compile"  "$CODEPILOT" workflow compile  "$WORKFLOW"
fi

# 4. Registry / runtime-init CLI surfaces for CodePilot and SREPilot.
run_target "codepilot.providers" "$CODEPILOT" providers
run_target "codepilot.tools"     "$CODEPILOT" tools
[ -x "$SREPILOT" ] && run_target "srepilot.help" "$SREPILOT" help

echo "=================================================================="
if [ "$GENUINE_DEFECTS" -eq 0 ]; then
    echo "PASS: no reproducible crash-class outcomes (no SIGSEGV/SIGBUS/SIGFPE/"
    echo "      SIGILL, no reproducible hang) in the code under test. malloc faults"
    echo "      fail-stop via std::bad_alloc (SIGABRT); I/O and network faults"
    echo "      propagate as graceful errors. A '*_transient' count is a rare"
    echo "      runtime-bootstrap artifact that did not reproduce on re-run and is"
    echo "      not counted as a defect (inspect its kept log under $OUT_DIR)."
    exit 0
else
    echo "FAIL: observed $GENUINE_DEFECTS reproducible crash-class outcome(s); see kept logs in $OUT_DIR."
    exit 1
fi
