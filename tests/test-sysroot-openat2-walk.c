/*
 * openat2(RESOLVE_NO_SYMLINKS) path walking under a sysroot
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * macOS has no equivalent of RESOLVE_NO_SYMLINKS, so elfuse answers the flag
 * by walking the path itself and reporting ELOOP at the first symlink
 * component. Linux bounds that resolution by the number of links followed,
 * never by the number of components walked (path_resolution(7)), so a
 * link-free path resolves however deep it runs. The walk sees the host
 * spelling, which carries the sysroot's own directories in front of the guest
 * path, and those are not the guest's to account for either.
 *
 * Code under test: sys_path_has_symlink() in src/syscall/path.c. A regression
 * reports ELOOP for a deep path with no symlink in it, which surfaces as a
 * program that opens a file everywhere except through openat2. The chains are
 * staged by the recipe rather than by the guest so their names reach the disk
 * literally, keeping the walk the only thing under test.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "linux-openat2.h"
#include "test-harness.h"

int passes = 0, fails = 0;

/* The deep chain runs past WALK_MAXSYMLINKS; the shallow one stays under it
 * on its own but crosses it once the sysroot's host directories are counted
 * too, which is the shape only a sysroot can show.
 */
#define DEEP_COMPONENTS (WALK_MAXSYMLINKS + 4)
#define SHALLOW_COMPONENTS (WALK_MAXSYMLINKS - 2)

static int open_no_symlinks(const char *path)
{
    struct open_how how = {.flags = O_RDONLY | O_DIRECTORY,
                           .mode = 0,
                           .resolve = RESOLVE_NO_SYMLINKS};

    return (int) syscall(SYS_openat2, AT_FDCWD, path, &how, sizeof(how));
}

/* Spell the chain the recipe staged: "/<root>" then @components levels of
 * "d". */
static void chain_path(char *out,
                       size_t outsz,
                       const char *root,
                       int components)
{
    size_t len = (size_t) snprintf(out, outsz, "/%s", root);

    for (int i = 0; i < components && len < outsz; i++)
        len += (size_t) snprintf(out + len, outsz - len, "/d");
}

static void expect_resolves(const char *label, const char *root, int components)
{
    char path[PATH_MAX];
    int fd;

    TEST(label);
    chain_path(path, sizeof(path), root, components);
    fd = open_no_symlinks(path);
    if (fd >= 0)
        close(fd);
    EXPECT_TRUE(fd >= 0, "a link-free path must resolve");
}

int main(void)
{
    printf("test-sysroot-openat2-walk: no-symlinks path walking\n");

    expect_resolves("a chain longer than the link budget resolves", "deep",
                    DEEP_COMPONENTS);
    expect_resolves("a chain under the budget resolves behind the sysroot",
                    "shallow", SHALLOW_COMPONENTS);

    /* The counterweight: with no budget left in the walk, the flag still has
     * to reject the thing it exists to reject.
     */
    TEST("a symlink component is still ELOOP");
    EXPECT_ERRNO(open_no_symlinks("/link/leaf"), ELOOP,
                 "a symlink component must be rejected");

    SUMMARY("test-sysroot-openat2-walk");
    return fails > 0 ? 1 : 0;
}
