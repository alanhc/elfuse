/*
 * Decode of a host-staged escape corpus
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The recipe stages a small tree of on-disk spellings copied byte-for-byte
 * from tests/casefold-vectors.h (the frozen format), and this guest opens
 * every entry strictly by its guest name. That direction is what an existing
 * sysroot exercises after an elfuse upgrade: the disk holds spellings an
 * older build wrote, and the current build must keep reading them. The codec
 * unit test asserts the same table in-process; this asserts it end to end,
 * through staging the recipe cannot derive from the codec under test.
 *
 * test-sysroot-name-staged is the neighbor with a different question: it pins
 * what escape-shaped and escape-resembling names mean when a host stages
 * them. Here every staged spelling is a well-formed escape from the frozen
 * table, and the assertion is that its guest name, and nothing else, reaches
 * it.
 *
 * Each staged file's content is its own guest name, so a resolve that lands
 * anywhere unexpected is caught by the first read. A regression shows up as
 * ENOENT on a guest name whose spelling is on disk (the decoder moved off
 * the frozen format), as an escape spelling leaking into a listing, or as
 * content that names a different file.
 *
 * Run under --sysroot on a folding volume; the recipe probes and skips
 * elsewhere, since staged escapes only mean their guest names where the
 * escape is active.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test-harness.h"
#include "test-util.h"

int passes = 0, fails = 0;

#define DIR_C "/corpus"

/* The staged entry must answer to its guest name and carry it as content;
 * the trailing newline the recipe appends is not part of the assertion.
 */
static void check_reads_itself(const char *label, const char *guest)
{
    char path[PATH_MAX];

    TEST(label);
    snprintf(path, sizeof(path), DIR_C "/%s", guest);
    if (file_content_starts_with(path, guest) < 0) {
        FAIL("guest name did not reach the staged spelling and content");
        return;
    }
    PASS();
}

int main(void)
{
    char longx[NAME_MAX + 1];

    memset(longx, 'X', 126);
    longx[126] = '\0';

    check_reads_itself("hex tier, uppercase", "Foo");
    check_reads_itself("hex tier, all uppercase", "README");
    check_reads_itself("hex tier, non-ascii", "caf\xc3\xa9");
    check_reads_itself("long tier, 126 bytes", longx);

    TEST("nested escaped directories resolve");
    if (file_content_starts_with(DIR_C "/GuestDir/New.File", "New.File") < 0)
        FAIL("a nested guest path under an escaped directory");
    else
        PASS();

    TEST("the on-disk spelling is not a guest name");
    {
        struct stat st;
        EXPECT_ERRNO(stat(DIR_C "/.ef=466f6f", &st), ENOENT, "host spelling");
    }

    TEST("the listing holds guest spellings only");
    {
        DIR *d = opendir(DIR_C);
        struct dirent *de;
        bool saw_long = false;
        bool leaked = false;
        int entries = 0;

        if (!d) {
            FAIL("opendir");
        } else {
            while ((de = readdir(d))) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                    continue;
                entries++;
                if (!strncmp(de->d_name, ".ef=", 4))
                    leaked = true;
                if (!strcmp(de->d_name, longx))
                    saw_long = true;
            }
            closedir(d);
            if (leaked)
                FAIL("an escape spelling leaked into the listing");
            else if (!saw_long)
                FAIL("the 126-byte name is missing or truncated");
            else if (entries != 5)
                FAIL("unexpected entry count");
            else
                PASS();
        }
    }

    TEST("stat by name matches the opened file");
    {
        struct stat by_name, by_fd;
        int fd = open(DIR_C "/Foo", O_RDONLY);

        if (fd < 0 || stat(DIR_C "/Foo", &by_name) < 0 ||
            fstat(fd, &by_fd) < 0 || by_name.st_ino != by_fd.st_ino ||
            by_name.st_dev != by_fd.st_dev)
            FAIL("stat and fstat disagree about the staged file");
        else
            PASS();
        if (fd >= 0)
            close(fd);
    }

    SUMMARY("test-sysroot-corpus");
    return fails > 0 ? 1 : 0;
}
