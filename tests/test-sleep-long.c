/*
 * test-sleep-long.c -- block in one host syscall for longer than a watchdog
 * period, then exit cleanly.
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A fixture for tests/test-vcpu-watchdog.sh, not a standalone test: on its own
 * it only sleeps, and it is the watchdog period it runs under that gives it
 * meaning.
 *
 * The watchdog must tell "wedged in EL0" from "waiting in a host syscall". Only
 * the first is a hang. Driven by tests/test-vcpu-watchdog.sh, which runs this
 * with a period shorter than the sleep.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv)
{
    /* Seconds from argv so the caller ties this to its own watchdog period. A
     * compiled-in constant has no textual link to the script's PERIOD, so
     * raising PERIOD would silently stop this crossing a tick.
     */
    long secs = argc > 1 ? strtol(argv[1], NULL, 10) : 5;
    struct timespec req = {.tv_sec = secs, .tv_nsec = 0};
    if (nanosleep(&req, NULL) != 0) {
        puts("nanosleep interrupted");
        return 1;
    }
    puts("slept");
    return 0;
}
