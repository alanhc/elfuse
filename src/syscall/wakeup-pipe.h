/*
 * Blocking-wait wakeup pipe
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A process-wide self-pipe whose read end joins every host poll/select/kevent
 * that would otherwise wait forever, so exit_group, futex_interrupt, and guest
 * signal delivery reach a thread parked outside hv_vcpu_run(), where
 * hv_vcpus_exit() cannot.
 */

#pragma once

/* Create the pipe, once. Main thread only: the one-shot gate is an
 * unsynchronized check-then-act, and by the time syscall_init(), the only
 * caller, reaches it the sigwait thread is already reading the fds.
 */
void wakeup_pipe_init(void);

/* Wake every thread parked on the read end. Safe from a signal-handling
 * thread; a missing pipe is a no-op.
 */
void wakeup_pipe_signal(void);

/* The read end to add to a blocking wait, or -1 when the pipe is missing.
 * poll(2) ignores a negative fd, so callers can pass the result through.
 */
int wakeup_pipe_read_fd(void);

/* Consume every queued byte. The pipe is nonblocking, so this cannot park. */
void wakeup_pipe_drain(void);
