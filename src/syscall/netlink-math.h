/*
 * Netlink TLV walk arithmetic: the parts a proof can reach
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Two walks step through netlink messages by a length field. The request parse
 * reads an rtattr chain out of bytes the guest wrote, so its per-entry bounds
 * are discharged as a machine-checked proof rather than reviewed by eye. The
 * reply span walk reads elfuse's own synthesized buffer, and is here for the
 * termination argument rather than for bounds; see below.
 *
 * Split out of netlink.c because that file cannot be given to Frama-C: it
 * includes the macOS network headers, which the analyzer's libc does not model.
 * This header needs nothing but stdint.h, so make verify-netlink proves it
 * directly.
 */

#pragma once

#include <stdint.h>

/* Wire layout of the two headers these walks step over. Kept here rather than
 * in netlink.c so the static assertions below tie the byte constants the proof
 * uses to the structs the code actually memcpys.
 */
typedef struct {
    uint32_t nlmsg_len;
    uint16_t nlmsg_type;
    uint16_t nlmsg_flags;
    uint32_t nlmsg_seq;
    uint32_t nlmsg_pid;
} nlmsghdr_t;

typedef struct {
    uint16_t rta_len;
    uint16_t rta_type;
} rtattr_t;

/* Both chains round each entry up to 4 bytes before the next one starts. */
#define NETLINK_ALIGNTO 4ULL

#define NLMSG_HDRLEN 16ULL
#define RTA_HDRLEN 4ULL

_Static_assert(sizeof(nlmsghdr_t) == NLMSG_HDRLEN,
               "NLMSG_HDRLEN must match the wire header");
_Static_assert(sizeof(rtattr_t) == RTA_HDRLEN,
               "RTA_HDRLEN must match the wire header");
_Static_assert(
    NETLINK_ALIGNTO == 4ULL,
    "netlink rounds to 4 bytes; the proofs adapt but the wire does not");
_Static_assert(NLMSG_HDRLEN % NETLINK_ALIGNTO == 0,
               "an aligned header keeps the walk offsets aligned");

/* Ceiling on a length field, so the align-up below stays far from overflow.
 * Both length fields are read from a message header: nlmsg_len is 32 bits and
 * rta_len is 16, and both are widened to uint64_t here.
 */
#define NETLINK_LEN_MAX 0xFFFFFFFFULL

/* Round a length up to the netlink alignment.
 *
 * Written as subtract-the-remainder rather than "(len + 3) & ~3": the compiler
 * emits the same instruction, and the prover reasons about the arithmetic form
 * without first establishing that the mask is one less than a power of two.
 * Same reason src/core/gva-math.h uses "% granule".
 */
/*@
  requires len <= NETLINK_LEN_MAX;
  assigns \nothing;
  ensures \result >= len;
  ensures \result < len + NETLINK_ALIGNTO;
  ensures \result % NETLINK_ALIGNTO == 0;
 */
static inline uint64_t netlink_align_up(uint64_t len)
{
    uint64_t padded = len + (NETLINK_ALIGNTO - 1);
    return padded - padded % NETLINK_ALIGNTO;
}

/* Payload extent and next offset for one rtattr, or 0 when the entry's length
 * field does not describe an entry that fits.
 *
 * The guard has two halves and both carry weight. Without the lower one,
 * *data_len underflows; without the upper one, the payload runs past the
 * message. The exact-advance clause pins the alignment: without it, dropping
 * the align-up satisfies every other clause while walking to a misaligned next
 * header, which mis-parses the rest of the chain.
 */
/*@
  requires total <= NETLINK_LEN_MAX;
  requires off + RTA_HDRLEN <= total;
  requires rta_len <= NETLINK_LEN_MAX;
  requires \valid(data_len);
  requires \valid(next_off);
  requires \separated(data_len, next_off);
  assigns *data_len, *next_off;
  ensures \result == 0 || \result == 1;
  ensures \result != 0 <==> (RTA_HDRLEN <= rta_len <= total - off);
  ensures \result != 0 ==> *data_len == rta_len - RTA_HDRLEN;
  ensures \result != 0 ==> off + RTA_HDRLEN + *data_len <= total;
  ensures \result != 0 ==> *next_off > off;
  ensures \result != 0 ==>
            *next_off == off + (rta_len + (NETLINK_ALIGNTO - 1))
                             - (rta_len + (NETLINK_ALIGNTO - 1))
                                 % NETLINK_ALIGNTO;
  ensures \result == 0 ==> *data_len == \old(*data_len);
  ensures \result == 0 ==> *next_off == \old(*next_off);
 */
static inline int netlink_rta_bounds(uint64_t off,
                                     uint64_t total,
                                     uint64_t rta_len,
                                     uint64_t *data_len,
                                     uint64_t *next_off)
{
    if (rta_len < RTA_HDRLEN || rta_len > total - off)
        return 0;

    *data_len = rta_len - RTA_HDRLEN;
    *next_off = off + netlink_align_up(rta_len);
    return 1;
}

/* Bytes one netlink message occupies, including its alignment padding.
 *
 * The reason this is proved rather than inlined is termination. netlink.c
 * rounded hdr->nlmsg_len up in uint32 arithmetic, which wraps to 0 for
 * nlmsg_len >= 0xFFFFFFFD, so the caller's cursor would advance by nothing and
 * spin forever. That was unreachable, because the buffer it walks is filled
 * only by elfuse's own message synthesis and never by guest bytes, but it was
 * unreachable by provenance rather than by construction. Widening to uint64_t
 * removes the wrap, and the postcondition that the span is strictly positive is
 * what makes the caller's loop terminate for any header at all.
 */
/*@
  requires nlmsg_len <= NETLINK_LEN_MAX;
  requires \valid(span);
  assigns *span;
  ensures \result == 0 || \result == 1;
  ensures \result != 0 <==> nlmsg_len >= NLMSG_HDRLEN;
  ensures \result != 0 ==> *span >= nlmsg_len;
  ensures \result != 0 ==> *span > 0;
  ensures \result != 0 ==> *span % NETLINK_ALIGNTO == 0;
  ensures \result != 0 ==>
            *span == (nlmsg_len + (NETLINK_ALIGNTO - 1))
                   - (nlmsg_len + (NETLINK_ALIGNTO - 1)) % NETLINK_ALIGNTO;
  ensures \result == 0 ==> *span == \old(*span);
 */
static inline int netlink_msg_span(uint64_t nlmsg_len, uint64_t *span)
{
    if (nlmsg_len < NLMSG_HDRLEN)
        return 0;

    *span = netlink_align_up(nlmsg_len);
    return 1;
}
