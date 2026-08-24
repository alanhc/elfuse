/*
 * Native-host unit tests for string_builder_t.
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * White-box: the builder is header-only and these are its own unit tests, so
 * they read storage.capacity directly to check the terminator invariant rather
 * than asking for a capacity accessor no caller in src/ needs.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host-test-util.h"
#include "utils.h"

static void expect_string(const string_builder_t *builder,
                          const char *expected,
                          const char *label)
{
    size_t length = strlen(expected);
    host_check(string_builder_length(builder) == length, label,
               "length must match the expected text");
    if (string_builder_data_const(builder)) {
        host_check(strcmp(string_builder_data_const(builder), expected) == 0,
                   label, "contents must match the expected text");
        host_check(string_builder_data_const(builder)[length] == '\0', label,
                   "the payload must be NUL-terminated");
    } else {
        host_check(length == 0, label,
                   "a NULL buffer may only stand for the empty string");
    }
}

/* The builder owns one byte past its payload for the terminator, so every
 * successful append must leave room for it.
 */
static void expect_terminator_room(const string_builder_t *builder,
                                   const char *label)
{
    host_check(builder->storage.capacity > builder->storage.count, label,
               "capacity must exceed the payload so the NUL fits");
}

static void test_zero_and_initial_capacity(void)
{
    const char *lane = "zero and initial capacity";

    /* Fresh automatic objects need not be manually zeroed before init. */
    string_builder_t fresh;
    host_check(string_builder_init(&fresh, 0) == 0, lane,
               "init with zero capacity must succeed");
    host_check(string_builder_length(&fresh) == 0, lane,
               "a fresh builder must be empty");
    host_check(!string_builder_data_const(&fresh), lane,
               "zero capacity must not allocate");
    host_check(string_builder_appendf(&fresh, "%s", "fresh") == 0, lane,
               "the first append must succeed");
    expect_string(&fresh, "fresh", lane);
    expect_terminator_room(&fresh, lane);
    string_builder_destroy(&fresh);

    /* A plain {0} value is usable without an init call. */
    string_builder_t zero = {0};
    host_check(string_builder_length(&zero) == 0, lane,
               "a {0} builder must be empty");
    host_check(!string_builder_data_const(&zero), lane,
               "a {0} builder must not have storage");
    /* A format that renders nothing must not allocate. */
    host_check(string_builder_appendf(&zero, "%c", '\0') == 0, lane,
               "an empty rendering must succeed");
    host_check(!string_builder_data_const(&zero), lane,
               "an empty rendering must not allocate");
    host_check(string_builder_appendf(&zero, "%s", "zero") == 0, lane,
               "append to a {0} builder must succeed");
    expect_string(&zero, "zero", lane);
    string_builder_destroy(&zero);

    /* initial_capacity includes the trailing NUL byte. */
    string_builder_t initial = {0};
    host_check(string_builder_init(&initial, 32) == 0, lane,
               "init with capacity must succeed");
    host_check(initial.storage.capacity >= 32, lane,
               "the requested capacity must be honored");
    host_check(string_builder_data_const(&initial) != NULL, lane,
               "a nonzero capacity must allocate");
    expect_string(&initial, "", lane);
    string_builder_destroy(&initial);
}

static void test_formatted_append(void)
{
    const char *lane = "formatted append";
    string_builder_t builder = {0};
    host_check(string_builder_init(&builder, 1) == 0, lane, "init");

    host_check(string_builder_appendf(&builder, "%s", "prefix") == 0, lane,
               "plain append");
    host_check(string_builder_appendf(&builder, ":%s:%d", "formatted", 42) == 0,
               lane, "multi-conversion append");
    expect_string(&builder, "prefix:formatted:42", lane);
    expect_terminator_room(&builder, lane);

    /* An empty rendering is a no-op. */
    size_t old_length = string_builder_length(&builder);
    host_check(string_builder_appendf(&builder, "%s", "") == 0, lane,
               "empty append must succeed");
    host_check(string_builder_length(&builder) == old_length, lane,
               "empty append must not change the length");
    expect_string(&builder, "prefix:formatted:42", lane);
    string_builder_destroy(&builder);
}

/* A formatted NUL ends the appended prefix, whether or not the builder already
 * owns storage large enough to hold what followed it.
 */
static void test_c_string_semantics(void)
{
    const char *lane = "C string semantics";
    string_builder_t builder = {0};
    host_check(string_builder_appendf(&builder, "%s", "prefix") == 0, lane,
               "seed the builder");

    errno = 0;
    host_check(string_builder_appendf(&builder, "x%c y", '\0') == 0, lane,
               "append past a formatted NUL must succeed");
    host_check(errno == 0, lane, "a successful append must not disturb errno");
    expect_string(&builder, "prefixx", lane);
    string_builder_destroy(&builder);

    string_builder_t fit = {0};
    host_check(string_builder_init(&fit, 16) == 0, lane, "init with room");
    errno = 0;
    host_check(string_builder_appendf(&fit, "a%c%d", '\0', 1) == 0, lane,
               "append into preallocated storage must succeed");
    host_check(errno == 0, lane, "a successful append must not disturb errno");
    expect_string(&fit, "a", lane);
    string_builder_destroy(&fit);
}

static void test_formatted_append_reserves_terminator(void)
{
    const char *lane = "terminator reservation";
    string_builder_t builder = {0};

    /* A formatted append must reserve one byte beyond the visible payload for
     * the builder's trailing NUL. This exact-length payload matches the generic
     * array's initial capacity, so reserving only the payload bytes puts the
     * terminator store one byte past the allocation.
     */
    host_check(string_builder_appendf(&builder, "%s", "12345678") == 0, lane,
               "an exact-capacity payload must append");
    expect_string(&builder, "12345678", lane);
    expect_terminator_room(&builder, lane);
    string_builder_destroy(&builder);
}

static void test_growth_preserves_content(void)
{
    enum { COUNT = 4096 };
    const char *lane = "growth preserves content";
    char expected[COUNT];
    string_builder_t builder = {0};
    bool appends_ok = string_builder_init(&builder, 1) == 0;

    for (size_t i = 0; i < COUNT; i++) {
        expected[i] = (char) ('A' + (i % 26));
        appends_ok &= string_builder_appendf(&builder, "%c", expected[i]) == 0;
    }
    host_check(appends_ok, lane, "every append across the growth must succeed");
    host_check(string_builder_length(&builder) == COUNT, lane,
               "the length must be the number of appends");
    host_check(
        memcmp(string_builder_data_const(&builder), expected, COUNT) == 0, lane,
        "growth must preserve the payload");
    host_check(string_builder_data_const(&builder)[COUNT] == '\0', lane,
               "growth must preserve the terminator");
    expect_terminator_room(&builder, lane);
    string_builder_destroy(&builder);
}

/* Formatting runs into separate storage, so a format string and a %s argument
 * that both alias the builder stay valid across the growth that follows.
 */
static void test_formatted_alias_append(void)
{
    const char *lane = "aliased format";
    string_builder_t builder = {0};
    host_check(string_builder_appendf(&builder, "%s", "x%s") == 0, lane,
               "seed the builder with a format-looking payload");
    const char *alias = string_builder_data_const(&builder);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
    host_check(string_builder_appendf(&builder, alias, alias) == 0, lane,
               "a format aliasing the builder must append");
#pragma clang diagnostic pop
    expect_string(&builder, "x%sxx%s", lane);
    string_builder_destroy(&builder);
}

/* A byte append whose source points into the builder's own storage has to
 * survive the reserve that the same append triggers. append_n reserves one byte
 * more than the payload, so it cannot lean on the generic array's own alias
 * handling: that reserve can move the block first and leave the caller's
 * pointer dangling.
 */
static void test_byte_append_alias(void)
{
    const char *lane = "aliased byte append";
    string_builder_t builder = {0};

    host_check(string_builder_append_n(&builder, "abcd", 4) == 0, lane, "seed");

    /* Double the builder onto itself until a reserve has to grow the block. */
    bool appends_ok = true, mirrored = true;
    for (int round = 0; round < 6; round++) {
        size_t half = string_builder_length(&builder);
        appends_ok &=
            string_builder_append_n(
                &builder, string_builder_data_const(&builder), half) == 0;
        const char *data = string_builder_data_const(&builder);
        mirrored &= string_builder_length(&builder) == half * 2;
        for (size_t i = 0; i < half; i++)
            mirrored &= data[i] == data[half + i];
    }
    host_check(appends_ok, lane, "every self-append must succeed");
    host_check(mirrored, lane,
               "a self-append must copy the pre-growth contents");
    expect_terminator_room(&builder, lane);
    host_check(string_builder_data_const(
                   &builder)[string_builder_length(&builder)] == '\0',
               lane, "the result must stay NUL-terminated");
    string_builder_destroy(&builder);
}

static void test_formatted_errno_expansion(void)
{
    const char *lane = "errno expansion";
    string_builder_t builder = {0};
    char expected[128];

    /* %m is a GNU printf extension; use the host libc as the oracle so Darwin
     * (which leaves it literal) remains a valid host-test platform.
     */
    errno = ENOENT;
    host_check(snprintf(expected, sizeof(expected), "errno=%m") >= 0, lane,
               "the oracle rendering must succeed");
    errno = ENOENT;
    host_check(string_builder_appendf(&builder, "errno=%m") == 0, lane,
               "the builder rendering must succeed");
    host_check(strcmp(string_builder_data_const(&builder), expected) == 0, lane,
               "%m must expand exactly as the host libc does");
    host_check(errno == ENOENT, lane,
               "the caller's errno must survive the append");
    string_builder_destroy(&builder);
}

static void test_invalid_input_preserves_content(void)
{
    const char *lane = "invalid input";
    string_builder_t builder = {0};
    host_check(string_builder_init(&builder, 0) == 0, lane, "init");
    host_check(string_builder_appendf(&builder, "prefix:%d", 7) == 0, lane,
               "seed the builder");

    char snapshot[32];
    size_t old_length = string_builder_length(&builder);
    host_check(old_length < sizeof(snapshot), lane,
               "the seed must fit the snapshot buffer");
    memcpy(snapshot, string_builder_data_const(&builder), old_length);
    size_t old_capacity = builder.storage.capacity;

    const char *null_format = NULL;
    errno = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#pragma clang diagnostic ignored "-Wformat-security"
    host_check(string_builder_appendf(&builder, null_format) == -1, lane,
               "a NULL format must be refused");
#pragma clang diagnostic pop
    host_check(errno == EILSEQ, lane, "a refused append must report EILSEQ");
    host_check(string_builder_length(&builder) == old_length, lane,
               "a refused append must not change the length");
    host_check(builder.storage.capacity == old_capacity, lane,
               "a refused append must not reallocate");
    host_check(
        memcmp(string_builder_data_const(&builder), snapshot, old_length) == 0,
        lane, "a refused append must not disturb the payload");
    host_check(string_builder_data_const(&builder)[old_length] == '\0', lane,
               "a refused append must leave the terminator in place");

    errno = 0;
    host_check(string_builder_appendf(NULL, "%d", 1) == -1, lane,
               "a NULL builder must be refused");
    host_check(errno == EILSEQ, lane, "a refused builder must report EILSEQ");
    string_builder_destroy(&builder);
}

int main(void)
{
    test_zero_and_initial_capacity();
    test_formatted_append();
    test_c_string_semantics();
    test_formatted_append_reserves_terminator();
    test_growth_preserves_content();
    test_formatted_alias_append();
    test_byte_append_alias();
    test_formatted_errno_expansion();
    test_invalid_input_preserves_content();
    return host_summary("test-string-builder-host");
}
