/*
 * Control-message walk arithmetic: the parts a proof can reach
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * sendmsg walks the guest's msg_control buffer as a hand-rolled TLV loop. Each
 * entry's length is eight bytes read straight out of guest memory, and it
 * drives both the payload extent the host copies and the advance to the next
 * entry. A slip there reads past the control buffer, so the per-entry bounds
 * are discharged as a machine-checked proof rather than reviewed by eye.
 *
 * Split out of net-msg.c because net-msg.c cannot be given to Frama-C: it
 * includes the macOS socket headers, which the analyzer's libc does not model.
 * This header needs nothing but stdint.h, so make verify-cmsg proves it
 * directly.
 *
 * The host CMSG_SPACE / CMSG_LEN / CMSG_FIRSTHDR / CMSG_NXTHDR macros are
 * deliberately not modeled: they describe the macOS layout, and a stub for them
 * would be an unchecked model of the very thing in question. The caller keeps
 * them and passes only the Linux-side numbers in.
 */

#pragma once

#include <stdint.h>

/* Linux cmsghdr wire format: 8-byte cmsg_len, then 4-byte level and 4-byte
 * type. Fixed by the Linux ABI, so no host struct is involved.
 */
#define CMSG_LINUX_HDR_BYTES 16ULL

/* Linux rounds each control message up to an 8-byte boundary before the next
 * one starts (CMSG_ALIGN with sizeof(long) == 8 on aarch64).
 */
#define CMSG_LINUX_ALIGN 8ULL

/* Ceiling sys_sendmsg enforces before the walk starts, rejecting anything
 * larger with EINVAL. Restating it as a precondition keeps the align-up below
 * far from overflow without the prover needing a symbolic bound on ctl_len.
 */
#define CMSG_LINUX_CTL_MAX 65536ULL

/* Payload extent and next-entry offset for one control message, or 0 when the
 * length field does not describe an entry that fits.
 *
 * Takes the length already read from the buffer rather than the buffer itself:
 * the caller does the unaligned 8-byte load, so the contract never has to model
 * the guest buffer's contents.
 *
 * Two postconditions do the independent work, each with a mutation that breaks
 * it and nothing else:
 *   - the payload bound is what the caller's memcpy from
 *     pos + CMSG_LINUX_HDR_BYTES rests on (drop either guard);
 *   - the exact-advance clause pins the alignment. Without it, dropping the
 *     align-up (*next_pos = pos + cmsg_len) or under-advancing
 *     (*next_pos = pos + CMSG_LINUX_HDR_BYTES) satisfies every other clause:
 *     still memory-safe, but it walks to a misaligned or re-read header and
 *     mis-parses the rest of the buffer. Same reason gva_chunk_clamp states its
 *     result exactly rather than only bounding it.
 *
 * The two remaining next_pos clauses, *next_pos > pos and the overshoot bound,
 * are corollaries of the exact-advance clause: removing either still proves.
 * They are kept because they state, in the form the caller actually reasons
 * about, that its loop terminates and that its own bound check still rejects
 * the next position. Removing one drops the goal count below MIN_GOALS, so the
 * gate still notices if they go missing.
 *
 * The reject path leaves both outputs alone, stated as a postcondition because
 * "assigns" permits writing them: without it a conforming implementation could
 * scribble on them before returning 0, and a caller reading them on the failure
 * path would be relying on the body rather than the contract.
 */
/*@
  requires ctl_len <= CMSG_LINUX_CTL_MAX;
  requires pos + CMSG_LINUX_HDR_BYTES <= ctl_len;
  requires \valid(data_len);
  requires \valid(next_pos);
  requires \separated(data_len, next_pos);
  assigns *data_len, *next_pos;
  ensures \result == 0 || \result == 1;
  ensures \result != 0 <==>
            (CMSG_LINUX_HDR_BYTES <= cmsg_len <= ctl_len - pos);
  ensures \result != 0 ==> *data_len == cmsg_len - CMSG_LINUX_HDR_BYTES;
  ensures \result != 0 ==>
            pos + CMSG_LINUX_HDR_BYTES + *data_len <= ctl_len;
  ensures \result != 0 ==> *next_pos > pos;
  ensures \result != 0 ==> *next_pos <= ctl_len + (CMSG_LINUX_ALIGN - 1);
  ensures \result != 0 ==>
            *next_pos == pos + (cmsg_len + (CMSG_LINUX_ALIGN - 1))
                             - (cmsg_len + (CMSG_LINUX_ALIGN - 1))
                                 % CMSG_LINUX_ALIGN;
  ensures \result == 0 ==> *data_len == \old(*data_len);
  ensures \result == 0 ==> *next_pos == \old(*next_pos);
 */
static inline int cmsg_entry_bounds(uint64_t pos,
                                    uint64_t ctl_len,
                                    uint64_t cmsg_len,
                                    uint64_t *data_len,
                                    uint64_t *next_pos)
{
    if (cmsg_len < CMSG_LINUX_HDR_BYTES)
        return 0;
    if (cmsg_len > ctl_len - pos)
        return 0;

    *data_len = cmsg_len - CMSG_LINUX_HDR_BYTES;

    /* Align the advance up, written as subtract-the-remainder rather than the
     * usual "(len + 7) & ~7": the compiler emits the same instruction, but the
     * prover would first have to establish that the mask is one less than a
     * power of two, and leaves the two next_pos bounds open when it cannot.
     * Same reason src/proved/gva.h uses "% granule".
     */
    uint64_t advance = cmsg_len + (CMSG_LINUX_ALIGN - 1);
    advance -= advance % CMSG_LINUX_ALIGN;
    *next_pos = pos + advance;
    return 1;
}
