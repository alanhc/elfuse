# Stack sample for a test about to hit its watchdog
#
# Copyright 2026 elfuse contributors
# SPDX-License-Identifier: Apache-2.0

# shellcheck shell=bash
# A hang that only reproduces under suite load is otherwise reported as a bare
# "timeout after Ns" with nothing to diagnose. Both test entry points
# (tests/driver.sh and tests/lib/test-runner.sh) arm this around every
# invocation: it samples the live process shortly before timeout(1) kills it,
# and the caller keeps the output only when the watchdog actually fired.
#
# Set TEST_SAMPLE_TIMEOUTS=0 to turn it off.
#
# Usage:
#   hang_sample_arm <binary> <timeout_secs> <outfile>
#   ... run the command, recording its exit status ...
#   hang_sample_finish <timed_out>            # 1 keeps the sample, 0 discards

# Set by hang_sample_arm, read by hang_sample_finish. Empty when disarmed.
hang_sample_out=""
hang_sample_sentinel=""
hang_sample_pid=""
hang_sample_work=""

# Bumped per arm so the sentinel and the working file are unique to one
# invocation. Sharing them across arms is what forced the passing path to block
# until the watchdog exited: a watchdog still symbolizing would otherwise write
# into the next test's file. With distinct names per arm a straggler can only
# touch its own, so only the timed-out path has any reason to wait.
_hang_sample_seq=0

# The watchdog polls a sentinel file rather than being killed by pid: lanes that
# spawn hundreds of short-lived processes (the busybox applets) can recycle a
# pid between the watchdog exiting and a kill landing, which would turn this
# into a random SIGTERM.
_hang_sample_watch()
{
    local binary="$1" cap="$2" out="$3" sentinel="$4"

    # Leave room for the collection plus symbolization, which is the slow part:
    # measured around five seconds on a loaded machine, and anything that runs
    # past the kill sees a process that is already gone.
    local lead=$((cap - 8))
    [ "$lead" -lt 1 ] && lead=1

    # Quarter-second granularity bounds how long a timed-out test waits for a
    # watchdog that has not started collecting yet.
    local waited=0 steps=$((lead * 4))
    while [ "$waited" -lt "$steps" ]; do
        [ -e "$sentinel" ] || return 0
        sleep 0.25
        waited=$((waited + 1))
    done
    [ -e "$sentinel" ] || return 0

    # Match the process name, not the command line: elfuse renames itself to the
    # guest program (src/runtime/proctitle.c), so the guest's basename finds it.
    # A -f match would find timeout(1) first, whose argv carries the whole
    # command line, and a sample of the watchdog wrapper says nothing.
    local pid
    pid=$(pgrep -n "$(basename "$binary")" 2> /dev/null) || return 0
    [ -z "$pid" ] && return 0

    # Per-thread state first: it is instant and always reads, while sample can
    # spend seconds symbolizing and has come back with an empty call graph.
    # Doing it second measured an empty table, because the process was killed
    # while sample was still working.
    [ -e "$sentinel" ] || return 0
    {
        printf "%s\n" "---- ps -M $pid ----"
        ps -M "$pid" 2>&1
        printf "\n"
    } > "$out" 2> /dev/null

    sample "$pid" 1 -f "${out}.sample" > /dev/null 2>&1

    # Re-check before appending. Symbolization can run for seconds, and the test
    # may have passed and been reaped in that time; without this the append
    # recreates a path the parent just removed and leaves it behind for good if
    # no later arm sweeps it.
    if [ -e "$sentinel" ]; then
        cat "${out}.sample" >> "$out" 2> /dev/null
    else
        rm -f "$out"
    fi
    rm -f "${out}.sample"
}

hang_sample_arm()
{
    local binary="$1" cap="$2" out="$3"

    hang_sample_out=""
    hang_sample_sentinel=""
    hang_sample_pid=""
    hang_sample_work=""
    [ "${TEST_SAMPLE_TIMEOUTS:-1}" = 1 ] || return 0
    command -v sample > /dev/null 2>&1 || return 0

    mkdir -p "$(dirname "$out")" 2> /dev/null || return 0

    # Sweep whatever earlier arms left behind. A watchdog that was still writing
    # when its test passed owns a name no live arm uses, so removing it here is
    # safe even if that watchdog has not exited: the write goes to an unlinked
    # fd and the directory entry is gone.
    rm -f "${out}".part.* "${out}".running.*

    _hang_sample_seq=$((_hang_sample_seq + 1))
    hang_sample_out="$out"
    hang_sample_work="${out}.part.$$.${_hang_sample_seq}"
    hang_sample_sentinel="${out}.running.$$.${_hang_sample_seq}"
    : > "$hang_sample_sentinel"
    _hang_sample_watch "$binary" "$cap" "$hang_sample_work" \
        "$hang_sample_sentinel" &
    hang_sample_pid=$!
}

hang_sample_finish()
{
    local timed_out="$1"

    [ -n "$hang_sample_sentinel" ] && rm -f "$hang_sample_sentinel"
    if [ -z "$hang_sample_out" ]; then
        return 0
    fi

    if [ "$timed_out" = 1 ]; then

        # Only here is the watchdog's output wanted, so only here is it worth
        # waiting for: the collection starts before timeout(1) kills the test,
        # but symbolization can run for seconds after it, and the move below
        # needs a finished file. A test that already burned its whole timeout
        # pays this; a passing one must not.
        [ -n "$hang_sample_pid" ] && wait "$hang_sample_pid" 2> /dev/null
        if [ -s "$hang_sample_work" ]; then
            mv "$hang_sample_work" "$hang_sample_out"
            printf "  sampled hung %s to %s\n" \
                "$(basename "${hang_sample_out%-hang.txt}")" \
                "$hang_sample_out" >&2
        fi
    fi

    # The sentinel is already gone, so a watchdog that has not started writing
    # returns without touching anything. One that is mid-symbolization keeps a
    # name no later arm will use, and the sweep in hang_sample_arm collects it.
    rm -f "$hang_sample_work"

    hang_sample_out=""
    hang_sample_sentinel=""
    hang_sample_pid=""
    hang_sample_work=""
}
