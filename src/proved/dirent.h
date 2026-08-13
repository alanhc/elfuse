/*
 * getdents64 record arithmetic: the parts a proof can reach
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Two directory readers pack Linux dirent64 records into a guest buffer: the
 * host-directory walk in fs.c and the FUSE reply walk in fuse.c. Both size each
 * record from a name length, both pad it to 8, and both must land the record
 * inside a guest-supplied byte count and inside a fixed-size staging buffer on
 * the C stack. Getting either bound wrong overruns a 280-byte stack array with
 * attacker-influenced bytes, so the record arithmetic is discharged as a
 * machine-checked proof rather than reviewed by eye.
 *
 * The two copies had drifted: fuse.c bounds the daemon-supplied name length
 * against NAME_MAX and then re-checks the record against sizeof(entry), while
 * fs.c relied on its NAME_MAX + 1 translation buffer to make the same check
 * unnecessary. One proved function now serves both, and the "fits the staging
 * buffer" half stops being a check at all: it is a postcondition.
 *
 * Split out of fs.c because that file cannot be given to Frama-C: it includes
 * the macOS dirent and stat headers, which the analyzer's libc does not model.
 * This header needs nothing but stdint.h, so make verify-dirent proves it
 * directly.
 */

#pragma once

#include <stdint.h>

/* Bytes before the name in a Linux dirent64: d_ino(8) + d_off(8) + d_reclen(2)
 * + d_type(1). Not sizeof() a struct: the wire record has no padding before the
 * name, while a C struct with those members would be padded to 24 by uint64_t
 * alignment. Both callers therefore memcpy a header struct and then write the
 * name at this offset, so the constant is the layout.
 */
#define DIRENT64_HDR_BYTES 19ULL

/* Linux pads each record so the next one starts 8-byte aligned. */
#define DIRENT64_ALIGN 8ULL

/* Linux NAME_MAX. Named here rather than taken from the host limits.h: this is
 * the guest's limit, and the proof is about what a guest dirent can hold.
 */
#define DIRENT64_NAME_MAX 255ULL

/* Capacity both callers' staging buffers must have. dirent_record_bounds
 * guarantees the record fits it, which is why neither caller checks.
 */
#define DIRENT64_MAX_RECLEN 280ULL

_Static_assert(DIRENT64_MAX_RECLEN == (DIRENT64_HDR_BYTES + DIRENT64_NAME_MAX +
                                       1 + (DIRENT64_ALIGN - 1)) /
                                          DIRENT64_ALIGN * DIRENT64_ALIGN,
               "DIRENT64_MAX_RECLEN must be the padded size of a NAME_MAX "
               "entry, or the staging buffers it sizes are too small");
_Static_assert(DIRENT64_ALIGN == 8ULL,
               "Linux dirent64 records are 8-byte aligned; the proofs adapt "
               "but the guest ABI does not");
_Static_assert(DIRENT64_HDR_BYTES == 8 + 8 + 2 + 1,
               "d_ino + d_off + d_reclen + d_type, unpadded");

/* The padded record size, as a logic term. ACSL cannot call a C function, and
 * dirent_record_bounds' contract has to say which size it computed, so the
 * arithmetic is written once here and both contracts refer to it. Defined
 * rather than axiomatized: a definition unfolds, so nothing here is assumed.
 */
/*@
  logic integer dirent_reclen_of(integer name_len) =
      (DIRENT64_HDR_BYTES + name_len + 1 + (DIRENT64_ALIGN - 1)) -
      (DIRENT64_HDR_BYTES + name_len + 1 + (DIRENT64_ALIGN - 1)) %
          DIRENT64_ALIGN;
 */

/* Size of the record holding a name of name_len bytes, padded to alignment.
 *
 * Written as subtract-the-remainder rather than "(n + 7) & ~7": the compiler
 * emits the same instruction, and the prover reasons about the arithmetic form
 * without first establishing that the mask is one less than a power of two.
 * Same reason src/proved/netlink.h uses "% NETLINK_ALIGNTO".
 *
 * The upper bound on the result is the clause that matters: it is what lets
 * both callers stage a record in a fixed 280-byte array with no bounds check of
 * their own.
 */
/*@
  requires name_len <= DIRENT64_NAME_MAX;
  assigns \nothing;
  ensures \result == dirent_reclen_of(name_len);
  ensures \result % DIRENT64_ALIGN == 0;
  ensures \result >= DIRENT64_HDR_BYTES + name_len + 1;
  ensures \result < DIRENT64_HDR_BYTES + name_len + 1 + DIRENT64_ALIGN;
  ensures \result <= DIRENT64_MAX_RECLEN;
  ensures \result > 0;
 */
static inline uint64_t dirent_reclen(uint64_t name_len)
{
    uint64_t padded = DIRENT64_HDR_BYTES + name_len + 1 + (DIRENT64_ALIGN - 1);
    return padded - padded % DIRENT64_ALIGN;
}

/* Record size and start of the padding for one entry, or 0 when the entry does
 * not fit the remaining guest buffer.
 *
 * pos is where the record would start, count is the buffer the guest passed to
 * getdents64. The subtraction form of the fit test is deliberate: "pos + reclen
 * > count" is the form both callers used, and it is only safe because reclen is
 * small and pos never exceeds count. Stating it as "reclen <= count - pos"
 * under a proved "pos <= count" keeps that reasoning out of the caller.
 *
 * pad_start is an output rather than a caller expression because it is the
 * memset extent: the callers zero [pad_start, reclen), and *pad_start <=
 * *reclen is what makes that length non-negative.
 */
/*@
  requires name_len <= DIRENT64_NAME_MAX;
  requires pos <= count;
  requires \valid(reclen);
  requires \valid(pad_start);
  requires \separated(reclen, pad_start);
  assigns *reclen, *pad_start;
  ensures \result == 0 || \result == 1;
  ensures \result != 0 <==> dirent_reclen_of(name_len) <= count - pos;
  ensures \result != 0 ==> *reclen == dirent_reclen_of(name_len);
  ensures \result != 0 ==> *reclen <= DIRENT64_MAX_RECLEN;
  ensures \result != 0 ==> *reclen > 0;
  ensures \result != 0 ==> *reclen % DIRENT64_ALIGN == 0;
  ensures \result != 0 ==> pos + *reclen <= count;
  ensures \result != 0 ==> *pad_start == DIRENT64_HDR_BYTES + name_len + 1;
  ensures \result != 0 ==> *pad_start <= *reclen;
  ensures \result == 0 ==> *reclen == \old(*reclen);
  ensures \result == 0 ==> *pad_start == \old(*pad_start);
 */
static inline int dirent_record_bounds(uint64_t name_len,
                                       uint64_t pos,
                                       uint64_t count,
                                       uint64_t *reclen,
                                       uint64_t *pad_start)
{
    uint64_t len = dirent_reclen(name_len);

    if (len > count - pos)
        return 0;

    *reclen = len;
    *pad_start = DIRENT64_HDR_BYTES + name_len + 1;
    return 1;
}
