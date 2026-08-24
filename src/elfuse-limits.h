/*
 * Shared capacity limits used by the host runtime and regression harnesses.
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Keep the descriptor accounting in one header. Shell test runners derive their
 * limits from this file instead of copying the arithmetic from main.c.
 */

#pragma once

/* Maximum number of guest-visible file descriptors. */
#define FD_TABLE_SIZE 1024

/* Host descriptors kept available for elfuse's internal operations: runtime
 * pipes, fork IPC, debugger sockets, and sysroot/FUSE plumbing. A syscall's own
 * reference to a guest descriptor is not part of that budget, because
 * host_fd_ref_open borrows or pins the fd table's descriptor rather than
 * duplicating it, so the reserve does not scale with thread count or with the
 * length of a call's fd set.
 */
#define HOST_FD_RESERVE 256

/* Minimum host soft limit accepted by elfuse at startup. */
#define HOST_NOFILE_MIN (FD_TABLE_SIZE + HOST_FD_RESERVE)
