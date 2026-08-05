/*
 * openat2(2) ABI definitions for guest test programs
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The cross toolchains that build the guest tests predate openat2 in their
 * libc headers, so the syscall number, struct open_how, and the RESOLVE_*
 * flags (include/uapi/linux/openat2.h) are spelled here once for every test
 * that calls the syscall raw.
 */

#pragma once

#ifndef SYS_openat2
#define SYS_openat2 437
#endif

struct open_how {
    unsigned long long flags, mode, resolve;
};

#define RESOLVE_NO_XDEV 0x01
#define RESOLVE_NO_MAGICLINKS 0x02
#define RESOLVE_NO_SYMLINKS 0x04
#define RESOLVE_BENEATH 0x08
#define RESOLVE_IN_ROOT 0x10

/* Linux's symlink budget (MAXSYMLINKS, include/linux/namei.h) counts links
 * actually followed; a link-free path has no component limit at all
 * (path_resolution(7)). Tests size their directory chains against it to
 * expose a walker that charges the budget once per component instead. Not
 * named MAXSYMLINKS because glibc claims that name in sys/param.h.
 */
#define WALK_MAXSYMLINKS 40
