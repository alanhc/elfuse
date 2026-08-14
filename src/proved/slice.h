/*
 * Read-window arithmetic: the parts a proof can reach
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Several reads answer from a buffer elfuse synthesized rather than from a host
 * fd: the /proc/self/oom_* nodes format a value into a stack array and then
 * serve pread and preadv against it, and fallocate's punch-hole fallback writes
 * zeros over the part of a file that exists. All of them take a guest-supplied
 * offset and count and turn them into a memcpy extent, which is the shape that
 * reads past the end of a 32-byte stack array when the EOF test is wrong.
 *
 * Three copies of the clamp existed, each spelled slightly differently, and the
 * scalar and iovec variants in procemu.c had drifted into different loop
 * shapes. One proved function serves all of them.
 *
 * Split into a header because procemu.c and io.c cannot be given to Frama-C:
 * they include the macOS uio and fcntl headers, which the analyzer's libc does
 * not model. This header needs nothing but stdint.h, so make verify-slice
 * proves it directly.
 */

#pragma once

#include <stdint.h>

/* Bytes readable at offset, or 0 when the offset is at or past the end.
 *
 * The return value distinguishes "nothing left" (0, the caller reports EOF)
 * from "here is a window" (1). The subtraction that computes what remains runs
 * only under the proved offset < src_len, which is what keeps it from
 * underflowing into a huge extent.
 *
 * The last two clauses are what stop a degenerate implementation: without the
 * "result is one of the two inputs" clause a function returning 0 bytes forever
 * satisfies every bound, and without the progress clause it could stall a
 * caller that loops until the window is empty.
 */
/*@
  requires \valid(n);
  assigns *n;
  ensures \result == 0 || \result == 1;
  ensures \result != 0 <==> offset < src_len;
  ensures \result != 0 ==> *n <= count;
  ensures \result != 0 ==> offset + *n <= src_len;
  ensures \result != 0 ==> (*n == count || *n == src_len - offset);
  ensures (\result != 0 && count > 0) ==> *n > 0;
  ensures \result == 0 ==> *n == 0;
 */
static inline int slice_clamp(uint64_t src_len,
                              uint64_t offset,
                              uint64_t count,
                              uint64_t *n)
{
    if (offset >= src_len) {
        *n = 0;
        return 0;
    }

    uint64_t avail = src_len - offset;

    *n = count < avail ? count : avail;
    return 1;
}
