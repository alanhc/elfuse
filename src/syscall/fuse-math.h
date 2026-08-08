/*
 * FUSE frame arithmetic: the parts a proof can reach
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The FUSE daemon is another guest process, so the reply header it writes to
 * /dev/fuse is hostile input. Its len field decides how many bytes the host
 * copies out of the frame buffer, and a slip there hands one guest process a
 * memory-corruption primitive against another (that is not hypothetical: see
 * the fuse_read_common overrun in CHANGELOG). The frame-level bounds are
 * therefore discharged as a machine-checked proof rather than reviewed by eye.
 *
 * Split out of fuse.c because fuse.c cannot be given to Frama-C: it includes
 * pthread and the macOS headers, which the analyzer's libc does not model. This
 * header needs nothing but stdint.h, so make verify-fuse proves it directly.
 *
 * Scope is deliberately the frame level only. Per-opcode payload extents
 * (fuse_entry_out_t, fuse_attr_out_t and friends) would drag ~125 lines of ABI
 * struct definitions out of fuse.c, and the helpers would collapse to
 * "reply_len >= want", which generates almost nothing to prove. Those stay
 * test-covered.
 */

#pragma once

#include <stdint.h>

/* Reply header the daemon prepends to every frame. Kept here rather than in
 * fuse.c so the static assertion below can tie the constant the proof uses to
 * the struct the code actually memcpys.
 */
typedef struct {
    uint32_t len;
    int32_t error;
    uint64_t unique;
} fuse_out_header_t;

#define FUSE_OUT_HDR_BYTES 16ULL

_Static_assert(sizeof(fuse_out_header_t) == FUSE_OUT_HDR_BYTES,
               "FUSE_OUT_HDR_BYTES must match the wire header");

/* Implementation ceiling for a single FUSE frame (header + payload). The kernel
 * FUSE protocol caps a READ or WRITE payload at the negotiated max_pages times
 * the page size (FUSE_MAX_PAGES is the capability flag that enables that
 * negotiation, not a count), about 1 MiB by default and up to 4 MiB on recent
 * kernels. This 8 MiB hard cap leaves headroom for the header, in-band
 * sub-headers, and future readahead growth while still bounding the largest
 * single malloc a daemon can force.
 */
#define FUSE_FRAME_CAP (8ULL * 1024 * 1024)

/* Ceiling on a daemon-negotiated max_write. The 256 bytes of slack cover the
 * request header and sub-header a write frame carries on top of its payload.
 */
#define FUSE_MAX_NEGOTIATED_WRITE (FUSE_FRAME_CAP - 256)

/* Whether a daemon write to /dev/fuse is a plausible frame at all: big enough
 * to contain a header, and within the implementation ceiling.
 */
/*@
  assigns \nothing;
  ensures \result == 0 || \result == 1;
  ensures \result != 0 <==> (FUSE_OUT_HDR_BYTES <= count <= FUSE_FRAME_CAP);
 */
static inline int fuse_frame_count_ok(uint64_t count)
{
    return count >= FUSE_OUT_HDR_BYTES && count <= FUSE_FRAME_CAP;
}

/* Payload extent of a reply frame, or 0 when the header's len field does not
 * describe a frame that fits the write the daemon actually made.
 *
 * The clause that carries the safety argument is FUSE_OUT_HDR_BYTES +
 * *reply_len <= count: the caller copies *reply_len bytes from buf +
 * FUSE_OUT_HDR_BYTES out of a buffer holding exactly count bytes. Dropping
 * either half of the guard breaks it, the lower half by underflowing the
 * subtraction and the upper half by reading past the frame.
 */
/*@
  requires FUSE_OUT_HDR_BYTES <= count <= FUSE_FRAME_CAP;
  requires \valid(reply_len);
  assigns *reply_len;
  ensures \result == 0 || \result == 1;
  ensures \result != 0 <==> (FUSE_OUT_HDR_BYTES <= hdr_len <= count);
  ensures \result != 0 ==> *reply_len == hdr_len - FUSE_OUT_HDR_BYTES;
  ensures \result != 0 ==> FUSE_OUT_HDR_BYTES + *reply_len <= count;
  ensures \result != 0 ==> *reply_len <= FUSE_FRAME_CAP - FUSE_OUT_HDR_BYTES;
 */
static inline int fuse_reply_extent(uint64_t count,
                                    uint64_t hdr_len,
                                    uint64_t *reply_len)
{
    if (hdr_len < FUSE_OUT_HDR_BYTES || hdr_len > count)
        return 0;

    *reply_len = hdr_len - FUSE_OUT_HDR_BYTES;
    return 1;
}

/* Clamp a daemon-advertised max_write to what the frame path will accept.
 *
 * The last postcondition is the point of proving this rather than open-coding
 * the comparison. A read reply is a header plus at most max_write payload
 * bytes, and fuse_dev_write rejects any frame above FUSE_FRAME_CAP, so
 * FUSE_OUT_HDR_BYTES + result <= FUSE_FRAME_CAP is exactly the invariant that
 * keeps a successfully negotiated size from producing replies the write path
 * then refuses.
 */
/*@
  assigns \nothing;
  ensures \result <= FUSE_MAX_NEGOTIATED_WRITE;
  ensures \result == requested || \result == FUSE_MAX_NEGOTIATED_WRITE;
  ensures requested <= FUSE_MAX_NEGOTIATED_WRITE ==> \result == requested;
  ensures FUSE_OUT_HDR_BYTES + \result <= FUSE_FRAME_CAP;
 */
static inline uint64_t fuse_clamp_negotiated_write(uint64_t requested)
{
    if (requested > FUSE_MAX_NEGOTIATED_WRITE)
        return FUSE_MAX_NEGOTIATED_WRITE;
    return requested;
}
