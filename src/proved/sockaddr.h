/*
 * sockaddr conversion length arithmetic: the parts a proof can reach
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux and macOS disagree about sockaddr: Linux has a 2-byte family and no
 * length byte, macOS has a 1-byte length plus a 1-byte family. Every bind,
 * connect, sendto, recvfrom and accept therefore reshapes an address, and the
 * length driving that reshape is guest-supplied on the way in. Both directions
 * subtract the family bytes and clamp against the destination, so those bounds
 * are discharged as a machine-checked proof rather than reviewed by eye.
 *
 * Split out of net-abi.c because that file cannot be given to Frama-C: it and
 * its header include the macOS socket headers, which the analyzer's libc does
 * not model. This header needs nothing but stdint.h, so make verify-sockaddr
 * proves it directly.
 *
 * The proof deliberately names no socket type. Frama-C 31 does define struct
 * sockaddr_storage, but with the POSIX layout and no ss_len field, so proving
 * against it would be proving against the wrong struct. Instead the destination
 * capacity comes in as a plain integer and the callers keep the structs and the
 * memcpy. That is what makes one function serve both directions.
 */

#pragma once

#include <stdint.h>

/* Bytes at the front of either representation that are not payload: Linux
 * sa_family_t, or macOS sa_len plus sa_family. Both are 2.
 */
#define SOCKADDR_FAMILY_BYTES 2ULL

/* The proofs are parameterized over this, so changing it to 1 would still prove
 * while making net-abi.c accept 1-byte sockaddrs and copy from the wrong
 * payload boundary. Pin it: 2 is ABI on both sides, not tuning.
 */
_Static_assert(SOCKADDR_FAMILY_BYTES == 2ULL,
               "Linux sa_family_t and macOS sa_len+sa_family are both 2 bytes");

/* Whether an address buffer is long enough to hold the family bytes at all.
 * Both converters reject below this before touching the payload.
 */
/*@
  assigns \nothing;
  ensures \result == 0 || \result == 1;
  ensures \result != 0 <==> len >= SOCKADDR_FAMILY_BYTES;
 */
static inline int sockaddr_len_ok(uint64_t len)
{
    return len >= SOCKADDR_FAMILY_BYTES;
}

/* Payload bytes to copy: what the source offers, clamped to what the
 * destination can hold, with the family bytes excluded from both.
 *
 * The two subtractions are why this is proved rather than reviewed. Each
 * underflows if its guard is dropped, and an underflowed length here is a
 * memcpy extent, so the result is a host buffer overrun rather than a truncated
 * address.
 *
 * The last clause pins the result to the minimum. Without it an implementation
 * that always returns 0 satisfies every other clause: still safe, but it would
 * silently drop every address payload. Same reason gva_chunk_clamp states its
 * result exactly rather than only bounding it.
 */
/*@
  requires src_len >= SOCKADDR_FAMILY_BYTES;
  requires dst_cap >= SOCKADDR_FAMILY_BYTES;
  assigns \nothing;
  ensures SOCKADDR_FAMILY_BYTES + \result <= src_len;
  ensures SOCKADDR_FAMILY_BYTES + \result <= dst_cap;
  ensures \result == src_len - SOCKADDR_FAMILY_BYTES ||
          \result == dst_cap - SOCKADDR_FAMILY_BYTES;
 */
static inline uint64_t sockaddr_payload_len(uint64_t src_len, uint64_t dst_cap)
{
    uint64_t payload = src_len - SOCKADDR_FAMILY_BYTES;
    uint64_t room = dst_cap - SOCKADDR_FAMILY_BYTES;

    if (payload > room)
        payload = room;
    return payload;
}
