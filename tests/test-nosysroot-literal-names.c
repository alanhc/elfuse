/*
 * Escape-shaped host names are ordinary names without a sysroot
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The escape encoding exists to let a case-folding sysroot hold names Linux
 * keeps apart. Without --sysroot there is no sysroot and no escaping: the guest
 * is looking straight at the host filesystem, where a file named ".ef=464f4f"
 * is a file named ".ef=464f4f" and nothing else. Decoding it would invent a
 * name the directory does not contain, and the guest would then be unable to
 * open the entry under either spelling: not the name it was shown, which is
 * not on disk, nor the name on disk, which it was never shown.
 *
 * Code under test: path_translate_dirent_name in src/syscall/path.c, reached
 * from the getdents64 loop in src/syscall/fs.c. A regression shows up as a
 * listing that reports a name no open() can then resolve.
 *
 * argv[1] is a host directory staged by the make recipe, because a test running
 * without a sysroot has nowhere of its own to write.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "test-harness.h"
#include "test-util.h"

int passes = 0, fails = 0;

/* The escape of "FOO" on a folding sysroot. Here it must mean itself. */
#define ESCAPE_SHAPED ".ef=464f4f"
#define DECODES_TO "FOO"

int main(int argc, char **argv)
{
    char path[PATH_MAX];
    int fd;

    if (argc < 2) {
        printf("test-nosysroot-literal-names: no directory given\n");
        return 1;
    }

    printf("test-nosysroot-literal-names: literal names without a sysroot\n");

    TEST("an escape-shaped host name is listed under its own bytes");
    EXPECT_TRUE(dir_contains(argv[1], ESCAPE_SHAPED),
                "should appear as written");

    /* The decisive one: if the listing decoded the name, the guest was shown
     * DECODES_TO, which no directory entry matches.
     */
    TEST("the listing does not invent the decoded name");
    EXPECT_TRUE(!dir_contains(argv[1], DECODES_TO),
                "nothing on disk has that name");

    TEST("the name the listing reported can be opened");
    snprintf(path, sizeof(path), "%s/%s", argv[1], ESCAPE_SHAPED);
    fd = open(path, O_RDONLY);
    EXPECT_TRUE(fd >= 0, "the listed name must resolve");
    if (fd >= 0)
        close(fd);

    TEST("the decoded name resolves to nothing");
    snprintf(path, sizeof(path), "%s/%s", argv[1], DECODES_TO);
    EXPECT_ERRNO(open(path, O_RDONLY), ENOENT, "should not exist");

    SUMMARY("test-nosysroot-literal-names");
    return fails > 0 ? 1 : 0;
}
