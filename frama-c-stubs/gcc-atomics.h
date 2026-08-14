/*
 * GCC atomic and overflow builtins, modeled for the analyzer only
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Frama-C's libc models the size-suffixed atomics (__atomic_fetch_or_4 and
 * friends), __atomic_thread_fence, __sync_synchronize and the __builtin_
 * bit-counting and overflow-checking ones, in __fc_gcc_builtins.h. Nothing in
 * the modeled libc includes that header, and it has no entry at all for the
 * type-generic __atomic_*_n forms this tree actually calls.
 *
 * A source calling one of those gets an implicit declaration, whose argument
 * types are inferred per translation unit, so two files that pass different
 * widths conflict the moment they are loaded together: "Incompatible
 * declaration for __atomic_store_n, different integer types, unsigned long and
 * int". That is why a file could be proved on its own and still not join a
 * whole-program load.
 *
 * So this pulls in what Frama-C already models and adds only the generics it
 * does not. Redefining one it declares is an error rather than an override: a
 * function-like macro over its prototype fails the preprocessor outright.
 *
 * Modeled as the single-threaded reads and writes they reduce to, which is the
 * same trade -D_Atomic= already makes in mk/analysis.mk and carries the same
 * limit: sound for the per-function runtime-error and bounds obligations these
 * targets discharge, NOT sound for any analysis of concurrent behaviour. The
 * memory order argument is evaluated and discarded, so a call that computes it
 * keeps whatever side effect that had.
 *
 * Statement expressions rather than plain macros because fetch_ and exchange_
 * return the value from BEFORE the update, and a comma expression cannot hold
 * it. Frama-C accepts ({ ... }) and __typeof__; that is checked by the proof
 * targets that use this header, since a rejected construct fails the parse.
 *
 * Reached only through FRAMAC_CPP_ARGS, which force-includes it. A compile
 * never sees this file and keeps the real builtins.
 */

#pragma once

/* Everything Frama-C already models, including contracts. Included here rather
 * than left out because nothing in the modeled libc pulls it in, so without
 * this __builtin_ctzll and __builtin_add_overflow are implicit declarations
 * too.
 */
#include <__fc_gcc_builtins.h>

/* Frama-C's own handling of the _Atomic qualifier, which its front end cannot
 * parse. stdatomic.h carries "#define _Atomic" with the comment "_Atomic is
 * currently ignored by Frama-C", so this is the analyzer's stated position on
 * the keyword rather than a flag invented here. Taken from that header instead
 * of restated as a -D, so the concession lives next to the atomics model that
 * shares its reasoning, and so it moves when Frama-C's does.
 *
 * src/syscall/linux-wire.h qualifies one fd_entry_t field, and the tree does
 * not include stdatomic.h anywhere, which is why the definition has to arrive
 * ahead of the source rather than through a normal include.
 */
#include <stdatomic.h>

/* The order argument is (void)-cast rather than dropped, so a caller passing an
 * expression with a side effect still gets it.
 */
#define __atomic_load_n(ptr, order) ((void) (order), *(ptr))

#define __atomic_store_n(ptr, val, order) \
    ((void) (order), (void) (*(ptr) = (val)))

#define __atomic_load(ptr, ret, order) \
    ((void) (order), (void) (*(ret) = *(ptr)))

#define __atomic_fetch_or(ptr, val, order)                \
    ({                                                    \
        (void) (order);                                   \
        __typeof__(*(ptr)) __fc_old = *(ptr);             \
        *(ptr) = (__typeof__(*(ptr))) (__fc_old | (val)); \
        __fc_old;                                         \
    })

#define __atomic_fetch_and(ptr, val, order)               \
    ({                                                    \
        (void) (order);                                   \
        __typeof__(*(ptr)) __fc_old = *(ptr);             \
        *(ptr) = (__typeof__(*(ptr))) (__fc_old & (val)); \
        __fc_old;                                         \
    })

#define __atomic_fetch_add(ptr, val, order)               \
    ({                                                    \
        (void) (order);                                   \
        __typeof__(*(ptr)) __fc_old = *(ptr);             \
        *(ptr) = (__typeof__(*(ptr))) (__fc_old + (val)); \
        __fc_old;                                         \
    })

#define __atomic_exchange_n(ptr, val, order)  \
    ({                                        \
        (void) (order);                       \
        __typeof__(*(ptr)) __fc_old = *(ptr); \
        *(ptr) = (val);                       \
        __fc_old;                             \
    })

/* Always succeeds when the comparison holds, which is the strong form. The weak
 * flag is discarded: a spurious failure is a thread-visible behaviour, and
 * nothing modeled here has threads.
 */
#define __atomic_compare_exchange_n(ptr, expected, desired, weak, succ, fail) \
    ({                                                                        \
        (void) (weak);                                                        \
        (void) (succ);                                                        \
        (void) (fail);                                                        \
        int __fc_ok = (*(ptr) == *(expected));                                \
        if (__fc_ok)                                                          \
            *(ptr) = (desired);                                               \
        else                                                                  \
            *(expected) = *(ptr);                                             \
        __fc_ok;                                                              \
    })

/* __atomic_thread_fence and __sync_synchronize are declared by the header
 * above, with "assigns \nothing". Nothing to add.
 */
