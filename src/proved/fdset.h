/*
 * fd bitmap word arithmetic: the parts a proof can reach
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * pselect6 reads three guest bitmasks into fixed-size stack arrays and then
 * walks them a 64-bit word at a time. Two things there are worth proving rather
 * than reviewing.
 *
 * The first is the read extent. The arrays are sized from FD_TABLE_SIZE while
 * the nfds argument was bounded by the host's FD_SETSIZE, two constants that
 * happen to both be 1024 on macOS but are not the same constant. Nothing tied
 * them together, so a host whose FD_SETSIZE was larger would have made
 * guest_read_small write past three stack arrays with guest bytes.
 *
 * Bounding the extent is not the same as matching Linux on what nfds is legal.
 * elfuse rejects nfds above the table with EINVAL where core_sys_select clamps
 * and proceeds; that divergence predates this header and is unchanged by it. A
 * naive clamp would be wrong here, because the result writeback copies only
 * nfds_words * 8 bytes and would leave the guest's upper fd_set words unzeroed,
 * which Linux does clear.
 *
 * The second is the tail of the last word. nfds need not be a multiple of 64,
 * and the walk iterated whole words, so bits above nfds in the final word were
 * honored: a guest that set bit 100 with nfds=70 got that fd polled, and got
 * EBADF if it was not open, where Linux ignores it (fs/select.c bounds its
 * per-word iteration by n). fdset_fd_index answers that per set bit. Masking
 * each word once on the way in would read better at the call site, and was the
 * first draft, but its postcondition is a symbolic shift that no prover here
 * discharges; see the note on fdset_fd_index below. Nothing hands back a
 * pre-masked word, so every consumer of these bitmasks has to filter its bits
 * through fdset_fd_index.
 *
 * Several bitmaps in the tree run 64 bits to a word over the same fd table:
 * pselect6's three guest bitmasks in poll.c, the free-fd allocator's
 * fd_free_bitmap in fdtable.c, and the urandom bitmap in shim-globals.c. They
 * share the constants here, but not every one of them should route its
 * indexing through the helpers below.
 *
 * The bound belongs here when it is a fact about the input: pselect6's nfds
 * arrives from the guest, so the reject branch is a branch the code has to take
 * anyway. It does not belong here when the caller has already established the
 * bound, because then the reject branch is unreachable, and an unreachable
 * branch is invisible to a reader while being load-bearing to an analyzer. That
 * is not hypothetical: guarding fdtable.c's one-line bitmap setters this way
 * left the fd_table[fd] write beside them unguarded, and Infer's Pulse then
 * concluded a socket fd escaped nowhere and reported a leak in net.c, two
 * modules from the change. Those setters index directly again.
 *
 * Split into a header because poll.c cannot be given to Frama-C: it includes
 * the macOS poll and select headers, which the analyzer's libc does not model.
 * This header needs nothing but stdint.h, so make verify-fdset proves it
 * directly.
 */

#pragma once

#include <stdint.h>

#define FDSET_BITS_PER_WORD 64ULL

/* The largest nfds accepted, and the array sizing it implies. poll.c static
 * asserts FDSET_MAX_FDS against FD_TABLE_SIZE, which is what stops the two from
 * drifting apart again.
 */
#define FDSET_MAX_FDS 1024LL
#define FDSET_MAX_WORDS 16ULL
#define FDSET_MAX_BYTES 128ULL

_Static_assert(FDSET_MAX_WORDS ==
                   (uint64_t) FDSET_MAX_FDS / FDSET_BITS_PER_WORD,
               "the word count must cover exactly FDSET_MAX_FDS bits");
_Static_assert(FDSET_MAX_BYTES == FDSET_MAX_WORDS * 8,
               "the byte count is what guest_read_small copies");

/* The word and bit holding one fd, or 0 when the fd is outside the table.
 *
 * Use this where the rejection is a real case rather than a restatement of what
 * the caller already knows. Its one caller qualifies: fd_bitmap_find_free takes
 * minfd from fcntl(F_DUPFD), which forwards the guest's argument having
 * rejected only negatives. *word < FDSET_MAX_WORDS is then a postcondition, so
 * the bitmap access that follows needs no bound of its own.
 */
/*@
  requires \valid(word);
  requires \valid(bit);
  requires \separated(word, bit);
  assigns *word, *bit;
  ensures binary: \result == 0 || \result == 1;
  ensures exact: \result != 0 <==> (0 <= fd < FDSET_MAX_FDS);
  ensures word_in_table: \result != 0 ==> *word < FDSET_MAX_WORDS;
  ensures bit_in_word: \result != 0 ==> *bit < FDSET_BITS_PER_WORD;
  ensures splits_fd:
            \result != 0 ==> *word * FDSET_BITS_PER_WORD + *bit == fd;
  ensures untouched_on_reject: \result == 0 ==> *word == \old(*word);
  ensures bit_untouched_on_reject: \result == 0 ==> *bit == \old(*bit);
 */
static inline int fdset_slot(int64_t fd, uint64_t *word, uint64_t *bit)
{
    if (fd < 0 || fd >= FDSET_MAX_FDS)
        return 0;

    *word = (uint64_t) fd / FDSET_BITS_PER_WORD;
    *bit = (uint64_t) fd % FDSET_BITS_PER_WORD;
    return 1;
}

/* Words spanning nfds bits, or 0 when nfds is out of range.
 *
 * The result bounds the read extent, so the caller needs no size check of its
 * own: *words <= FDSET_MAX_WORDS is a postcondition, not a review note.
 */
/*@
  requires \valid(words);
  assigns *words;
  ensures \result == 0 || \result == 1;
  ensures \result != 0 <==> (0 <= nfds <= FDSET_MAX_FDS);
  ensures \result != 0 ==> *words <= FDSET_MAX_WORDS;
  ensures \result != 0 ==> *words * 8 <= FDSET_MAX_BYTES;
  ensures \result != 0 ==> *words * FDSET_BITS_PER_WORD >= nfds;
  ensures \result != 0 ==> *words * FDSET_BITS_PER_WORD <
                             nfds + FDSET_BITS_PER_WORD;
  ensures \result != 0 ==> (*words == 0 <==> nfds == 0);
  ensures \result == 0 ==> *words == \old(*words);
 */
static inline int fdset_words(int64_t nfds, uint64_t *words)
{
    if (nfds < 0 || nfds > FDSET_MAX_FDS)
        return 0;

    *words =
        ((uint64_t) nfds + (FDSET_BITS_PER_WORD - 1)) / FDSET_BITS_PER_WORD;
    return 1;
}

/* The fd a set bit names, or 0 when that bit sits above nfds.
 *
 * Called once per set bit, so the "walk whole words but honor only the bits
 * below nfds" rule lives in one place instead of in the loop's index
 * arithmetic. Returning the index rather than a yes/no is what makes the fd
 * bound a postcondition: *fd < nfds <= FDSET_MAX_FDS is what the caller needs
 * before it indexes the fd table with it.
 *
 * An earlier draft returned a per-word mask of valid bits instead, which reads
 * better at the call site but states its postcondition as "the low (nfds % 64)
 * bits are set". That is a symbolic shift, and both provers time out on it at
 * 60s. The property that matters here is the fd bound, and this form proves it
 * in milliseconds; a spec no prover discharges is not a spec.
 */
/*@
  requires 0 <= nfds <= FDSET_MAX_FDS;
  requires word < FDSET_MAX_WORDS;
  requires bit_index < FDSET_BITS_PER_WORD;
  requires \valid(fd);
  assigns *fd;
  ensures \result == 0 || \result == 1;
  ensures \result != 0 <==> word * FDSET_BITS_PER_WORD + bit_index < nfds;
  ensures \result != 0 ==> *fd == word * FDSET_BITS_PER_WORD + bit_index;
  ensures \result != 0 ==> *fd < nfds;
  ensures \result != 0 ==> *fd < FDSET_MAX_FDS;
  ensures \result == 0 ==> *fd == \old(*fd);
 */
static inline int fdset_fd_index(int64_t nfds,
                                 uint64_t word,
                                 uint64_t bit_index,
                                 uint64_t *fd)
{
    uint64_t index = word * FDSET_BITS_PER_WORD + bit_index;

    if (index >= (uint64_t) nfds)
        return 0;

    *fd = index;
    return 1;
}
