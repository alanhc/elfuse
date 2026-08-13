/*
 * Signal frame placement arithmetic: the parts a proof can reach
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * signal_deliver pushes an rt_sigframe onto the guest stack and points the
 * handler at it. Where that frame lands has to satisfy two things at once: the
 * base must be 16-byte aligned, because the handler runs with it as SP under
 * AAPCS64, and the frame must sit wholly below the interrupted SP without the
 * subtraction wrapping. A frame placed wrong does not fail cleanly; the handler
 * runs on a misaligned stack or over memory that was still live.
 *
 * Split out of signal.c because that file cannot be given to Frama-C: it and
 * signal.h include Hypervisor.framework. This header needs nothing but
 * stdint.h, so make verify-sigframe proves it directly. It names no frame type
 * at all, taking the frame size as a plain integer, which is what lets it be
 * proved without moving the guest ABI structs.
 *
 * Scope: placement only. Whether the frame's field offsets match the Linux ABI,
 * whose layout is fixed by arch/arm64/kernel/signal.c, is the property that
 * actually breaks libc when it is wrong, and nothing here establishes it. No
 * golden-layout test covers it either, so that property is currently unchecked
 * by anything in the tree.
 */

#pragma once

#include <stdint.h>

/* AAPCS64: the handler's SP must be 16-byte aligned. */
#define SIGFRAME_ALIGN 16ULL

_Static_assert(SIGFRAME_ALIGN == 16ULL,
               "AAPCS64 requires 16-byte stack alignment");

/* Place a frame of frame_bytes below sp.
 *
 * Returns 1 and writes the base to *base, or returns 0 when it does not fit
 * above floor.
 *
 * On failure *base is left untouched rather than zeroed, so a caller must
 * branch on the return value; the contract states that explicitly.
 *
 * floor is the lowest address the frame may occupy. On the alternate signal
 * stack that is the altstack base, which is the bound the caller otherwise has
 * no way to state; on the normal stack the caller passes 0, since the frame
 * lands wherever the interrupted SP was and the write itself is bounds-checked.
 *
 * The alignment, the fits-below-SP bound (which is what the underflow guard was
 * written for), and the floor each have a mutation that breaks them. The
 * distance bound does something narrower than it looks: together with the
 * alignment it leaves exactly one legal base, so the frame cannot sit a slot
 * lower than alignment requires. The iff stops the cheapest cheat of all, since
 * every other clause is guarded by a non-zero result and a body that always
 * refuses satisfies them without computing anything.
 */
/*@
  requires \valid(base);
  assigns *base;
  ensures \result == 0 || \result == 1;
  ensures \result != 0 <==>
            (frame_bytes <= sp &&
             (sp - frame_bytes) - (sp - frame_bytes) % SIGFRAME_ALIGN
               >= floor);
  ensures \result != 0 ==> *base % SIGFRAME_ALIGN == 0;
  ensures \result != 0 ==> *base + frame_bytes <= sp;
  ensures \result != 0 ==> *base >= floor;
  ensures \result != 0 ==> sp - *base < frame_bytes + SIGFRAME_ALIGN;
  ensures \result == 0 ==> *base == \old(*base);
 */
static inline int sigframe_base(uint64_t sp,
                                uint64_t frame_bytes,
                                uint64_t floor,
                                uint64_t *base)
{
    if (frame_bytes > sp)
        return 0;

    /* Align down, written as subtract-the-remainder rather than "& ~15": the
     * compiler emits the same instruction, and the prover reasons about the
     * arithmetic form without first establishing that the mask is one less than
     * a power of two. Same reason src/proved/gva.h uses "% granule".
     */
    uint64_t candidate = sp - frame_bytes;
    candidate -= candidate % SIGFRAME_ALIGN;

    if (candidate < floor)
        return 0;

    *base = candidate;
    return 1;
}
