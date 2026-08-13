/*
 * Blocking-wait wakeup pipe
 *
 * Copyright 2026 elfuse contributors
 * Copyright 2025 Moritz Angermann, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The self-pipe every indefinite host poll/select/kevent waits on alongside
 * its real fds. Free of syscall-layer dependencies, so the concurrency
 * contract in wakeup-pipe.h can be tested on its own.
 */

#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>

#include "syscall/wakeup-pipe.h"

/* Read by the sigwait thread and by every guest thread while wakeup_pipe_init()
 * is still assigning them, which plain int would let the compiler tear or fold.
 * Relaxed suffices: each fd is read on its own and publishes no other state,
 * and pipe(2) creates the descriptors before either store.
 */
static _Atomic int wakeup_pipe_rd = -1, wakeup_pipe_wr = -1;

void wakeup_pipe_init(void)
{
    /* Replacing a published pair strands every thread parked on the old fd. */
    if (atomic_load_explicit(&wakeup_pipe_rd, memory_order_relaxed) >= 0)
        return;

    int pipefd[2];
    if (pipe(pipefd) == 0) {
        fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
        fcntl(pipefd[1], F_SETFL, O_NONBLOCK);
        atomic_store_explicit(&wakeup_pipe_rd, pipefd[0], memory_order_relaxed);
        atomic_store_explicit(&wakeup_pipe_wr, pipefd[1], memory_order_relaxed);
    }
}

void wakeup_pipe_signal(void)
{
    int wr = atomic_load_explicit(&wakeup_pipe_wr, memory_order_relaxed);
    if (wr >= 0) {
        uint8_t byte = 1;
        write(wr, &byte, 1);
    }
}

int wakeup_pipe_read_fd(void)
{
    return atomic_load_explicit(&wakeup_pipe_rd, memory_order_relaxed);
}

void wakeup_pipe_drain(void)
{
    int rd = atomic_load_explicit(&wakeup_pipe_rd, memory_order_relaxed);
    if (rd < 0)
        return;

    uint8_t drain;
    while (read(rd, &drain, 1) > 0)
        ;
}
