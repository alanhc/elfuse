/*
 * iovec accumulation arithmetic: the parts a proof can reach
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * readv, writev, preadv, pwritev, recvmsg and sendmsg all take an iovec array
 * the guest wrote, and every one of them sums iov_len across it. Linux returns
 * EINVAL when that sum exceeds SSIZE_MAX; elfuse has to do the same, because
 * the sum reaches a malloc extent and a memcpy loop on the /proc
 * write-intercept path. A wrapped sum there is a small allocation followed by a
 * large copy.
 *
 * io.c carried four copies of the accumulation with three different guards, and
 * proc_try_writev_intercept had none at all: it summed straight into malloc().
 * That one was unreachable in practice, since host_iov_prepare clamps each
 * entry to the guest mapping it points into, but unreachable by provenance
 * rather than by construction. One proved add now serves every site:
 * urandom_fill_iov, validate_iov_total, proc_try_writev_intercept and
 * process_vm_import_iov.
 *
 * Split into a header because io.c and net-msg.c cannot be given to Frama-C:
 * they include the macOS uio and socket headers, which the analyzer's libc does
 * not model. This header needs nothing but stdint.h, so make verify-iov proves
 * it directly.
 */

#pragma once

#include <stdint.h>

/* Linux UIO_MAXIOV: the cap on iovcnt every one of these syscalls enforces. */
#define IOV_COUNT_MAX 1024LL

/* SSIZE_MAX on LP64, spelled out rather than included: this is the guest's
 * ssize_t, and the value Linux compares the running total against.
 */
#define IOV_TOTAL_MAX 0x7FFFFFFFFFFFFFFFULL

_Static_assert(IOV_TOTAL_MAX == (uint64_t) INT64_MAX,
               "the total cap is the guest's SSIZE_MAX");

/* Whether an iovec count is one Linux would accept.
 *
 * Takes int64_t rather than int so a caller holding a wider count can pass it
 * without narrowing first. Only the readv family uses it today, and with an
 * int. sendmsg and recvmsg keep their own msg_iovlen cap and have to: they
 * accept msg_iovlen == 0, which this check rejects, so it cannot serve them.
 */
/*@
  assigns \nothing;
  ensures \result == 0 || \result == 1;
  ensures \result != 0 <==> (1 <= iovcnt <= IOV_COUNT_MAX);
 */
static inline int iov_count_ok(int64_t iovcnt)
{
    return iovcnt >= 1 && iovcnt <= IOV_COUNT_MAX;
}

/* Add one entry's length to a running total, or 0 when that would carry the
 * total past SSIZE_MAX.
 *
 * The guard is written as "len > IOV_TOTAL_MAX - total" rather than "total +
 * len > IOV_TOTAL_MAX" for the obvious reason: the second form has already
 * overflowed by the time it is tested. That the two are not equivalent is
 * exactly what a reviewer skims past, so it is stated as a postcondition
 * instead.
 */
/*@
  requires total <= IOV_TOTAL_MAX;
  requires \valid(out);
  assigns *out;
  ensures \result == 0 || \result == 1;
  ensures \result != 0 <==> len <= IOV_TOTAL_MAX - total;
  ensures \result != 0 ==> *out == total + len;
  ensures \result != 0 ==> *out <= IOV_TOTAL_MAX;
  ensures \result != 0 ==> *out >= total;
  ensures \result == 0 ==> *out == \old(*out);
 */
static inline int iov_total_add(uint64_t total, uint64_t len, uint64_t *out)
{
    if (len > IOV_TOTAL_MAX - total)
        return 0;

    *out = total + len;
    return 1;
}
