/*
 * Linux ABI size limits
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Split out of syscall/internal.h so a file that only needs a Linux ABI limit
 * for buffer sizing (core/bootstrap.h, core/sysroot.h) does not have to pull in
 * internal.h's cross-module locks and FD table declarations to get it.
 *
 * At src/ root rather than under syscall/ for the same reason one step on: a
 * constant the Linux ABI fixes belongs to no layer, and core/ reaching into
 * syscall/ to read one made the include graph say the two are coupled when only
 * a number passes between them.
 */

#pragma once

/* Linux PATH_MAX (4096): used for path buffer sizing in syscall handlers. The
 * literal 4096 in core/stack.c (the AT_PAGESZ auxv entry) means actual page
 * size, not this.
 */
#define LINUX_PATH_MAX 4096

/* Smallest sigaltstack Linux aarch64 accepts, and what AT_MINSIGSTKSZ reports.
 * sys_sigaltstack rejects anything below it, so the auxv value and the check
 * have to be the same number.
 */
#define LINUX_MINSIGSTKSZ 5120
