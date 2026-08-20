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

/* Host descriptors kept available for elfuse's internal operations. Blocking
 * I/O may hold two duplicated descriptors per guest thread (for example,
 * copy_file_range()), while another bounded slice covers runtime pipes, fork
 * IPC, debugger sockets, and sysroot/FUSE plumbing. Operations whose descriptor
 * use grows with their input set, such as ppoll(), require separate accounting.
 */
#define HOST_FD_RESERVE 256

/* Minimum host soft limit accepted by elfuse at startup. */
#define HOST_NOFILE_MIN (FD_TABLE_SIZE + HOST_FD_RESERVE)
