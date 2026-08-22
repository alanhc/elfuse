/*
 * SIGIO knote identity: the packing a proof can reach
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * asyncio.c hands kqueue a udata word that has to name the fd a SIGIO belongs
 * to, and it cannot be a bare guest fd number: a slot closed and reopened
 * between arming the knote and the event firing would deliver the signal to
 * whatever occupies that number now. The word therefore carries the fd in its
 * low 16 bits and the slot generation stamped at arm time above it, and the
 * delivery path drops any event whose generation no longer matches.
 *
 * That makes the packing an ABA guard, and the guard is only as good as the
 * arithmetic: a fd that did not survive the round trip would deliver to the
 * wrong slot, and a generation that aliased would defeat the check the whole
 * scheme exists for. Both were asserted in a comment ("FD_TABLE_SIZE is 1024
 * (fits 16 bits) and the generation counter is monotonic, so 48 bits will not
 * wrap") and checked by nothing.
 *
 * Split into a header because asyncio.c cannot be given to Frama-C: it includes
 * sys/event.h for kqueue, which the analyzer's libc does not model and no stub
 * of ours can honestly supply. This header needs nothing but stdint.h, so make
 * verify-asyncudata proves it directly.
 */

#pragma once

#include <stdint.h>

#include "elfuse-limits.h"

/* The fd occupies the low 16 bits, the generation the remaining 48.
 *
 * Spelled as a span to divide and modulo by rather than as a shift and a mask.
 * The two are the same operation on an unsigned value and compile to the same
 * instructions, but a symbolic shift is a postcondition no prover here
 * discharges: written with & and >>, all three of these time out at 120s under
 * both Alt-Ergo and Z3, and written this way they close in milliseconds.
 * src/proved/fdset.h made the same choice for the same reason.
 */
#define ASYNC_UDATA_FD_BITS 16
#define ASYNC_UDATA_GEN_BITS 48

/* Stated as bit widths and derived spans, so the layout invariant is one the
 * compiler can actually check. An earlier spelling asserted GEN_SPAN * FD_SPAN
 * == 0, which is true through unsigned wraparound and
 * therefore asserted nothing: the || short-circuited and the real claim was
 * never evaluated.
 */
_Static_assert(ASYNC_UDATA_FD_BITS + ASYNC_UDATA_GEN_BITS == 64,
               "the two fields must span exactly 64 bits");

#define ASYNC_UDATA_FD_SPAN (1ULL << ASYNC_UDATA_FD_BITS)
#define ASYNC_UDATA_GEN_SPAN (1ULL << ASYNC_UDATA_GEN_BITS)

/* Tied to the table's own bound rather than repeating 1024: a table that grows
 * past the low field would otherwise keep passing this while packing aliased
 * two fds onto one word, and SIGIO would reach the wrong slot.
 */
_Static_assert(FD_TABLE_SIZE <= ASYNC_UDATA_FD_SPAN,
               "every guest fd must fit the low field");

/* The fd a packed udata word names.
 *
 * The second ensures pins which field is read, not just how big the answer is.
 * A range alone is satisfied by an accessor that reads the wrong field, and
 * pack's round-trip is stated as arithmetic rather than as calls to these (ACSL
 * cannot call a C function), so nothing else here would catch that. make
 * verify-mutants does: it flips each accessor onto the other field.
 */
/*@
  assigns \nothing;
  ensures 0 <= \result < ASYNC_UDATA_FD_SPAN;
  ensures \result == v % ASYNC_UDATA_FD_SPAN;
 */
static inline int async_udata_fd(uint64_t v)
{
    return (int) (v % ASYNC_UDATA_FD_SPAN);
}

/* The slot generation a packed udata word names. Pinned the same way and for
 * the same reason as async_udata_fd above.
 */
/*@
  assigns \nothing;
  ensures \result < ASYNC_UDATA_GEN_SPAN;
  ensures \result == (v / ASYNC_UDATA_FD_SPAN) % ASYNC_UDATA_GEN_SPAN;
 */
static inline uint64_t async_udata_gen(uint64_t v)
{
    return (v / ASYNC_UDATA_FD_SPAN) % ASYNC_UDATA_GEN_SPAN;
}

/* Pack an fd and the generation of its slot into one word.
 *
 * The two ensures are the ABA guard stated as arithmetic: whatever goes in
 * comes back out, so a delivery that compares the unpacked generation against
 * the slot's own is comparing what was armed, and an fd that survives the round
 * trip cannot name a different slot. They are written as the field expressions
 * rather than as calls to the accessors above, because ACSL cannot call a C
 * function from a specification; the accessors compute exactly these.
 *
 * The generation is reduced rather than required to be small, and the
 * postcondition says so. Requiring it below 2^48 would have been an obligation
 * on a caller that cannot meet it: asyncio.c passes fd_table's generation
 * straight through, and nothing bounds that counter. What the reduction costs
 * is stated rather than hidden: the delivery side reduces the slot's own
 * generation the same way, so a stale event is accepted only if its generation
 * and the current one are congruent modulo 2^48 -- 2^48 reuses of that one slot
 * between arming the knote and the event firing.
 *
 * The fd is a genuine precondition: FD_TABLE_SIZE is 1024, every caller has
 * already range-checked, and a fd that did not fit would name a different slot
 * rather than be caught.
 */
/*@
  requires 0 <= guest_fd < ASYNC_UDATA_FD_SPAN;
  assigns \nothing;
  ensures \result % ASYNC_UDATA_FD_SPAN == (uint64_t) guest_fd;
  ensures (\result / ASYNC_UDATA_FD_SPAN) % ASYNC_UDATA_GEN_SPAN
          == generation % ASYNC_UDATA_GEN_SPAN;
 */
static inline uint64_t async_udata_pack(int guest_fd, uint64_t generation)
{
    return (generation % ASYNC_UDATA_GEN_SPAN) * ASYNC_UDATA_FD_SPAN +
           (uint64_t) guest_fd;
}
