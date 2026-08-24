/*
 * Shared utility helpers
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Small inline helpers and macros used across multiple modules. Keeps leaf
 * utilities in one place so they do not accumulate as single-function headers.
 *
 * This header also carries the growable array and the string builder built on
 * it. They are header-only so nothing has to link an object for them, and they
 * live here rather than beside their one caller because the tree keeps leaf
 * utilities together. Two consequences to know before adding to them: every
 * translation unit that includes this file parses all of it, and mk/verify.mk
 * lists this header in VERIFY_ELF_SCAN and VERIFY_RSP_SCAN, so an ACSL contract
 * added here becomes mandatory for both of those proof targets.
 */

#pragma once

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* Generic growable array of trivially-copyable elements. Capacity is measured
 * in element slots; count is the number of logical elements. The allocation is
 * one contiguous block of capacity * element_size bytes, and both the count
 * addition and the multiplication are checked before a growth. Arithmetic
 * overflow reports EOVERFLOW, invalid arguments EINVAL, allocation failure
 * ENOMEM. Growth is transactional: on any failure the old pointer, count, and
 * capacity remain valid.
 */
typedef struct dynamic_array {
    void *data;
    size_t count;
    size_t capacity;
    size_t element_size;
} dynamic_array_t;

#define DYNAMIC_ARRAY_INITIAL_CAPACITY ((size_t) 8)

/* Set errno for a bad argument and return a standard failure code. */
static inline int dynamic_array_invalid(void)
{
    errno = EINVAL;
    return -1;
}

/* Check that the array handle is non-null and has a non-zero element size. */
static inline int dynamic_array_validate(const dynamic_array_t *array)
{
    if (!array || array->element_size == 0)
        return dynamic_array_invalid();
    return 0;
}

/* Return count + extra after guarding against size_t overflow. */
static inline int dynamic_array_count_plus(const dynamic_array_t *array,
                                           size_t extra,
                                           size_t *total)
{
    if (extra > SIZE_MAX - array->count) {
        errno = EOVERFLOW;
        return -1;
    }
    *total = array->count + extra;
    return 0;
}

/* Compute the byte size for a number of elements with overflow protection. */
static inline int dynamic_array_bytes(const dynamic_array_t *array,
                                      size_t count,
                                      size_t *bytes)
{
    if (count != 0 && array->element_size > SIZE_MAX / count) {
        errno = EOVERFLOW;
        return -1;
    }
    *bytes = count * array->element_size;
    return 0;
}

/* Return the offset of an in-array source span, including capacity bytes. An
 * append whose source aliases the array's own storage has to re-derive the
 * pointer after a realloc moves the block.
 */
static inline bool dynamic_array_source_offset(const dynamic_array_t *array,
                                               const void *source,
                                               size_t bytes,
                                               size_t *offset)
{
    if (!array->data || !source)
        return false;

    uintptr_t base = (uintptr_t) array->data;
    uintptr_t address = (uintptr_t) source;
    if (address < base)
        return false;
    uintptr_t delta = address - base;
    if (delta > (uintptr_t) SIZE_MAX)
        return false;
    size_t start = (size_t) delta;
    size_t allocation_bytes;
    if (dynamic_array_bytes(array, array->capacity, &allocation_bytes) < 0)
        return false;
    if (start > allocation_bytes || bytes > allocation_bytes - start)
        return false;
    *offset = start;
    return true;
}

/* Initialize and reserve storage for an initial number of elements.
 *
 * This is a fresh-initialization operation and deliberately does not inspect
 * prior object contents, so an automatic, uninitialized object is safe. In
 * particular, do not free an indeterminate pointer from one; destroy an
 * existing array before reinitializing it. A zero capacity allocates nothing
 * and just establishes the element size.
 */
static inline int dynamic_array_init_with_capacity(dynamic_array_t *array,
                                                   size_t element_size,
                                                   size_t initial_capacity)
{
    if (!array || element_size == 0)
        return dynamic_array_invalid();

    /* Establish a safe zero state before any fallible allocation. This is a
     * fresh initializer, so an existing allocation must have been destroyed by
     * the caller rather than silently leaked here.
     */
    *array = (dynamic_array_t) {0};

    if (initial_capacity != 0 && element_size > SIZE_MAX / initial_capacity) {
        errno = EOVERFLOW;
        return -1;
    }
    size_t bytes = initial_capacity * element_size;
    void *storage = NULL;
    if (bytes != 0) {
        storage = malloc(bytes);
        if (!storage) {
            errno = ENOMEM;
            return -1;
        }
    }

    *array = (dynamic_array_t) {
        .data = storage,
        .capacity = initial_capacity,
        .element_size = element_size,
    };
    return 0;
}

/* Release backing storage and reset the array object to zero state. */
static inline void dynamic_array_destroy(dynamic_array_t *array)
{
    if (!array)
        return;
    free(array->data);
    *array = (dynamic_array_t) {0};
}

/* Ensure the array has capacity for at least extra additional elements. */
static inline int dynamic_array_reserve(dynamic_array_t *array, size_t extra)
{
    if (dynamic_array_validate(array) < 0)
        return -1;

    size_t needed;
    if (dynamic_array_count_plus(array, extra, &needed) < 0)
        return -1;
    if (needed <= array->capacity)
        return 0;

    size_t new_capacity = array->capacity;
    if (new_capacity == 0)
        new_capacity = DYNAMIC_ARRAY_INITIAL_CAPACITY;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }

    size_t bytes;
    if (dynamic_array_bytes(array, new_capacity, &bytes) < 0)
        return -1;
    void *grown = realloc(array->data, bytes);
    if (!grown && bytes != 0) {
        errno = ENOMEM;
        return -1;
    }
    array->data = grown;
    array->capacity = new_capacity;
    return 0;
}

/* Resize logical length; zero-initialize any newly visible elements. */
static inline int dynamic_array_resize(dynamic_array_t *array, size_t count)
{
    if (dynamic_array_validate(array) < 0)
        return -1;
    if (count > array->capacity) {
        size_t extra = count - array->count;
        if (dynamic_array_reserve(array, extra) < 0)
            return -1;
    }
    if (count > array->count) {
        size_t old_bytes, new_bytes;
        if (dynamic_array_bytes(array, array->count, &old_bytes) < 0)
            return -1;
        if (dynamic_array_bytes(array, count, &new_bytes) < 0)
            return -1;
        memset((unsigned char *) array->data + old_bytes, 0,
               new_bytes - old_bytes);
    }
    array->count = count;
    return 0;
}

/* Append count elements from source memory to the end of the array. */
static inline int dynamic_array_append_n(dynamic_array_t *array,
                                         const void *data,
                                         size_t count)
{
    if (dynamic_array_validate(array) < 0)
        return -1;
    if (count == 0)
        return 0;
    if (!data)
        return dynamic_array_invalid();

    size_t total;
    if (dynamic_array_count_plus(array, count, &total) < 0)
        return -1;
    size_t bytes;
    if (dynamic_array_bytes(array, count, &bytes) < 0)
        return -1;
    size_t offset = 0;
    bool aliases = dynamic_array_source_offset(array, data, bytes, &offset);

    if (dynamic_array_reserve(array, count) < 0)
        return -1;
    if (aliases)
        data = (const unsigned char *) array->data + offset;
    size_t old_bytes;
    if (dynamic_array_bytes(array, array->count, &old_bytes) < 0)
        return -1;
    memmove((unsigned char *) array->data + old_bytes, data, bytes);
    array->count = total;
    return 0;
}

/* Return a mutable pointer to the index-th element, or NULL when invalid. */
static inline void *dynamic_array_at(dynamic_array_t *array, size_t index)
{
    if (!array || array->element_size == 0 || index >= array->count) {
        errno = EINVAL;
        return NULL;
    }
    return (unsigned char *) array->data + index * array->element_size;
}

/* Return a read-only pointer to the index-th element, or NULL when invalid. */
static inline const void *dynamic_array_at_const(const dynamic_array_t *array,
                                                 size_t index)
{
    if (!array || array->element_size == 0 || index >= array->count) {
        errno = EINVAL;
        return NULL;
    }
    return (const unsigned char *) array->data + index * array->element_size;
}

/* Establish the element size on the first typed operation. A failed first touch
 * leaves it set, which nothing can observe: the array is still empty, so every
 * accessor answers the same either way and the next call re-establishes the
 * identical size.
 */
static inline void dynamic_array_type(dynamic_array_t *array,
                                      size_t element_size)
{
    if (array && !array->element_size)
        array->element_size = element_size;
}

#if defined(__GNUC__) || defined(__clang__)
#define DYNAMIC_ARRAY_INLINE static inline __attribute__((unused))
#else
#define DYNAMIC_ARRAY_INLINE static inline
#endif

/* Generate a small type-safe facade over the raw container. The facade owns no
 * additional state; all growth and copying stays in the raw operations above. A
 * typed object is zero-initialized by its declaration and establishes its
 * element size on the first operation, so it needs no explicit init call.
 */
#define DYNAMIC_ARRAY_DEFINE(name, type)                                      \
    typedef struct name {                                                     \
        dynamic_array_t raw;                                                  \
    } name##_t;                                                               \
                                                                              \
    /* Destroy the typed array and release backing storage. */                \
    DYNAMIC_ARRAY_INLINE void name##_destroy(name##_t *array)                 \
    {                                                                         \
        if (array)                                                            \
            dynamic_array_destroy(&array->raw);                               \
    }                                                                         \
    /* Reserve extra slots in the typed array. */                             \
    DYNAMIC_ARRAY_INLINE int name##_reserve(name##_t *array, size_t extra)    \
    {                                                                         \
        dynamic_array_t *raw = array ? &array->raw : NULL;                    \
        dynamic_array_type(raw, sizeof(type));                                \
        return dynamic_array_reserve(raw, extra);                             \
    }                                                                         \
    /* Resize typed array, zero-filling newly visible elements. */            \
    DYNAMIC_ARRAY_INLINE int name##_resize(name##_t *array, size_t count)     \
    {                                                                         \
        dynamic_array_t *raw = array ? &array->raw : NULL;                    \
        dynamic_array_type(raw, sizeof(type));                                \
        return dynamic_array_resize(raw, count);                              \
    }                                                                         \
    /* Append one value through a typed pointer. */                           \
    DYNAMIC_ARRAY_INLINE int name##_append_ptr(name##_t *array,               \
                                               const type *value)             \
    {                                                                         \
        dynamic_array_t *raw = array ? &array->raw : NULL;                    \
        dynamic_array_type(raw, sizeof(type));                                \
        return dynamic_array_append_n(raw, value, 1);                         \
    }                                                                         \
    /* Append one typed value by value. */                                    \
    DYNAMIC_ARRAY_INLINE int name##_append_value(name##_t *array, type value) \
    {                                                                         \
        return name##_append_ptr(array, &value);                              \
    }                                                                         \
    /* Return a typed pointer to the element at index. */                     \
    DYNAMIC_ARRAY_INLINE type *name##_at(name##_t *array, size_t index)       \
    {                                                                         \
        return (type *) dynamic_array_at(array ? &array->raw : NULL, index);  \
    }                                                                         \
    /* Return a typed const pointer to the element at index. */               \
    DYNAMIC_ARRAY_INLINE const type *name##_at_const(const name##_t *array,   \
                                                     size_t index)            \
    {                                                                         \
        return (const type *) dynamic_array_at_const(                         \
            array ? &array->raw : NULL, index);                               \
    }                                                                         \
    /* Access the underlying typed data pointer. */                           \
    DYNAMIC_ARRAY_INLINE type *name##_data(name##_t *array)                   \
    {                                                                         \
        return array ? (type *) array->raw.data : NULL;                       \
    }                                                                         \
    /* Query current number of elements in the typed array. */                \
    DYNAMIC_ARRAY_INLINE size_t name##_count(const name##_t *array)           \
    {                                                                         \
        return array ? array->raw.count : 0;                                  \
    }

/* Align x up to the next multiple of a; a must be a power of two. Both x and a
 * are evaluated as uint64_t. Most callers manipulate guest addresses or sizes
 * that already fit, so this avoids surprises from signed/unsigned mixing in
 * alignment masks.
 */
#define ALIGN_UP(x, a) \
    (((uint64_t) (x) + ((uint64_t) (a) - 1)) & ~((uint64_t) (a) - 1))

/* Align x down to the previous multiple of a; a must be a power of two. */
#define ALIGN_DOWN(x, a) ((uint64_t) (x) & ~((uint64_t) (a) - 1))

/* The Linux ABI fixes the page size at 4KiB on aarch64 regardless of the host
 * page size, so this is shared by every guest memory path (mmap, brk, mprotect,
 * ELF loading).
 */
#define GUEST_PAGE_SIZE 4096ULL
#define PAGE_ALIGN_UP(x) ALIGN_UP(x, GUEST_PAGE_SIZE)

/* 2MiB block alignment shared by region setup, page table walking, and stack
 * placement. BLOCK_2MIB itself is defined in core/guest.h.
 */
#define ALIGN_2MIB_DOWN(x) ALIGN_DOWN(x, 2ULL * 1024 * 1024)
#define ALIGN_2MIB_UP(x) ALIGN_UP(x, 2ULL * 1024 * 1024)

/* Branchless range check: true when minx <= x < minx + size.
 *
 * Replaces the recurring pair (x >= minx && x < minx + size) with a single
 * unsigned compare: shift x into a [0, size) window and let unsigned wraparound
 * flag both underflow (x < minx) and overflow (x >= minx + size). Width-safe
 * for any operand up to uint64_t.
 *
 * Operands are cast to uint64_t *before* the subtraction so signed inputs near
 * the type extremes (e.g., LONG_MIN passed by a strtol result) cannot trigger
 * signed overflow UB. Negative signed values sign-extend through the unsigned
 * conversion to a large uint64_t, which still yields the correct out-of-range
 * answer.
 *
 * Caveat: x and minx are evaluated twice; do not pass expressions with side
 * effects.
 */
#define RANGE_CHECK(x, minx, size) \
    (((uint64_t) (x) - (uint64_t) (minx)) < (uint64_t) (size))

/* Number of elements in a fixed-size array. */
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* Growable, NUL-terminated C-string builder over the generic array. The array
 * counts payload bytes only, so every path that grows the builder reserves one
 * byte beyond the payload for the terminator written at storage.count.
 */
typedef struct string_builder {
    dynamic_array_t storage;
} string_builder_t;

/* Set EILSEQ for invalid input and report failure. */
static inline int string_builder_invalid(void)
{
    errno = EILSEQ;
    return -1;
}

/* Initialize a builder, reserving initial_capacity bytes including the NUL. A
 * zero capacity leaves storage unallocated for lazy growth, so
 * string_builder_data_const stays NULL until the first append. This is a fresh
 * initializer, safe on an uninitialized automatic object; destroy an existing
 * builder before reinitializing it.
 */
static inline int string_builder_init(string_builder_t *builder,
                                      size_t initial_capacity)
{
    if (!builder)
        return string_builder_invalid();
    if (dynamic_array_init_with_capacity(&builder->storage, sizeof(char),
                                         initial_capacity) < 0)
        return -1;
    if (initial_capacity)
        ((char *) builder->storage.data)[0] = '\0';
    return 0;
}

/* Release storage and reset the logical length. Safe to call with NULL. */
static inline void string_builder_destroy(string_builder_t *builder)
{
    if (builder)
        dynamic_array_destroy(&builder->storage);
}

/* Return read-only builder data, or NULL when builder is NULL or unallocated.
 */
static inline const char *string_builder_data_const(
    const string_builder_t *builder)
{
    return builder ? builder->storage.data : NULL;
}

/* Return the number of data bytes currently stored, excluding the NUL. */
static inline size_t string_builder_length(const string_builder_t *builder)
{
    return builder ? builder->storage.count : 0;
}

/* Convert a formatting failure into the documented errno values. */
static inline void string_builder_format_failure(void)
{
    if (errno != EOVERFLOW && errno != EILSEQ)
        errno = EILSEQ;
}

/* Append len bytes verbatim, keeping the builder NUL-terminated. Bytes are
 * copied as-is, embedded NULs included; a caller that wants C-string truncation
 * passes the prefix length itself. This is the cheap path for text a caller
 * already has in hand: the maps writer in procemu.c formats a line into a stack
 * buffer and hands the bytes straight over, rather than paying a second round
 * of formatting just to copy them.
 */
static inline int string_builder_append_n(string_builder_t *builder,
                                          const char *data,
                                          size_t len)
{
    if (!builder || (!data && len))
        return string_builder_invalid();
    /* First touch establishes the element size; sizeof(char) cannot fail. */
    if (!builder->storage.element_size)
        builder->storage.element_size = sizeof(char);
    if (!len)
        return 0;
    if (len == SIZE_MAX) {
        errno = EOVERFLOW;
        return -1;
    }

    /* Resolve an aliasing source before reserving. dynamic_array_append_n does
     * this for its own reserve, but the extra byte reserved here can move the
     * block first, which would leave data dangling and defeat the alias check
     * inside it.
     */
    size_t offset = 0;
    bool aliases =
        dynamic_array_source_offset(&builder->storage, data, len, &offset);

    /* Reserve the payload plus the trailing NUL in one step. The generic array
     * only ever sizes itself for the payload, so reserving len alone leaves the
     * terminator store below one byte past the allocation whenever the payload
     * lands exactly on a capacity boundary.
     */
    if (dynamic_array_reserve(&builder->storage, len + 1) < 0)
        return -1;
    if (aliases)
        data = (const char *) builder->storage.data + offset;
    if (dynamic_array_append_n(&builder->storage, data, len) < 0)
        return -1;
    ((char *) builder->storage.data)[builder->storage.count] = '\0';
    return 0;
}

/* Append formatted text using printf-style arguments, keeping the builder
 * NUL-terminated. A formatted NUL byte terminates the appended C-string prefix.
 *
 * Format into separate storage before touching the builder. Besides avoiding
 * writes through an aliased format string, this keeps %s arguments that point
 * into the builder valid even when appending the result grows the allocation.
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
static inline int string_builder_appendf(string_builder_t *builder,
                                         const char *format,
                                         ...)
{
    if (!builder || !format)
        return string_builder_invalid();
    /* First touch establishes the element size; sizeof(char) cannot fail. */
    if (!builder->storage.element_size)
        builder->storage.element_size = sizeof(char);

    int saved_errno = errno;
    char *formatted = NULL;
    va_list ap;

    /* Keep the caller's errno visible to printf extensions such as %m, on both
     * passes, so the two cannot disagree about the length.
     */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"

    /* A non-NULL destination keeps static analyzers from treating the
     * standards-sanctioned n == 0 sizing call as a null dereference. The
     * destination is never written when its size is zero. Infer fails the build
     * without it, which is what this comment is here to prevent someone
     * rediscovering.
     */
    char sizing_sink;
    va_start(ap, format);
    int formatted_len = vsnprintf(&sizing_sink, 0, format, ap);
    va_end(ap);
    if (formatted_len < 0) {
        string_builder_format_failure();
        goto out;
    }

    formatted = malloc((size_t) formatted_len + 1);
    if (!formatted) {
        errno = ENOMEM;
        goto out;
    }

    errno = saved_errno;
    va_start(ap, format);
    int rendered_len =
        vsnprintf(formatted, (size_t) formatted_len + 1, format, ap);
    va_end(ap);
#pragma clang diagnostic pop
    if (rendered_len < 0) {
        string_builder_format_failure();
        goto out;
    }

    /* Preserve the builder's C-string semantics for formatted NUL bytes: a
     * formatted NUL ends the appended prefix, so an all-NUL render appends
     * nothing and leaves an untouched builder untouched. A formatted NUL ends
     * the appended prefix, so an all-NUL render appends nothing and leaves an
     * untouched builder untouched.
     */
    if (string_builder_append_n(builder, formatted, strlen(formatted)) < 0)
        goto out;

    /* free before the restore: free is allowed to set errno. */
    free(formatted);
    errno = saved_errno;
    return 0;

out:
    free(formatted);
    return -1;
}

#ifndef MIN
#define MIN(a, b)               \
    ({                          \
        __typeof__(a) _a = (a); \
        __typeof__(b) _b = (b); \
        _a < _b ? _a : _b;      \
    })
#endif

#ifndef MAX
#define MAX(a, b)               \
    ({                          \
        __typeof__(a) _a = (a); \
        __typeof__(b) _b = (b); \
        _a > _b ? _a : _b;      \
    })
#endif

/* Copy src into dst, truncating to dst_size-1 and always NUL-terminating.
 * Returns strlen(src) so the caller can detect truncation (ret >= dst_size).
 */
static inline size_t str_copy_trunc(char *dst, const char *src, size_t dst_size)
{
    size_t src_len = strlen(src);

    if (dst_size > 0) {
        size_t copy_len = src_len < dst_size ? src_len : dst_size - 1;
        memcpy(dst, src, copy_len);
        dst[copy_len] = '\0';
    }

    return src_len;
}

/* Free @n owned strings and the array holding them. Every slot must be a heap
 * copy, never a borrowed environ or argv pointer; a NULL @v is a no-op. The
 * count is a parameter rather than a NULL terminator because the guest argv is
 * counted rather than terminated.
 */
static inline void strv_free(const char **v, int n)
{
    if (!v)
        return;
    for (int i = 0; i < n; i++)
        free((void *) v[i]);
    free((void *) v);
}

/* close(2) on a cleanup path: preserves errno across the close so the caller's
 * failure errno survives untouched. Skips the close when fd < 0.
 */
static inline void close_keep_errno(int fd)
{
    int saved = errno;
    if (fd >= 0)
        (void) close(fd);
    errno = saved;
}

/* Encode @len bytes of @src as lowercase hex into @dst, writing len*2 hex
 * characters followed by a terminating NUL. @dst must hold at least len*2+1
 * bytes.
 *
 * Returns the number of hex characters written (len*2).
 */
static inline size_t bytes_to_hex(char *dst, const uint8_t *src, size_t len)
{
    static const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        dst[i * 2] = hex_chars[(src[i] >> 4) & 0xf];
        dst[i * 2 + 1] = hex_chars[src[i] & 0xf];
    }
    dst[len * 2] = '\0';
    return len * 2;
}

/* Decode a single hex digit to its 0-15 value, or -1 if @c is not a hex digit.
 * The inverse building block of bytes_to_hex; accepts either case. The bounds
 * matter to callers that shift the result: "hex_nibble(c) << 4" is undefined
 * behavior when c is not a hex digit, so the range and the digit-or-not
 * equivalence are both stated for Frama-C (make verify-rsp).
 */
/*@ logic integer hex_val(integer c) =
      ('0' <= c <= '9')   ? c - '0' :
      ('a' <= c <= 'f')   ? c - 'a' + 10 :
      ('A' <= c <= 'F')   ? c - 'A' + 10 : -1;
 */
/*@
  assigns \nothing;
  ensures -1 <= \result <= 15;
  ensures \result >= 0 <==> (('0' <= c <= '9') || ('a' <= c <= 'f') ||
                             ('A' <= c <= 'F'));
  ensures \result == hex_val(c);
 */
static inline int hex_nibble(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Write exactly @len bytes to a blocking @fd, resuming across short writes and
 * EINTR.
 *
 * Returns 0 once every byte is written, or -1 with errno set on error. An
 * unexpected zero-byte return is treated as EIO rather than spun on, since the
 * offset would otherwise never advance. A zero-length request returns 0.
 */
static inline int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, p + sent, len - sent);
        if (n > 0) {
            sent += (size_t) n;
            continue;
        }
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        errno = EIO;
        return -1;
    }
    return 0;
}

/* Read exactly @len bytes from a blocking @fd into @buf, resuming across short
 * reads and EINTR. On error returns -1 with errno set. On a premature EOF the
 * result depends on @eof_is_error: when true the call returns -1, when false it
 * returns the count of bytes read before EOF so the caller can detect a clean
 * end of stream. Otherwise returns @len.
 */
static inline ssize_t read_all(int fd, void *buf, size_t len, bool eof_is_error)
{
    uint8_t *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n > 0) {
            got += (size_t) n;
            continue;
        }
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (eof_is_error)
            return -1;
        break;
    }
    return (ssize_t) got;
}

/* Create a private per-user scratch directory. mkdir(path, 0700), then tolerate
 * an already-existing entry only if it is a real directory, owned by the
 * current uid, with no group/other access bits. That rejects a symlink, a
 * foreign-owned directory, or a stale world-writable one that a local user
 * could use to interpose on the contents. The group/other check matters because
 * an existing 0777 dir owned by this uid would otherwise pass and let other
 * local users drop files into it.
 *
 * Returns 0 on success, -1 with errno set (EACCES when the ownership, type, or
 * permission check fails). Centralized so every caller applies the same guard;
 * drift here is a security bug.
 */
static inline int create_private_dir(const char *path)
{
    if (mkdir(path, 0700) < 0 && errno != EEXIST)
        return -1;

    struct stat st;
    if (lstat(path, &st) < 0)
        return -1;
    if (!S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

/* Enable an fd flag if it is not already set.
 *
 * Returns 0 on success or -1 with errno preserved from the failing fcntl call.
 */
static inline int fd_set_fd_flag(int fd, int flag)
{
    int flags = fcntl(fd, F_GETFD);

    if (flags < 0)
        return -1;
    if ((flags & flag) != 0)
        return 0;
    return fcntl(fd, F_SETFD, flags | flag);
}

/* Set or clear a file status flag such as O_NONBLOCK. */
static inline int fd_update_status_flag(int fd, int flag, bool enabled)
{
    int flags = fcntl(fd, F_GETFL);

    if (flags < 0)
        return -1;
    if (enabled)
        flags |= flag;
    else
        flags &= ~flag;
    return fcntl(fd, F_SETFL, flags);
}

static inline int fd_set_cloexec(int fd)
{
    return fd_set_fd_flag(fd, FD_CLOEXEC);
}

static inline int fd_set_nonblock(int fd)
{
    return fd_update_status_flag(fd, O_NONBLOCK, true);
}

/* Create a temp file that exists only as long as the returned fd: mkstemp under
 * /tmp with an elfuse-<what>- prefix, unlinked before it is handed back.
 *
 * The unlink is the point. Four places wanted this shape and each spelled out
 * the create-then-unlink pair, which is a file left on disk the first time
 * somebody adds an early return between the two. Callers that keep the name
 * (the Rosetta AOT cache staging its output for a rename, and the FUSE exec
 * materializer, which unlinks after the exec) genuinely differ and stay as they
 * are; a flag to suppress the unlink here would just move their decision
 * somewhere it reads as an afterthought.
 *
 * Returns the fd, or -1 with errno set. The path is never reported because
 * nothing can reach it: that is what makes it anonymous.
 */
static inline int tmpfile_anon(const char *what)
{
    char path[64];
    int n = snprintf(path, sizeof(path), "/tmp/elfuse-%s-XXXXXX", what);
    if (n < 0 || (size_t) n >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    int fd = mkstemp(path);
    if (fd < 0)
        return -1;

    /* The name has to go, or the fd is not anonymous and the caller has no way
     * to remove a path it never sees. Retry EINTR, and fail the call rather
     * than hand back a descriptor with a name still attached.
     */
    int rc;
    do {
        rc = unlink(path);
    } while (rc < 0 && errno == EINTR);

    /* Any failure other than ENOENT leaves a named file behind that the caller
     * cannot see to remove, so the call fails rather than hand back a
     * descriptor that is not anonymous. Retrying would not help: the error is a
     * property of the path or the directory, not a transient of this call, and
     * EINTR is already handled above.
     */
    if (rc < 0 && errno != ENOENT) {
        int unlink_errno = errno;
        close(fd);
        errno = unlink_errno;
        return -1;
    }

    /* Neither answer the unlink can give is proof the file is anonymous, so ask
     * the descriptor. A success is not proof: another process with this uid can
     * hard-link the entry between mkstemp and here, and then the unlink removes
     * the name it was given while the descriptor stays linked under the other
     * one. An ENOENT is not proof either: the same race with rename leaves the
     * unlink missing entirely. The link count is the fact that matters in both,
     * so it is checked in both.
     */
    struct stat anon;
    if (fstat(fd, &anon) != 0) {
        /* Its own errno, not EEXIST. Folding the two together reported a
         * spurious EEXIST out of memfd_create for anything fstat could fail
         * with.
         */
        int fstat_errno = errno;
        close(fd);
        errno = fstat_errno;
        return -1;
    }
    if (anon.st_nlink != 0) {
        close(fd);
        errno = EEXIST;
        return -1;
    }
    return fd;
}

/* Carry overflow/underflow between tv_nsec and tv_sec so the result is a
 * canonical timespec with 0 <= tv_nsec < 1e9. Uses div/mod (which truncate
 * toward zero in C99) plus a single borrow so the LONG_MIN case never negates
 * tv_nsec -- that would be undefined behavior.
 *
 * NSEC_PER_SEC is also defined by mach/clock_types.h and dispatch/time.h on
 * macOS; the guard avoids redefinition warnings when those system headers are
 * pulled in transitively.
 */
#ifndef NSEC_PER_SEC
#define NSEC_PER_SEC 1000000000L
#endif

static inline void timespec_normalize(struct timespec *ts)
{
    ts->tv_sec += ts->tv_nsec / (long) NSEC_PER_SEC;
    ts->tv_nsec %= (long) NSEC_PER_SEC;
    if (ts->tv_nsec < 0) {
        ts->tv_sec -= 1;
        ts->tv_nsec += (long) NSEC_PER_SEC;
    }
}

/* Compute a CLOCK_REALTIME absolute deadline that is rel_ms milliseconds in the
 * future, suitable for pthread_cond_timedwait().
 */
static inline void timespec_deadline_in_ms(struct timespec *out, long rel_ms)
{
    clock_gettime(CLOCK_REALTIME, out);
    out->tv_sec += rel_ms / 1000;
    out->tv_nsec += (rel_ms % 1000) * 1000000L;
    timespec_normalize(out);
}

/* Bitmap helpers.
 *
 * Operate on a single uint64_t word. For multi-word bitmaps, callers index the
 * word and pass the bit position within it. Centralizing the shift and
 * compiler-intrinsic calls here keeps the meaning ("the bit for slot N",
 * "lowest set bit") visible at the call site instead of leaving readers to
 * decode 1ULL << (n) and __builtin_ctzll.
 */

/* The bit value for position n (0..63). n is evaluated once. */
#define BIT64(n) (1ULL << (n))

/* Mask of the low n bits. n may be 0..64. */
static inline uint64_t bit_mask64_low(unsigned int n)
{
    return n >= 64 ? UINT64_MAX : (BIT64(n) - 1);
}

/* Position of the lowest set bit. word must be non-zero -- __builtin_ctzll is
 * undefined on zero. Range: 0..63.
 */
static inline int bit_ctz64(uint64_t word)
{
    return __builtin_ctzll(word);
}

/* Number of set bits in word. */
static inline int bit_popcount64(uint64_t word)
{
    return __builtin_popcountll(word);
}

/* 64-bit FNV-1a over @len bytes. Not cryptographic: collision resistance is the
 * birthday bound on 64 bits, which suits stable identifiers derived from names
 * (synthetic inode numbers, derived filenames), not adversarial input.
 * Constants from the FNV reference (offset basis, prime).
 */
static inline uint64_t fnv1a64(const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *) data;
    uint64_t h = 1469598103934665603ULL;

    while (len--) {
        h ^= (uint64_t) *p++;
        h *= 1099511628211ULL;
    }
    return h;
}

/* Compiler attribute wrappers.
 *
 * PACKED removes inter-field padding, used for Linux ABI structures whose
 * layout must match the kernel exactly (e.g., linux_dirent64). Apply at the end
 * of a struct definition: } PACKED name_t;.
 */
#define PACKED __attribute__((packed))
