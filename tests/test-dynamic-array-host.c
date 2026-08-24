/*
 * Native-host unit tests for the generic dynamic array.
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * White-box: the container is header-only and these are its own unit tests, so
 * they read the raw count, capacity, and data members directly.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host-test-util.h"
#include "utils.h"

typedef struct odd {
    unsigned char tag;
    uint32_t value;
} odd_t;

DYNAMIC_ARRAY_DEFINE(int_array, int)
DYNAMIC_ARRAY_DEFINE(odd_array, odd_t)

/* A typed array carries no init call, so first touch is what establishes the
 * element size. Check that a plain {0} object reaches a usable state through
 * each of the three operations that can be the first one.
 */
static void test_lazy_first_touch(void)
{
    const char *lane = "lazy first touch";

    int_array_t appended = {0};
    host_check(int_array_append_value(&appended, 7) == 0, lane,
               "append must establish the element size");
    host_check(int_array_count(&appended) == 1, lane, "count after append");
    host_check(*int_array_at(&appended, 0) == 7, lane, "value after append");
    int_array_destroy(&appended);

    int_array_t reserved = {0};
    host_check(int_array_reserve(&reserved, 16) == 0, lane,
               "reserve must establish the element size");
    host_check(int_array_count(&reserved) == 0, lane,
               "reserve must not change the count");
    host_check(int_array_append_value(&reserved, 3) == 0, lane,
               "append after reserve");
    host_check(*int_array_at(&reserved, 0) == 3, lane,
               "value after reserve then append");
    int_array_destroy(&reserved);

    int_array_t resized = {0};
    host_check(int_array_resize(&resized, 4) == 0, lane,
               "resize must establish the element size");
    host_check(int_array_count(&resized) == 4, lane, "count after resize");
    bool zeroed = true;
    for (size_t i = 0; i < 4; i++)
        zeroed &= *int_array_at(&resized, i) == 0;
    host_check(zeroed, lane, "resize must zero-fill what it exposes");
    int_array_destroy(&resized);
}

/* Growth past the initial capacity must move the block without losing or
 * reordering what is already in it.
 */
static void test_growth_preserves_content(void)
{
    enum { COUNT = 4096 };
    const char *lane = "growth preserves content";
    int_array_t array = {0};
    bool appends_ok = true;

    for (int i = 0; i < COUNT; i++)
        appends_ok &= int_array_append_value(&array, i * 3) == 0;
    host_check(appends_ok, lane, "every append across the growth must succeed");
    host_check(int_array_count(&array) == COUNT, lane,
               "the count must be the number of appends");
    bool ordered = true;
    for (int i = 0; i < COUNT; i++)
        ordered &= *int_array_at(&array, i) == i * 3;
    host_check(ordered, lane, "growth must preserve order and values");
    host_check(int_array_data(&array)[COUNT - 1] == (COUNT - 1) * 3, lane,
               "the raw data pointer must agree with the accessor");
    int_array_destroy(&array);
}

/* An element wider than one byte and not a power of two catches a stride
 * computed as anything other than count * element_size.
 */
static void test_odd_element_stride(void)
{
    const char *lane = "odd element stride";
    odd_array_t array = {0};
    bool appends_ok = true;

    for (unsigned i = 0; i < 64; i++) {
        odd_t entry = {.tag = (unsigned char) i, .value = i * 7u};
        appends_ok &= odd_array_append_value(&array, entry) == 0;
    }
    host_check(appends_ok, lane, "every append must succeed");
    host_check(odd_array_count(&array) == 64, lane, "count after appends");
    bool intact = true;
    for (unsigned i = 0; i < 64; i++) {
        const odd_t *entry = odd_array_at_const(&array, i);
        intact &= entry->tag == (unsigned char) i && entry->value == i * 7u;
    }
    host_check(intact, lane, "every element must round-trip at its own stride");
    odd_array_destroy(&array);
}

/* An append whose source points into the array's own storage has to survive the
 * realloc that the same append triggers.
 */
static void test_self_aliasing_append(void)
{
    const char *lane = "self-aliasing append";
    dynamic_array_t array = {0};
    host_check(dynamic_array_init_with_capacity(&array, sizeof(int), 0) == 0,
               lane, "init");

    int seed[4] = {1, 2, 3, 4};
    host_check(dynamic_array_append_n(&array, seed, 4) == 0, lane, "seed");

    /* Append the array onto itself repeatedly, so at least one round has to
     * grow the block while reading from it.
     */
    bool doubled_ok = true, mirrored = true;
    for (int round = 0; round < 6; round++) {
        size_t half = array.count;
        doubled_ok &= dynamic_array_append_n(&array, array.data, half) == 0;
        doubled_ok &= array.count == half * 2;
        const int *data = array.data;
        for (size_t i = 0; i < half; i++)
            mirrored &= data[i] == data[half + i];
    }
    host_check(doubled_ok, lane, "every self-append must succeed");
    host_check(mirrored, lane,
               "a self-append must copy the pre-growth contents");
    dynamic_array_destroy(&array);
}

/* Every failure has to leave the array exactly as it was. */
static void test_failures_preserve_state(void)
{
    const char *lane = "failures preserve state";
    dynamic_array_t array = {0};
    host_check(dynamic_array_init_with_capacity(&array, sizeof(int), 0) == 0,
               lane, "init");
    int seed[3] = {10, 20, 30};
    host_check(dynamic_array_append_n(&array, seed, 3) == 0, lane, "seed");

    void *old_data = array.data;
    size_t old_count = array.count, old_capacity = array.capacity;

    /* count + extra overflows size_t. */
    errno = 0;
    host_check(dynamic_array_reserve(&array, SIZE_MAX) == -1, lane,
               "a count overflow must be refused");
    host_check(errno == EOVERFLOW, lane, "a count overflow reports EOVERFLOW");

    /* count * element_size overflows size_t. */
    errno = 0;
    host_check(dynamic_array_reserve(&array, SIZE_MAX / 2) == -1, lane,
               "a byte-size overflow must be refused");
    host_check(errno == EOVERFLOW, lane,
               "a byte-size overflow reports EOVERFLOW");

    errno = 0;
    host_check(dynamic_array_append_n(&array, NULL, 1) == -1, lane,
               "a NULL source must be refused");
    host_check(errno == EINVAL, lane, "a NULL source reports EINVAL");

    host_check(array.data == old_data && array.count == old_count &&
                   array.capacity == old_capacity,
               lane, "a refused call must not disturb the array");
    const int *data = array.data;
    host_check(data[0] == 10 && data[1] == 20 && data[2] == 30, lane,
               "a refused call must not disturb the payload");

    /* A zero-count append is a no-op, not a failure. */
    host_check(dynamic_array_append_n(&array, seed, 0) == 0, lane,
               "a zero-count append must succeed");
    host_check(array.count == old_count, lane,
               "a zero-count append must not change the count");

    dynamic_array_destroy(&array);
    host_check(!array.data && array.count == 0 && array.capacity == 0, lane,
               "destroy must restore the zero state");
}

/* An out-of-range index reports EINVAL rather than handing back a pointer. */
static void test_bounds(void)
{
    const char *lane = "bounds";
    int_array_t array = {0};
    host_check(int_array_append_value(&array, 42) == 0, lane, "seed");

    host_check(int_array_at(&array, 0) != NULL, lane,
               "an in-range index must resolve");
    errno = 0;
    host_check(!int_array_at(&array, 1), lane,
               "an out-of-range index must not resolve");
    host_check(errno == EINVAL, lane, "an out-of-range index reports EINVAL");
    errno = 0;
    host_check(!int_array_at_const(&array, 1), lane,
               "the const accessor must agree");
    host_check(errno == EINVAL, lane, "the const accessor reports EINVAL");
    int_array_destroy(&array);

    /* An untyped array answers EINVAL rather than dividing by a zero stride. */
    dynamic_array_t raw = {0};
    errno = 0;
    host_check(!dynamic_array_at(&raw, 0), lane,
               "an untyped array must not resolve an index");
    host_check(errno == EINVAL, lane, "an untyped array reports EINVAL");
    errno = 0;
    host_check(dynamic_array_init_with_capacity(&raw, 0, 0) == -1, lane,
               "a zero element size must be refused");
    host_check(errno == EINVAL, lane, "a zero element size reports EINVAL");
}

/* An explicit capacity is honored, and a zero one allocates nothing. */
static void test_init_with_capacity(void)
{
    const char *lane = "init with capacity";

    dynamic_array_t array;
    host_check(dynamic_array_init_with_capacity(&array, sizeof(int), 32) == 0,
               lane, "init with a capacity must succeed");
    host_check(array.capacity == 32 && array.count == 0 && array.data, lane,
               "the requested capacity must be allocated and empty");
    dynamic_array_destroy(&array);

    dynamic_array_t empty;
    host_check(dynamic_array_init_with_capacity(&empty, sizeof(int), 0) == 0,
               lane, "init with zero capacity must succeed");
    host_check(empty.capacity == 0 && !empty.data, lane,
               "zero capacity must not allocate");
    dynamic_array_destroy(&empty);

    dynamic_array_t huge;
    errno = 0;
    host_check(
        dynamic_array_init_with_capacity(&huge, sizeof(int), SIZE_MAX) == -1,
        lane, "a capacity whose byte size overflows must be refused");
    host_check(errno == EOVERFLOW, lane, "that refusal reports EOVERFLOW");
}

int main(void)
{
    test_lazy_first_touch();
    test_growth_preserves_content();
    test_odd_element_stride();
    test_self_aliasing_append();
    test_failures_preserve_state();
    test_bounds();
    test_init_with_capacity();
    return host_summary("test-dynamic-array-host");
}
