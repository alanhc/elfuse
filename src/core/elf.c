/*
 * ELF64 parser and loader
 *
 * Copyright 2026 elfuse contributors
 * Copyright 2025 Moritz Angermann, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Reads aarch64-linux ELF64 executables, validates the header, extracts PT_LOAD
 * segments, and copies them into guest memory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "core/elf.h"
#include "debug/log.h"
#include "utils.h"

/* Verified parsing core.
 *
 * The five functions below take scalars and a bounded byte buffer, touch no
 * I/O, and carry ACSL contracts discharged by Frama-C WP with -wp-rte (make
 * verify-elf). They are ordered primitive-first.
 *
 * Covered: the offsets and extents feeding the host-side reads and writes in
 * THIS file. The unproved code around them computes no guest-slab offset or
 * extent of its own, so no pread, memcpy, or memset here can leave the slab.
 *
 * NOT covered, and do not assume otherwise:
 * - info->segments[] holds p_vaddr and p_memsz verbatim, never bounds-checked
 *   here. Callers add a load base to them and build the mem_region_t ranges
 *   that decide RX vs RW page-table permissions (bootstrap.c
 *   append_elf_segment_regions, register_elf_segment_regions, and the exec.c
 *   region tables). That arithmetic is unproved.
 * - The pread return-length checks and the interp_path handling are ordinary
 *   reviewed code.
 */

/* a + b, or 0 when the sum does not fit in 64 bits.
 *
 * ELF address fields are unconstrained uint64_t, so the addresses derived from
 * them (segment end, program header GPA) can only be formed with the carry
 * checked. Saturating to UINT64_MAX instead is worse than rejecting: every
 * consumer adds a load base to the result, so the clamp only relocates the wrap
 * into the consumer, where the bound check it defeats reads "elf_end >
 * guest_size" and silently passes.
 */
/*@
  requires \valid(sum);
  assigns *sum;
  ensures \result != 0 ==> *sum == a + b;
 */
static int elf_add_no_wrap(uint64_t a, uint64_t b, uint64_t *sum)
{
    if (a > UINT64_MAX - b)
        return 0;

    *sum = a + b;
    return 1;
}

/* Size of the program header table in bytes, or 0 when the header geometry is
 * unusable. Rejects an empty table, an entry stride too small to hold a program
 * header, and a total past the kernel's 64KiB cap.
 */
/*@
  requires \valid(total);
  assigns *total;
  ensures \result != 0 ==> *total == (size_t) phnum * phentsize;
  ensures \result != 0 ==> phentsize >= sizeof(elf64_phdr_t);
  ensures \result != 0 ==> 0 < *total <= ELF_PHDR_TABLE_MAX;
 */
static int elf_phdr_table_bytes(uint16_t phnum,
                                uint16_t phentsize,
                                size_t *total)
{
    if (phnum == 0 || phentsize < sizeof(elf64_phdr_t))
        return 0;

    size_t bytes = (size_t) phnum * phentsize;
    if (bytes > ELF_PHDR_TABLE_MAX)
        return 0;

    *total = bytes;
    return 1;
}

/* Copy program header idx out of a buffer of buflen bytes.
 *
 * Returns 0 when the entry does not lie wholly inside the buffer.
 *
 * The contract below constrains the copy's SAFETY (the source range lies in the
 * buffer), not its correctness (that *out holds those bytes). Stating the
 * latter was tried and does not work: an ensures over ((char *) out)[k] is
 * discharged vacuously, because WP's memory model does not relate byte-level
 * access through a cast to the struct's typed contents. Verified by mutation:
 * with the byte-equality postcondition in place, replacing this memcpy with
 * memset(out, 0, sizeof(*out)) still proved every obligation. Do not restate it
 * without re-running that mutation.
 */
/*@
  requires \valid_read(buf + (0 .. buflen - 1));
  requires \valid(out);
  requires \separated(out, buf + (0 .. buflen - 1));
  requires phentsize >= sizeof(elf64_phdr_t);
  assigns *out;
  ensures \result != 0 ==>
            (size_t) idx * phentsize + sizeof(elf64_phdr_t) <= buflen;
 */
static int elf_phdr_fetch(const uint8_t *buf,
                          size_t buflen,
                          uint16_t idx,
                          uint16_t phentsize,
                          elf64_phdr_t *out)
{
    size_t off = (size_t) idx * phentsize;
    if (off > buflen || buflen - off < sizeof(*out))
        return 0;

    /* memcpy rather than an aliased struct pointer: e_phentsize is attacker
     * controlled and need not be a multiple of the program header alignment, so
     * buf + off is not guaranteed to be suitably aligned.
     */
    memcpy(out, buf + off, sizeof(*out));
    return 1;
}

/* Guest VA of the program header table, given one PT_LOAD's file range.
 *
 * Linux derives AT_PHDR from the PT_LOAD whose file data contains e_phoff
 * (fs/binfmt_elf.c), not from the lowest mapped address. The two agree for the
 * usual layout where the first PT_LOAD maps from file offset 0, and diverge
 * otherwise.
 *
 * Returns 0 unless the whole table lies inside this segment's file data, which
 * Returns 0 unless the whole table lies inside this segment's file data. Linux
 * only requires e_phoff itself to be inside a PT_LOAD, but Linux keeps the
 * headers reachable through the file mapping; elfuse dropped the separate phdr
 * copy, so a table spanning two PT_LOADs would only stay readable if those
 * segments happened to be adjacent in guest VA, which nothing checks. Requiring
 * one segment to hold it all is what makes the no-copy scheme sound.
 */
/*@
  requires \valid(gpa_out);
  assigns *gpa_out;
  ensures \result != 0 ==> p_offset <= phoff;
  ensures \result != 0 ==> phoff + total <= p_offset + p_filesz;
  ensures \result != 0 ==> *gpa_out == p_vaddr + (phoff - p_offset);
 */
static int elf_phdr_gpa_in_segment(uint64_t phoff,
                                   size_t total,
                                   uint64_t p_offset,
                                   uint64_t p_filesz,
                                   uint64_t p_vaddr,
                                   uint64_t *gpa_out)
{
    if (phoff < p_offset)
        return 0;

    uint64_t rel = phoff - p_offset;
    if (rel > p_filesz || p_filesz - rel < total)
        return 0;

    return elf_add_no_wrap(p_vaddr, rel, gpa_out);
}

/* Place one PT_LOAD segment in the guest slab. On success gpa_out is the
 * relocated load address and zero_len_out is the extent the loader may touch,
 * rounded up to the page boundary after the segment ends. Both outputs are
 * bounded by guest_size, and zero_len_out is never smaller than filesz.
 */
/*@
  requires \valid(gpa_out);
  requires \valid(zero_len_out);
  requires \separated(gpa_out, zero_len_out);
  assigns *gpa_out, *zero_len_out;
  ensures \result != 0 ==> vaddr >= va_base;
  ensures \result != 0 ==> *gpa_out == target_base + (vaddr - va_base);
  ensures \result != 0 ==> *gpa_out + *zero_len_out <= guest_size;
  ensures \result != 0 ==> memsz <= *zero_len_out;
  ensures \result != 0 ==> filesz <= *zero_len_out;
 */
static int elf_segment_extent(uint64_t vaddr,
                              uint64_t va_base,
                              uint64_t target_base,
                              uint64_t filesz,
                              uint64_t memsz,
                              uint64_t guest_size,
                              uint64_t *gpa_out,
                              uint64_t *zero_len_out)
{
    /* Relocation is expressed as a window, not a pre-wrapped base: the segment
     * at vaddr sits at target_base + (vaddr - va_base).
     *
     * The Rosetta translator links at 0x800000000000 and is mapped low, which
     * rosetta.c used to arrange by passing load_base = guest_base - va_base and
     * relying on the truncated sum. That is the same value this computes
     * without any wrapping, so the pair states the intent the wrap only
     * implied, and a guest ELF (va_base 0) can no longer reach an address by
     * overflowing into it.
     */
    if (vaddr < va_base)
        return 0;

    uint64_t gpa;
    if (!elf_add_no_wrap(target_base, vaddr - va_base, &gpa))
        return 0;

    /* A segment cannot contain more initialized file data than its in-memory
     * extent.
     */
    if (filesz > memsz)
        return 0;

    /* Keep the mapped segment inside the configured IPA-sized guest slab. */
    if (memsz > guest_size || gpa > guest_size - memsz)
        return 0;

    /* The loader zeros up to the next page boundary AFTER the segment ends, so
     * the extent is PAGE_ALIGN_UP(gpa + memsz) rather than gpa +
     * PAGE_ALIGN_UP(memsz): gpa is not always page-aligned (e.g. ld.so's RW
     * segment at vaddr 0x2f650), and with the older bytes-from-gpa formula the
     * page covering the last memsz byte kept its mid-page tail untouched.
     * execve into a dynamic-linked target then read stale state from the prior
     * incarnation of the same interpreter at offsets ld.so allocates beyond
     * memsz (e.g. the first link_map in _dl_new_object).
     */
    uint64_t end = gpa + memsz;
    uint64_t aligned = PAGE_ALIGN_UP(end);
    /* aligned < end catches the align-up carrying past UINT64_MAX. */
    if (aligned < end || aligned > guest_size)
        aligned = guest_size;

    *gpa_out = gpa;
    *zero_len_out = aligned - gpa;
    return 1;
}

/* Read a PT_INTERP segment's dynamic linker path into info->interp_path.
 *
 * Returns 0 on success, -1 when the path does not fit. The path lives in the
 * file rather than in a loadable segment, so it is read while the ELF is still
 * open.
 */
static int elf_read_interp(int fd,
                           const elf64_phdr_t *ph,
                           const char *display_path,
                           elf_info_t *info)
{
    size_t len = ph->p_filesz;
    if (len >= sizeof(info->interp_path)) {
        log_error("%s: PT_INTERP path too long (%zu >= %zu)", display_path, len,
                  sizeof(info->interp_path));
        return -1;
    }
    if (len == 0)
        return 0;

    /* len counts the NUL stored in the file. A short read leaves the path
     * unusable, so clear it rather than act on a truncated interpreter name; a
     * full read is force-terminated as insurance.
     */
    if (pread(fd, info->interp_path, len, ph->p_offset) < (ssize_t) len)
        info->interp_path[0] = '\0';
    else
        info->interp_path[len - 1] = '\0';
    return 0;
}

/* Record one PT_LOAD segment and fold its extent into the load bounds.
 *
 * Returns 0 on success, -1 when the segment table is full or the extent wraps.
 * seg_count is the caller's running total, kept out of info until the whole
 * table parses so a rejected ELF leaves no partial segment list behind.
 */
static int elf_record_load(const elf64_phdr_t *ph,
                           const char *display_path,
                           uint16_t idx,
                           int *seg_count,
                           elf_info_t *info)
{
    /* Linux ignores a PT_LOAD that maps no bytes, and so does the mapper.
     * Recording it anyway would still feed load_min/load_max, the boot region
     * tables and /proc/self/maps: a zero-memsz segment at p_vaddr ==
     * guest_size satisfies the extent bound (gpa > guest_size - memsz is false
     * when memsz is 0), so load_max reaches the end of the slab and brk_base
     * follows it there.
     */
    if (ph->p_memsz == 0)
        return 0;

    if (*seg_count >= ELF_MAX_SEGMENTS) {
        log_error("%s: too many PT_LOAD segments", display_path);
        return -1;
    }

    /* A PT_LOAD whose extent wraps past the end of the address space is
     * unloadable in any case, and it must be rejected here rather than
     * saturated: consumers of load_max add a load base before comparing against
     * guest_size (exec.c "ELF extends beyond guest address space"), so a
     * UINT64_MAX sentinel wraps there and passes the very check it should trip.
     */
    uint64_t seg_end;
    if (!elf_add_no_wrap(ph->p_vaddr, ph->p_memsz, &seg_end)) {
        log_error("%s: PT_LOAD %u extent 0x%llx+0x%llx wraps the address space",
                  display_path, idx, (unsigned long long) ph->p_vaddr,
                  (unsigned long long) ph->p_memsz);
        return -1;
    }

    int slot = *seg_count;
    info->segments[slot].gpa = ph->p_vaddr;
    info->segments[slot].offset = ph->p_offset;
    info->segments[slot].filesz = ph->p_filesz;
    info->segments[slot].memsz = ph->p_memsz;
    info->segments[slot].flags = (int) ph->p_flags;
    *seg_count = slot + 1;

    if (ph->p_vaddr < info->load_min)
        info->load_min = ph->p_vaddr;
    if (seg_end > info->load_max)
        info->load_max = seg_end;
    return 0;
}

int elf_load_fd(int fd, const char *display_path, elf_info_t *info)
{
    memset(info, 0, sizeof(*info));

    elf64_ehdr_t ehdr;
    if (pread(fd, &ehdr, sizeof(ehdr), 0) != sizeof(ehdr)) {
        log_error("%s: failed to read ELF header", display_path);
        return -1;
    }

    /* Reject non-ELF inputs before interpreting the rest of the header. */
    if (ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 ||
        ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3) {
        log_error("%s: not an ELF file", display_path);
        return -1;
    }

    /* elfuse only implements the 64-bit Linux ABI. */
    if (ehdr.e_ident[EI_CLASS] != ELFCLASS64) {
        log_error("%s: not a 64-bit ELF", display_path);
        return -1;
    }

    /* aarch64-linux user binaries are little-endian in the supported mode. */
    if (ehdr.e_ident[EI_DATA] != ELFDATA2LSB) {
        log_error("%s: not little-endian", display_path);
        return -1;
    }

    /* x86_64 is recognized so callers can report a clear unsupported-arch
     * diagnostic instead of a generic parse failure.
     */
    if (ehdr.e_machine != EM_AARCH64 && ehdr.e_machine != EM_X86_64) {
        log_error("%s: unsupported architecture (e_machine=%u)", display_path,
                  ehdr.e_machine);
        return -1;
    }

    /* ET_DYN is accepted for PIE executables and interpreters; callers choose
     * the load base that keeps them away from elfuse's reserved regions.
     */
    if (ehdr.e_type != ET_EXEC && ehdr.e_type != ET_DYN) {
        log_error("%s: not an executable (e_type=%u)", display_path,
                  ehdr.e_type);
        return -1;
    }

    info->entry = ehdr.e_entry;
    info->e_type = ehdr.e_type;
    info->e_machine = ehdr.e_machine;
    info->phnum = ehdr.e_phnum;
    info->phentsize = ehdr.e_phentsize;
    info->load_min = UINT64_MAX;
    info->load_max = 0;

    /* Program headers drive both memory mappings and auxv AT_PHDR. The table
     * geometry is rejected before anything is allocated, so a pathological
     * e_phnum/e_phentsize pair cannot drive a large attacker-chosen malloc.
     */
    size_t ph_total;
    if (!elf_phdr_table_bytes(ehdr.e_phnum, ehdr.e_phentsize, &ph_total)) {
        log_error(
            "%s: unusable program header table (e_phnum=%u, "
            "e_phentsize=%u)",
            display_path, ehdr.e_phnum, ehdr.e_phentsize);
        return -1;
    }

    uint8_t *ph_buf = malloc(ph_total);
    if (!ph_buf) {
        perror("malloc");
        return -1;
    }

    if (pread(fd, ph_buf, ph_total, ehdr.e_phoff) != (ssize_t) ph_total) {
        log_error("%s: failed to read program headers", display_path);
        goto fail;
    }

    /* Collect only the program headers that affect process startup. */
    int seg_count = 0;
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        /* Zero-initialized so the phdr scratch is never read uninitialized
         * on a path Pulse cannot follow; three of the suppressed Infer
         * findings were here. One 56-byte clear per program header.
         */
        elf64_phdr_t ph = {0};
        if (!elf_phdr_fetch(ph_buf, ph_total, i, ehdr.e_phentsize, &ph)) {
            log_error("%s: program header %u outside the header table",
                      display_path, i);
            goto fail;
        }

        if (ph.p_type == PT_INTERP &&
            elf_read_interp(fd, &ph, display_path, info) < 0)
            goto fail;

        if (ph.p_type == PT_LOAD &&
            elf_record_load(&ph, display_path, i, &seg_count, info) < 0)
            goto fail;
    }

    info->num_segments = seg_count;

    if (seg_count == 0) {
        log_error("%s: no PT_LOAD segments", display_path);
        goto fail;
    }

    /* AT_PHDR, following Linux 5.19+ (commit 0da1d5002745, which replaced the
     * older load_addr + e_phoff): the program headers are visible to the guest
     * only because some PT_LOAD maps the file range they occupy, so their
     * address comes from that segment. The two formulas agree for the usual
     * layout where the first PT_LOAD starts at file offset 0.
     *
     * phdr_gpa stays 0 when no PT_LOAD covers the table, which is what Linux
     * reports and what a hand-linked static binary with its phdrs outside the
     * loaded range gets. Such a program cannot use AT_PHDR on Linux either, so
     * there is nothing to deliver and the load is not an error. The old code
     * instead memcpy'd the table to load_min + e_phoff, an address inside no
     * segment: for build/test-hello that landed 0x1a bytes past the end of
     * .text.
     *
     * Requiring the whole table inside one segment's file data is what lets
     * elf_map_segments_fd deliver it with no separate copy and no second
     * destination to bound-check.
     */
    /* A segment that covers e_phoff but whose p_vaddr + rel is
     * unrepresentable is skipped rather than distinguished, so in principle
     * the table could be attributed to a later segment. Unreachable: such a
     * p_vaddr sits within a few bytes of UINT64_MAX, and elf_segment_extent
     * rejects it at map time because every accepted segment satisfies
     * gpa <= guest_size - memsz with guest_size at most 1 TiB, so phdr_gpa
     * never reaches build_linux_stack.
     */
    for (int i = 0; i < seg_count; i++) {
        if (elf_phdr_gpa_in_segment(ehdr.e_phoff, ph_total,
                                    info->segments[i].offset,
                                    info->segments[i].filesz,
                                    info->segments[i].gpa, &info->phdr_gpa)) {
            info->phdr_valid = true;
            break;
        }
    }

    free(ph_buf);
    return 0;

fail:
    free(ph_buf);
    return -1;
}

int elf_load(const char *path, elf_info_t *info)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror(path);
        return -1;
    }
    int rc = elf_load_fd(fd, path, info);
    close(fd);
    return rc;
}

int elf_map_segments_fd(const elf_info_t *info,
                        int fd,
                        const char *display_path,
                        void *guest_base,
                        uint64_t guest_size,
                        elf_window_t window,
                        uint64_t infra_lo,
                        uint64_t infra_hi)
{
    /* Half-open intersection test for [a, a+alen) and [b, b+blen). When
     * infra_lo == infra_hi the caller opted out (early bring-up before guest_t
     * is wired up); the host-side writes that follow still get the existing
     * guest_size bound check.
     */
    bool infra_active = infra_lo < infra_hi;

    /* The layout comes entirely from the single parse in elf_load_fd. Do not
     * re-read or re-parse the header here.
     *
     * This function used to, and that was unsound rather than merely wasteful.
     * elf_map_segments re-opens by path and elfuse has no ETXTBSY, so a guest
     * thread can rewrite the image another thread is execve'ing. A second parse
     * then validates bytes that nothing downstream consumes: bootstrap.c and
     * exec.c build the page-table permissions from info->segments[]. A changed
     * PT_LOAD count or order left a segment mapped but never loaded, and the
     * guest read pre-execve bytes at an address /proc/self/maps calls
     * file-backed.
     *
     * The program headers need no separate copy either. elf_load_fd sets
     * phdr_gpa only to an address inside a PT_LOAD whose file data holds the
     * whole table, so the segment read below delivers them at phdr_gpa +
     * load_base as a side effect; when no segment covers the table phdr_gpa is
     * 0 and AT_PHDR reports 0, as on Linux. Dropping that copy removed the last
     * hand-written extent check in this file along with the second destination
     * it guarded.
     */
    for (int i = 0; i < info->num_segments; i++) {
        uint64_t filesz = info->segments[i].filesz;
        uint64_t memsz = info->segments[i].memsz;
        uint64_t gpa, zero_len;

        if (!elf_segment_extent(info->segments[i].gpa, window.va_base,
                                window.target_base, filesz, memsz, guest_size,
                                &gpa, &zero_len)) {
            log_error(
                "%s: segment vaddr 0x%llx (memsz 0x%llx, filesz 0x%llx) is "
                "not loadable: %s [window va_base 0x%llx target 0x%llx, guest "
                "size 0x%llx]",
                display_path, (unsigned long long) info->segments[i].gpa,
                (unsigned long long) memsz, (unsigned long long) filesz,
                info->segments[i].gpa < window.va_base
                    ? "below the relocation window"
                    : (filesz > memsz ? "filesz exceeds memsz"
                                      : "does not fit guest memory"),
                (unsigned long long) window.va_base,
                (unsigned long long) window.target_base,
                (unsigned long long) guest_size);
            return -1;
        }

        /* PT_LOAD with memsz == 0 maps no bytes, but the page-tail zero extent
         * still rounds up to the next page boundary. For an unaligned gpa that
         * means a crafted ELF could splat zeros across the tail of a previously
         * loaded segment in the same page, or trip the infra-overlap check with
         * no live mapping behind it. Linux ignores zero-memsz PT_LOADs; mirror
         * that here.
         */
        if (memsz == 0)
            continue;

        if (infra_active && gpa < infra_hi && gpa + zero_len > infra_lo) {
            log_error(
                "%s: segment at 0x%llx+0x%llx (zero-extent 0x%llx) overlaps "
                "infra reserve [0x%llx, 0x%llx)",
                display_path, (unsigned long long) gpa,
                (unsigned long long) memsz, (unsigned long long) zero_len,
                (unsigned long long) infra_lo, (unsigned long long) infra_hi);
            return -1;
        }

        /* Zero only the tail beyond filesz: the BSS portion [filesz, memsz)
         * plus the page-padding [memsz, zero_len) that Linux guarantees clean
         * for dynamic linkers allocating from the last mapped page's tail.
         * Skipping the file-data range avoids writing zeros that the pread
         * below would immediately overwrite; for typical shared libraries that
         * is a hundreds-of-KiB win per segment.
         */
        if (zero_len > filesz)
            memset((uint8_t *) guest_base + gpa + filesz, 0, zero_len - filesz);

        if (filesz > 0) {
            if (pread(fd, (uint8_t *) guest_base + gpa, filesz,
                      info->segments[i].offset) != (ssize_t) filesz) {
                log_error(
                    "%s: short read for segment at 0x%llx "
                    "(expected %llu)",
                    display_path, (unsigned long long) gpa,
                    (unsigned long long) filesz);
                return -1;
            }
        }
    }

    return 0;
}

int elf_map_segments(const elf_info_t *info,
                     const char *path,
                     void *guest_base,
                     uint64_t guest_size,
                     elf_window_t window,
                     uint64_t infra_lo,
                     uint64_t infra_hi)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror(path);
        return -1;
    }
    int rc = elf_map_segments_fd(info, fd, path, guest_base, guest_size, window,
                                 infra_lo, infra_hi);
    close(fd);
    return rc;
}

void elf_resolve_interp(const char *sysroot,
                        const char *interp_path,
                        char *out,
                        size_t out_sz)
{
    if (sysroot) {
        /* Strategy 1: sysroot + full interp path */
        snprintf(out, out_sz, "%s%s", sysroot, interp_path);
        if (access(out, F_OK) == 0)
            return;

        /* Strategy 2: sysroot/lib/basename. Handles store-style interpreter
         * paths such as /.../lib/ld-musl-aarch64.so.1
         */
        const char *base = strrchr(interp_path, '/');
        base = base ? base + 1 : interp_path;
        snprintf(out, out_sz, "%s/lib/%s", sysroot, base);
        if (access(out, F_OK) == 0)
            return;
    }
    /* Strategy 3: use interp_path as-is */
    str_copy_trunc(out, interp_path, out_sz);
}

int elf_read_shebang_fd(int fd,
                        char *interp_out,
                        size_t interp_sz,
                        char *arg_out,
                        size_t arg_sz)
{
    char buf[512];
    ssize_t nread = pread(fd, buf, sizeof(buf) - 1, 0);

    if (nread < 0) {
        return -errno;
    }
    if (nread < 2 || buf[0] != '#' || buf[1] != '!') {
        return 0; /* Not a shebang script */
    }
    buf[nread] = '\0';

    /* Ignore script bytes after the first line (find \n or \r). If the shebang
     * line is longer than our 511-byte buffer (no EOL found but buffer is
     * full), reject it.
     */
    char *eol = strpbrk(buf + 2, "\r\n");
    if (!eol) {
        if (nread == (ssize_t) (sizeof(buf) - 1)) {
            return -ENOEXEC; /* Shebang line too long */
        }
    } else {
        *eol = '\0';
    }

    char *ptr = buf + 2;
    while (*ptr == ' ' || *ptr == '\t') {
        ptr++;
    }

    /* Strip trailing whitespace/newlines of the whole shebang line */
    size_t len = strlen(ptr);
    while (len > 0 && (ptr[len - 1] == ' ' || ptr[len - 1] == '\t' ||
                       ptr[len - 1] == '\r' || ptr[len - 1] == '\n')) {
        ptr[--len] = '\0';
    }

    if (len == 0) {
        return -ENOEXEC; /* Empty shebang interpreter */
    }

    /* Parse interpreter path and single optional argument */
    char *interp = ptr;
    char *space = strpbrk(ptr, " \t");
    char *arg = NULL;
    if (space) {
        *space = '\0';
        arg = space + 1;
        /* Strip leading space of the argument */
        while (*arg == ' ' || *arg == '\t') {
            arg++;
        }
        /* Strip trailing space/newlines/tabs of the argument */
        size_t arg_len = strlen(arg);
        while (arg_len > 0 &&
               (arg[arg_len - 1] == ' ' || arg[arg_len - 1] == '\t' ||
                arg[arg_len - 1] == '\r' || arg[arg_len - 1] == '\n')) {
            arg[--arg_len] = '\0';
        }
        if (strlen(arg) == 0) {
            arg = NULL;
        }
    }

    if (str_copy_trunc(interp_out, interp, interp_sz) >= interp_sz) {
        return -ENOEXEC; /* Buffer too small */
    }

    if (str_copy_trunc(arg_out, arg ? arg : "", arg_sz) >= arg_sz) {
        return -ENOEXEC; /* Buffer too small */
    }

    return 1; /* Successfully parsed shebang */
}

int elf_read_shebang(const char *host_path,
                     char *interp_out,
                     size_t interp_sz,
                     char *arg_out,
                     size_t arg_sz)
{
    int fd = open(host_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    int rc = elf_read_shebang_fd(fd, interp_out, interp_sz, arg_out, arg_sz);
    close_keep_errno(fd);
    return rc;
}
