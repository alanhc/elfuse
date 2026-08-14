/*
 * Guest address arithmetic: the parts a proof can reach
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Every offset and window computed from a guest address before the host
 * dereferences it. A guest controls the address, the length, and (through
 * mmap/mprotect) much of the page-table content these read, so an arithmetic
 * slip here is a host out-of-bounds access rather than a guest fault.
 *
 * Split out of guest.c because guest.c cannot be given to Frama-C at all: it
 * includes sys/sysctl.h and Hypervisor.framework, which the analyzer's libc
 * does not model. These functions need nothing but stdint.h, so make verify-gva
 * proves this header directly.
 *
 * static inline rather than a separate translation unit on purpose:
 * gva_translate_perm runs on every guest pointer access, and
 * tests/bench-hot-guard.c holds that path to a ceiling.
 *
 * Offsets within a granule use "% granule" rather than "& (granule - 1)". For
 * the power-of-two granules in play the compiler emits the same instruction,
 * and the prover reasons about the arithmetic form without first having to
 * establish the mask is one less than a power of two.
 */

#pragma once

#include <stdint.h>

/* Call-site precondition checks, off by default.
 *
 * guest.c cannot be given to Frama-C, so nothing checks that its call sites
 * honor the requires clauses below; they are reviewed by eye. Building with
 * -DELFUSE_CONTRACT_ASSERT (see make check-contracts) turns the expressible
 * ones into runtime checks so the existing suite exercises them.
 *
 * Only five of the nine requires clauses have a C expression. The four pointer
 * ones do not: assert(p != NULL) is strictly weaker than \valid(p), and
 * assert(a != b) misses overlapping distinct pointers into one object, which is
 * exactly the drift worth catching. Those four stay review-only, which is the
 * ceiling on what this closes.
 *
 * The checks call the *_args_ok predicates below rather than restating the
 * conditions inline. A restated condition can drift weaker than the clause it
 * mirrors with nothing to notice; those predicates carry <==> contracts, so a
 * weakened one fails make verify-gva.
 *
 * verify-gva runs with -DELFUSE_CONTRACT_ASSERT so the prover sees these calls
 * too, and must discharge each assert from the requires clause it mirrors. That
 * extends the guarantee from the predicate bodies to the wiring: a check handed
 * permuted arguments, or pointed at the wrong predicate, fails the proof rather
 * than surfacing later as a spurious abort. It does NOT catch a call passing a
 * constant that satisfies the predicate instead of the real argument, since
 * that is still derivable. tests/test-gva-contracts.c covers that from the
 * other side by checking each conjunct actually rejects.
 *
 * Off by default because the tree has no NDEBUG release split and this sits on
 * the hot guest_read / guest_write path (see the static inline note above).
 */
#ifdef ELFUSE_CONTRACT_ASSERT
#include <stdlib.h>

/* Deliberately not assert(). assert.h re-reads NDEBUG at include time, so
 * routing through it would need NDEBUG undefined here, and that both enables
 * every unrelated assert in the including translation unit and leaves NDEBUG
 * cleared for whatever that unit includes next. Calling abort() directly cannot
 * be switched off by a caller's -DNDEBUG, which is the property this build mode
 * needs, and it fails with SIGABRT specifically, so tests/test-gva-contracts.c
 * can require that exact signal rather than accepting any crash as a rejection.
 */
#define GVA_CONTRACT_ASSERT(cond) ((cond) ? (void) 0 : abort())
#else
#define GVA_CONTRACT_ASSERT(cond) ((void) 0)
#endif

/* Output-address field of a page-table descriptor (bits [47:12]). */
#define GVA_PT_ADDR_MASK 0xFFFFFFFFF000ULL

/* Offset within the primary buffer of the table a descriptor points at, or 0
 * when the descriptor points outside it.
 *
 * A table descriptor must land inside the primary buffer: the walker reads the
 * next level straight out of it. Leaf descriptors are different and go through
 * gva_leaf_target instead, because a leaf may name a GPA in an extra IPA
 * mapping (Rosetta segments, kbuf) that the caller looks up rather than one
 * this function can bound. That lookup is currently a dormant path:
 * guest_add_mapping and guest_overflow_alloc have no callers in the tree, so
 * n_mappings and noverflow stay 0 and every accepted leaf resolves inside the
 * primary buffer. The split is kept because wiring either back up must not
 * require revisiting this bound. A page table is a full 4 KiB of descriptors
 * and the walker indexes all 512 of them, so the whole table must fit, not
 * merely its first byte. "*off < guest_size" would be satisfied by off ==
 * guest_size - 8, which puts l1[511] past the end of the slab.
 *
 * The reject path leaves off alone, stated as a postcondition because
 * "assigns" permits writing it: without that clause a conforming
 * implementation could scribble on it before returning 0, and a caller reading
 * it on the failure path would be relying on the body rather than the
 * contract.
 */
#define GVA_PT_TABLE_BYTES 4096ULL

/*@
  requires \valid(off);
  assigns *off;
  ensures \result == 0 || \result == 1;
  ensures \result != 0 ==> *off + GVA_PT_TABLE_BYTES <= guest_size;
  ensures \result != 0 ==> *off == (desc & GVA_PT_ADDR_MASK) - base;
  ensures \result != 0 <==>
            ((desc & GVA_PT_ADDR_MASK) >= base &&
             (desc & GVA_PT_ADDR_MASK) - base + GVA_PT_TABLE_BYTES
               <= guest_size);
  ensures \result == 0 ==> *off == \old(*off);
 */
static inline int gva_pt_table_offset(uint64_t desc,
                                      uint64_t base,
                                      uint64_t guest_size,
                                      uint64_t *off)
{
    uint64_t ipa = desc & GVA_PT_ADDR_MASK;
    if (ipa < base)
        return 0;

    uint64_t candidate = ipa - base;
    if (guest_size < GVA_PT_TABLE_BYTES ||
        candidate > guest_size - GVA_PT_TABLE_BYTES)
        return 0;

    *off = candidate;
    return 1;
}

/* The expressible half of gva_leaf_target's precondition, as one predicate. The
 * <==> is what makes it usable as a runtime check: an implementation that
 * dropped a conjunct would still be implied by the requires clauses, so a
 * one-directional contract would not catch the weakening, but <==> does.
 */
/*@
  assigns \nothing;
  ensures \result == 0 || \result == 1;
  ensures \result != 0 <==> (0 < granule <= GVA_PT_ADDR_MASK &&
                             ipa <= GVA_PT_ADDR_MASK);
 */
static inline int gva_leaf_target_args_ok(uint64_t granule, uint64_t ipa)
{
    return granule > 0 && granule <= GVA_PT_ADDR_MASK &&
           ipa <= GVA_PT_ADDR_MASK;
}

/* Guest physical address and remaining-bytes-in-granule for a leaf descriptor.
 *
 * chunk is what stops the copy loops from spinning: it is at least 1 for any
 * accepted descriptor, so every iteration makes progress. Takes the
 * descriptor's output address already masked, rather than the descriptor and a
 * mask: the two leaf kinds use different masks (page vs 2MiB block), and
 * keeping the bitwise step outside means the contract never has to reason about
 * "desc & addr_mask" with both operands symbolic, which Z3 does not discharge.
 */
/*@
  requires 0 < granule <= GVA_PT_ADDR_MASK;
  requires ipa <= GVA_PT_ADDR_MASK;
  requires \valid(gpa);
  requires \valid(chunk);
  requires \separated(gpa, chunk);
  assigns *gpa, *chunk;
  ensures \result == 0 || \result == 1;
  ensures \result != 0 <==> ipa >= base;
  ensures \result != 0 ==> 1 <= *chunk <= granule;
  ensures \result != 0 ==> *chunk == granule - gva % granule;
  ensures \result != 0 ==> *gpa == ipa - base + gva % granule;
  ensures \result == 0 ==> *gpa == \old(*gpa);
  ensures \result == 0 ==> *chunk == \old(*chunk);
 */
static inline int gva_leaf_target(uint64_t ipa,
                                  uint64_t base,
                                  uint64_t gva,
                                  uint64_t granule,
                                  uint64_t *gpa,
                                  uint64_t *chunk)
{
    GVA_CONTRACT_ASSERT(gva_leaf_target_args_ok(granule, ipa));

    if (ipa < base)
        return 0;

    uint64_t offset = gva % granule;
    *gpa = (ipa - base) + offset;
    *chunk = granule - offset;
    return 1;
}

/* gva_chunk_clamp's precondition, as one predicate. All three of its clauses
 * are expressible, so unlike gva_leaf_target this covers the whole contract.
 *
 * The parameter list deliberately matches gva_chunk_clamp's exactly, including
 * limit before total. Five same-typed uint64_t parameters give no type-checking
 * against a permuted call, so the two lists agreeing is the only thing making a
 * swap visible by eye.
 */
/*@
  assigns \nothing;
  ensures \result == 0 || \result == 1;
  ensures \result != 0 <==> (chunk >= 1 && gpa < region_end && total < limit);
 */
static inline int gva_chunk_clamp_args_ok(uint64_t chunk,
                                          uint64_t gpa,
                                          uint64_t region_end,
                                          uint64_t limit,
                                          uint64_t total)
{
    return chunk >= 1 && gpa < region_end && total < limit;
}

/* Bytes copyable in one step: the smallest of what the descriptor grants, what
 * remains in the backing region, and what the caller still wants.
 *
 * The preconditions are what the caller must already know, and the
 * postcondition that the result is at least 1 is what makes the surrounding
 * loop terminate.
 *
 * The last clause (below every bound AND equal to one of them) pins the result
 * to the minimum. Without it an implementation that always returns 1 satisfies
 * every other clause: still safe, but it would copy a byte at a time forever.
 */
/*@
  requires chunk >= 1;
  requires gpa < region_end;
  requires total < limit;
  assigns \nothing;
  ensures 1 <= \result <= chunk;
  ensures \result <= region_end - gpa;
  ensures \result <= limit - total;
  ensures \result == chunk || \result == region_end - gpa ||
          \result == limit - total;
 */
static inline uint64_t gva_chunk_clamp(uint64_t chunk,
                                       uint64_t gpa,
                                       uint64_t region_end,
                                       uint64_t limit,
                                       uint64_t total)
{
    GVA_CONTRACT_ASSERT(
        gva_chunk_clamp_args_ok(chunk, gpa, region_end, limit, total));

    if (chunk > region_end - gpa)
        chunk = region_end - gpa;
    if (chunk > limit - total)
        chunk = limit - total;
    return chunk;
}

/* Whether [gva, gva + len) is a non-empty span that does not wrap.
 *
 * Checked before any copy loop starts, so that gva + copied cannot wrap partway
 * through and resolve to an unrelated address.
 */
/*@
  assigns \nothing;
  ensures \result == 0 || \result == 1;
  ensures \result != 0 <==> (len != 0 && gva + len <= 0xFFFFFFFFFFFFFFFF);
 */
static inline int gva_span_ok(uint64_t gva, uint64_t len)
{
    if (len == 0)
        return 0;
    return gva <= UINT64_MAX - len;
}
