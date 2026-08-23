/*
 * Darwin ucontext stub for Frama-C
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * macOS supplies this header; Frama-C's modeled libc does not. src/syscall/
 * signal.c reaches into a host ucontext in exactly one place, the SIGBUS
 * recovery handler, which redirects the interrupted context's PC. Only the
 * shape that reaches it needs declaring: uc_mcontext is a pointer to a machine
 * context whose __ss member carries the arm64 thread state.
 *
 * The Linux sigcontext the guest sees is elfuse's own type in
 * syscall/linux-wire.h and has nothing to do with this one. Both appear in
 * signal.c; do not conflate them when reading it.
 */

#pragma once

#ifndef __darwin_ucontext_stub_defined
#define __darwin_ucontext_stub_defined

#include <stdint.h>

struct __darwin_arm_thread_state64 {
    uint64_t __x[29];
    uint64_t __fp;
    uint64_t __lr;
    uint64_t __sp;
    uint64_t __pc;
    uint32_t __cpsr;
    uint32_t __pad;
};

/* __es first, as on the real SDK, so __ss sits at its true offset of 16. No
 * proof reasons about the offset today and nothing here is compiled for real,
 * but a stub that silently disagrees with the header it stands in for is the
 * kind that answers a future question wrongly.
 */
struct __darwin_arm_exception_state64 {
    uint64_t __far;
    uint32_t __esr;
    uint32_t __exception;
};

struct __darwin_mcontext64 {
    struct __darwin_arm_exception_state64 __es;
    struct __darwin_arm_thread_state64 __ss;
};

typedef struct __darwin_ucontext {
    int uc_onstack;
    unsigned int uc_sigmask;
    struct __darwin_mcontext64 *uc_mcontext;
} ucontext_t;

#endif
