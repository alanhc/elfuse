/*
 * Mach arm64 thread-state stub for Frama-C
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * macOS supplies this header; Frama-C's modeled libc does not, and its absence
 * is the only thing that stopped src/syscall/signal.c from parsing. Only the
 * one name signal.c reaches for is declared: arm_thread_state64_set_pc_fptr,
 * which on a real arm64e host signs the pointer before storing it and on arm64
 * is a plain assignment. The proof does not depend on which, because no proved
 * function touches thread state; this exists so the file parses at all.
 */

#pragma once

#include <stdint.h>

#ifndef __arm_thread_state64_stub_defined
#define __arm_thread_state64_stub_defined

#define arm_thread_state64_set_pc_fptr(ts, fptr) \
    ((void) ((ts).__pc = (uint64_t) (uintptr_t) (void *) (fptr)))

#endif
