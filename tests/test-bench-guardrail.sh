#!/usr/bin/env bash

# Hot-syscall performance guardrail
#
# Runs bench-hot-guard (musl-static) and, when the cross-glibc toolchain is
# available, bench-hot-guard-glibc (dynamic glibc) under elfuse, then enforces
# limits on four hot paths:
#
#   getpid                <= 200 ns/op    (shim identity fast path)
#   clock_gettime(libc)   <=  50 ns/op    (vDSO CNTVCT fast path)
#   read(/dev/urandom, 1) <= 400 ns/op    (shim urandom ring fast path)
#   stat("/dev/null")     <= Nx getpid    (guest_read_path, no fast path)
#
# The first three are absolute; stat-path is a ratio. See the threshold block
# below for why the two kinds of limit are not interchangeable.
#
# The static (musl) bench is the baseline; the dynamic-glibc bench verifies that
# glibc 2.41's vDSO probe (NT_GNU_ABI_TAG PT_NOTE) keeps clock_gettime on the
# trampolines instead of trapping. When LINUX_TOOLCHAIN is missing the glibc
# variant skips cleanly.

set -u

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ELFUSE="${ELFUSE:-${REPO_ROOT}/build/elfuse}"
BENCH_GUARDRAIL_DIR="${BENCH_GUARDRAIL_DIR:-${REPO_ROOT}/build}"
BENCH_GUARDRAIL_REQUIRE_STATIC="${BENCH_GUARDRAIL_REQUIRE_STATIC:-1}"
STATIC_BENCH="${BENCH_GUARDRAIL_DIR}/bench-hot-guard"
GLIBC_BENCH="${BENCH_GUARDRAIL_DIR}/bench-hot-guard-glibc"
GLIBC_TOOLCHAIN="${LINUX_TOOLCHAIN:-/opt/toolchain/aarch64-linux-gnu}"
GLIBC_SYSROOT="${GLIBC_TOOLCHAIN}/aarch64-unknown-linux-gnu/sysroot"
ITERS="${BENCH_GUARDRAIL_ITERS:-200000}"

# Absolute ceilings for the three shim/vDSO-served lanes. The wide headroom is
# deliberate and matches what these lanes actually check: each is served inline
# and regresses as a step function, not a slope. Losing the shim identity path
# takes getpid from ~50 ns to SVC time, and losing vDSO acceptance takes
# clock_gettime from ~5 ns to ~2000 ns. Both blow past any of these numbers, so
# tightening them buys nothing and only risks flaking on slower CI hardware.
# read-urandom1 keeps the widest margin of the three: on a loaded laptop it was
# observed up to ~280 ns/op across sequential runs, while an SVC bail lands past
# 1000 ns, so 400 separates them without chasing load noise.
THRESH_GETPID=200
THRESH_CLOCK_GETTIME=50
THRESH_URANDOM=400

# stat-path is checked as a ratio to getpid from the same run, not as an
# absolute figure. It is the only lane whose cost is a slope: it scales with
# what the guest-copy helpers charge per call, so it regresses by a percentage
# rather than by falling off a fast path. An absolute ceiling cannot catch that
# without being tuned to one machine. Dividing by getpid, measured on the same
# hardware moments earlier, cancels the machine out.
#
# The number this exists to catch: arming the SIGBUS recovery pad with sigsetjmp
# instead of _setjmp added two sigprocmask traps per guest copy and took stat
# from 4.2 us to 6.1 us, a ratio of 84 to 122. It tripped no ceiling here,
# because this lane did not exist.
#
# Observed 84 static and 407 dynamic, both within 3% across runs. The dynamic
# arm is 5x the static one because sysroot redirection resolves every path
# component. Ceilings sit ~1.25x over observed.
#
# The static arm is the detector. Restoring the sigsetjmp regression moves it 82
# -> 125, comfortably past 105, while the dynamic arm moved only 407 -> 488 and
# slipped under its ceiling on one run: sysroot resolution is so much of that
# 13.6 us that a per-copy cost is diluted to noise. Keep the dynamic arm for
# gross regressions, do not tighten it toward 450 chasing the small ones, and do
# not read a dynamic pass as evidence the copy helpers are clean.
THRESH_STAT_RATIO_STATIC=105
THRESH_STAT_RATIO_GLIBC=500

C_RED='\033[0;31m'
C_GREEN='\033[0;32m'
C_YELLOW='\033[0;33m'
C_RESET='\033[0m'

if [ ! -x "$ELFUSE" ]; then
    echo "elfuse binary missing at $ELFUSE" >&2
    exit 1
fi

run_static=1
if [ ! -x "$STATIC_BENCH" ]; then
    if [ "$BENCH_GUARDRAIL_REQUIRE_STATIC" = 1 ]; then
        echo "bench-hot-guard missing at $STATIC_BENCH" >&2
        exit 1
    fi
    /usr/bin/printf "  ${C_YELLOW}SKIP${C_RESET}  static        bench-hot-guard absent: %s\n" \
        "$STATIC_BENCH"
    run_static=0
fi

failures=0
unmeasured=0
benchmarks_run=0

# Set by any failure a second run would reproduce, so run_and_check skips the
# retry. Reset per pass by run_and_check.
deterministic=0

# extract_ns <bench-output> <label> Prints the floating-point ns/op for the line
# whose first column is exactly <label>.
#
# Returns nothing if the line is absent.
extract_ns()
{
    awk -v label="$2" '$1 == label { print $2 }' "$1"
}

# A throughput ceiling is a claim about this machine when it is free to answer.
# On a loaded host the number measures the load, not the code: every lane that
# does host work inflates together while the shim-served ones barely move, so
# the getpid-relative ratios stop cancelling and the absolute ceilings blow out.
# Observed here at 2 to 3 times the idle figures with a prover run in the
# background, on code that was not the cause. Consulted only after a retry has
# also failed, so a real regression on an idle machine still fails twice and is
# reported.
. "$(dirname "${BASH_SOURCE[0]}")/lib/bash-compat.sh"

host_is_busy()
{
    test_host_is_busy
}

# check_threshold <variant> <label> <ns/op> <ceiling-ns>
check_threshold()
{
    local variant="$1" label="$2" actual="$3" ceiling="$4"
    if [ -z "$actual" ]; then
        printf "  ${C_RED}MISS${C_RESET}  %-12s %-22s no measurement reported\n" \
            "$variant" "$label" >&2
        failures=$((failures + 1))
        deterministic=1
        return
    fi
    awk -v a="$actual" -v c="$ceiling" 'BEGIN { exit !(a <= c) }'
    if [ $? -eq 0 ]; then
        printf "  ${C_GREEN}OK${C_RESET}    %-12s %-22s %7.1f ns/op  (ceiling %d)\n" \
            "$variant" "$label" "$actual" "$ceiling"
    else
        printf "  ${C_RED}FAIL${C_RESET}  %-12s %-22s %7.1f ns/op  > %d\n" \
            "$variant" "$label" "$actual" "$ceiling" >&2
        failures=$((failures + 1))
    fi
}

check_ratio()
{
    local variant="$1" label="$2" actual="$3" base="$4" ceiling="$5"
    if [ -z "$actual" ] || [ -z "$base" ]; then
        printf "  ${C_RED}MISS${C_RESET}  %-12s %-22s no measurement reported\n" \
            "$variant" "$label" >&2
        failures=$((failures + 1))
        deterministic=1
        return
    fi
    local ratio
    ratio="$(awk -v a="$actual" -v b="$base" \
        'BEGIN { if (b <= 0) print ""; else printf "%.1f", a / b }')"
    if [ -z "$ratio" ]; then
        printf "  ${C_RED}MISS${C_RESET}  %-12s %-22s getpid baseline was zero\n" \
            "$variant" "$label" >&2
        failures=$((failures + 1))
        deterministic=1
        return
    fi
    if awk -v r="$ratio" -v c="$ceiling" 'BEGIN { exit !(r <= c) }'; then
        printf "  ${C_GREEN}OK${C_RESET}    %-12s %-22s %7.1f ns/op  (%sx getpid, ceiling %dx)\n" \
            "$variant" "$label" "$actual" "$ratio" "$ceiling"
    else
        printf "  ${C_RED}FAIL${C_RESET}  %-12s %-22s %7.1f ns/op  %sx getpid > %dx\n" \
            "$variant" "$label" "$actual" "$ratio" "$ceiling" >&2
        failures=$((failures + 1))
    fi
}

run_one_pass()
{
    local variant="$1" bench="$2" stat_ratio="$3"
    shift 3
    local out
    out="$(mktemp)"
    if ! "$ELFUSE" "$@" "$bench" "$ITERS" > "$out" 2>&1; then
        echo "  ${C_RED}FAIL${C_RESET}  $variant bench exited non-zero" >&2
        cat "$out" >&2
        failures=$((failures + 1))
        deterministic=1
        rm -f "$out"
        return
    fi

    local getpid_ns
    getpid_ns="$(extract_ns "$out" getpid)"

    check_threshold "$variant" "getpid" "$getpid_ns" "$THRESH_GETPID"
    check_threshold "$variant" "clock_gettime" \
        "$(extract_ns "$out" clock_gettime)" "$THRESH_CLOCK_GETTIME"
    check_threshold "$variant" "read-urandom1" \
        "$(extract_ns "$out" read-urandom1)" "$THRESH_URANDOM"
    check_ratio "$variant" "stat-path" \
        "$(extract_ns "$out" stat-path)" "$getpid_ns" "$stat_ratio"

    rm -f "$out"
}

# Re-measure once before failing, and report only what survives the retry.
#
# Every lane here shares the machine with whatever else is running, and the
# lanes do not degrade together: getpid is served inside the shim and never
# leaves the process, while stat-path makes a real host filesystem call. Under
# filesystem load the second moves and the first does not, so the ratio between
# them climbs on a build where nothing changed. Measured: a `make clean` build
# put stat-path at 205x against a 105x ceiling purely because Gatekeeper was
# scanning the binaries that had just been produced, and the same binary on a
# quiet machine came back at 87x.
#
# A retry separates the two cases at the cost of one extra run on the rare bad
# pass: real slowdowns are in the code and reproduce, load spikes usually do
# not. A gate that fails on a clean tree gets ignored, which costs more than the
# regressions it would have caught.
#
# Only a measurement over its limit is retried. A bench that exited non-zero or
# produced no number for a lane failed for a reason a second run reproduces, so
# re-running it would burn a full pass and report the wrong cause.
run_and_check()
{
    local variant="$1"
    local before=$failures
    benchmarks_run=$((benchmarks_run + 1))

    deterministic=0
    run_one_pass "$@"
    if [ "$failures" -eq "$before" ] || [ "$deterministic" -eq 1 ]; then
        return
    fi

    printf "  ${C_YELLOW}RETRY${C_RESET} %-12s first pass exceeded a limit; re-measuring\n" \
        "$variant"
    failures=$before
    deterministic=0
    run_one_pass "$@"

    if [ "$failures" -ne "$before" ] && [ "$deterministic" -eq 0 ] \
        && host_is_busy; then
        printf "  ${C_YELLOW}SKIP${C_RESET}  %-12s host load %s over %s cpus; throughput not measurable\n" \
            "$variant" "$(sysctl -n vm.loadavg 2> /dev/null | awk '{print $2}')" \
            "$(sysctl -n hw.ncpu 2> /dev/null)"
        failures=$before
        unmeasured=$((unmeasured + 1))
    fi
}

echo "=== bench-guardrail (iters=$ITERS) ==="

if [ "$run_static" = 1 ]; then
    echo "[static (musl)]"
    run_and_check static "$STATIC_BENCH" "$THRESH_STAT_RATIO_STATIC"
fi

if [ -x "$GLIBC_BENCH" ] && [ -d "$GLIBC_SYSROOT" ]; then
    echo "[dynamic-glibc]"
    run_and_check dyn-glibc "$GLIBC_BENCH" "$THRESH_STAT_RATIO_GLIBC" \
        --sysroot "$GLIBC_SYSROOT"
else
    /usr/bin/printf "  ${C_YELLOW}SKIP${C_RESET}  dyn-glibc      cross-toolchain absent: %s\n" \
        "$GLIBC_TOOLCHAIN"
fi

if [ "$benchmarks_run" -eq 0 ]; then
    echo
    echo "guardrail FAILED (no benchmark variants were available to run)" >&2
    exit 1
fi

if [ "$failures" -ne 0 ]; then
    echo
    echo "guardrail FAILED ($failures threshold violation(s))" >&2
    exit 1
fi

# A variant whose thresholds were exceeded twice and then dropped for host load
# was not measured, so this run is no evidence that the ceilings hold. It exits
# non-zero for the same reason it would on a real violation: make check is the
# CI gate, and a run that never exercised the ceilings must not report the same
# status as one that did.
#
# What the load check buys is the diagnosis, not the exit code. The bare
# threshold number this replaces said nothing about why, and cost more than one
# investigation before anyone thought to look at the host.
if [ "$unmeasured" -ne 0 ]; then
    echo
    echo "guardrail UNMEASURED ($unmeasured variant(s), host busy)" >&2
    echo "  the ceilings were not exercised; re-run on an idle host" >&2
    exit 1
fi
echo
echo "guardrail PASS"
exit 0
