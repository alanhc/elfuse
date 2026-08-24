/*
 * libkern/OSCacheControl.h, for the analyzer only
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Darwin's cache maintenance calls. elfuse issues sys_icache_invalidate after
 * writing instructions the guest is about to fetch: the shim, the loaded
 * segments at bootstrap and again at execve, and the Rosetta image. Frama-C
 * models a portable libc, which has no such header, so the three sources naming
 * it (core/bootstrap.c, core/rosetta.c, syscall/exec.c) stopped before parsing.
 *
 * Declarations only, and deliberately no body: what these do is invisible to
 * the analyzer's memory model, which has no instruction cache to be stale. A
 * hand-written body would be a claim about hardware rather than about the
 * program. Signatures match the SDK header.
 *
 * Same placement rule as the Hypervisor stub: outside src/, reachable only
 * through FRAMAC_STUB_DIR in mk/verify.mk, never on a compile's include path.
 */

#pragma once

#include <stddef.h>

void sys_icache_invalidate(void *start, size_t len);
void sys_dcache_flush(void *start, size_t len);
