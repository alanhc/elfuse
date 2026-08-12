/*
 * Native-host unit test for the wakeup pipe's concurrency contract
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A reader thread runs wakeup_pipe_signal() and wakeup_pipe_read_fd() across
 * the main thread's wakeup_pipe_init(), the pairing ThreadSanitizer reported
 * on wakeup_pipe_wr under test-fork-exec. Only a -fsanitize=thread build has a
 * race detector; elsewhere this checks init, idempotency, and drain.
 */

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#include "host-test-util.h"

#include "syscall/wakeup-pipe.h"

/* Plain bool here would be the defect under test, blamed on the harness. */
static atomic_bool reader_run = true;

/* Pacing is what makes the race observable: an unpaced loop churns
 * ThreadSanitizer's history until a conflicting access is evicted before its
 * partner arrives. With the fds plain: unpaced 0 of 20 runs, paced 20 of 20.
 */
#define RACE_WINDOW_US 20000
#define READER_PACE_US 200

static void *reader_main(void *arg)
{
    (void) arg;
    while (atomic_load_explicit(&reader_run, memory_order_relaxed)) {
        wakeup_pipe_signal();
        (void) wakeup_pipe_read_fd();
        usleep(READER_PACE_US);
    }
    return NULL;
}

int main(void)
{
    host_check(wakeup_pipe_read_fd() == -1, "unset read end",
               "the read end must be -1 before init");

    pthread_t reader;
    if (pthread_create(&reader, NULL, reader_main, NULL) != 0) {
        fprintf(stderr, "FAIL thread: cannot start the reader thread\n");
        return 1;
    }

    /* Bracket the one-shot store with loads already in flight: a race is
     * reported only when an access finds a conflicting one recorded.
     */
    usleep(RACE_WINDOW_US);

    wakeup_pipe_init();

    usleep(RACE_WINDOW_US);

    atomic_store_explicit(&reader_run, false, memory_order_relaxed);
    pthread_join(reader, NULL);

    int fd = wakeup_pipe_read_fd();
    host_check(fd >= 0, "init", "init must publish a read end");

    /* Regression guard: syscall_init() runs once per process. */
    wakeup_pipe_init();
    host_check(wakeup_pipe_read_fd() == fd, "idempotent init",
               "a second init must keep the first read end");

    wakeup_pipe_drain();
    char byte;
    host_check(read(fd, &byte, 1) == -1 && errno == EAGAIN, "drain",
               "drain must leave the pipe empty and nonblocking");

    wakeup_pipe_signal();
    host_check(read(fd, &byte, 1) == 1, "signal", "signal must queue a byte");

    return host_summary("test-wakeup-pipe-host");
}
