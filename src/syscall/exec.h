/*
 * execve syscall handler
 *
 * Copyright 2026 elfuse contributors
 * Copyright 2025 Moritz Angermann, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Implements execve: reads path/argv/envp from guest memory, reloads ELF,
 * resets guest state, rebuilds page tables, and restarts at new entry point.
 */

#pragma once

#include <Hypervisor/Hypervisor.h>
#include <stdbool.h>
#include <stdint.h>
#include "core/guest.h"

/* Execute a new binary, replacing current process image. Reads path, argv[],
 * envp[] from guest memory, reloads ELF, resets state.
 * Returns SYSCALL_EXEC_HAPPENED on success (caller skips X0 write), or negative
 * Linux errno on failure. execve handoff to the thread group leader.
 *
 * Linux de_thread() destroys the leader and gives its tid to the exec'ing
 * thread. elfuse cannot: the leader is the main host thread, and its run loop
 * returning is what tears the process down. So a non-leader execve is handed to
 * the leader instead, which runs the whole syscall on its own vCPU. The result
 * is what Linux produces, the new image runs single-threaded with gettid() ==
 * getpid(), and the requester dies as a sibling in de_thread.
 *
 * Whether a request is waiting is published through
 * thread_set_leader_work_pending, so both readers (the leader's run loop and
 * thread_stop_requested) ask the thread table rather than the exec layer.
 */

/* Wake anything parked in the handoff, so a requester whose exec already
 * succeeded notices that de_thread is reaping it instead of sitting out its
 * safety-net quantum. Part of thread_wake_all_blocked's wake set.
 */
void exec_handoff_wake_waiters(void);

/* Leader side: run a pending handoff to completion on this vCPU.
 *
 * Returns SYSCALL_EXEC_HAPPENED when the new image is installed (the caller
 * resumes the vCPU on the rebuilt registers), or 0 when the exec failed before
 * its point of no return and the requester was given the errno.
 */
int64_t exec_run_handoff(hv_vcpu_t vcpu, guest_t *g, bool verbose);

int64_t sys_execve(hv_vcpu_t vcpu,
                   guest_t *g,
                   uint64_t path_gva,
                   uint64_t argv_gva,
                   uint64_t envp_gva,
                   bool verbose,
                   const char *host_path);
