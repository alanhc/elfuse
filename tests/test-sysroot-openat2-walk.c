/*
 * openat2(RESOLVE_NO_SYMLINKS) path walking under a sysroot
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * macOS has no equivalent of RESOLVE_NO_SYMLINKS, so elfuse answers the flag by
 * walking the path itself and reporting ELOOP at the first symlink component.
 * Linux bounds that resolution by the number of links followed, never by the
 * number of components walked (path_resolution(7)), so a link-free path
 * resolves however deep it runs. The walk sees the host spelling, which carries
 * the sysroot's own directories in front of the guest path, and those are not
 * the guest's to account for either. A dirfd-relative path is walked from the
 * descriptor instead, where Linux clamps '..' at the guest root
 * (path_resolution(7)); the walk must stop climbing there rather than continue
 * onto the host directories above the sysroot.
 *
 * Code under test: sys_path_has_symlink() in src/syscall/path.c, both the
 * absolute arm and the dirfd-relative arm. A regression reports ELOOP or ENOENT
 * for a link-free path Linux resolves: a deep chain through the absolute arm,
 * or an escaping '..' whose relative walk leaves the sysroot and meets the
 * host's own symlinks (macOS /tmp). The chains are staged by the recipe rather
 * than by the guest so their names reach the disk literally, keeping the walk
 * the only thing under test.
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

/* The deep chain runs past WALK_MAXSYMLINKS; the shallow one stays under it on
 * its own but crosses it once the sysroot's host directories are counted too,
 * which is the shape only a sysroot can show.
 */
#define DEEP_COMPONENTS (WALK_MAXSYMLINKS + 4)
#define SHALLOW_COMPONENTS (WALK_MAXSYMLINKS - 2)

static int openat2_no_symlinks(int dirfd, const char *path)
{
    struct open_how how = {.flags = O_RDONLY | O_DIRECTORY,
                           .mode = 0,
                           .resolve = RESOLVE_NO_SYMLINKS};

    return (int) syscall(SYS_openat2, dirfd, path, &how, sizeof(how));
}

static int open_no_symlinks(const char *path)
{
    return openat2_no_symlinks(AT_FDCWD, path);
}

/* Spell the chain the recipe staged: "/<root>" then @components levels of "d".
 */
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

static void expect_dirfd_resolves(const char *label,
                                  int dirfd,
                                  const char *path)
{
    int fd;

    TEST(label);
    fd = openat2_no_symlinks(dirfd, path);
    if (fd >= 0)
        close(fd);
    EXPECT_TRUE(fd >= 0, "a link-free path must resolve");
}

/* The walk from a descriptor must clamp '..' where the guest root sits, not
 * where the sysroot's host parent does. An unclamped walk climbs onto the host,
 * where the next named component resolves against the host's own tree (macOS
 * /tmp is a symlink), so a path the guest resolves comes back ELOOP or ENOENT.
 */
static void section_relative(void)
{
    int root = open("/", O_RDONLY | O_DIRECTORY);
    int tmp = open("/tmp", O_RDONLY | O_DIRECTORY);

    TEST("open the guest root and /tmp as descriptors");
    EXPECT_TRUE(root >= 0 && tmp >= 0, "opening the base directories failed");
    if (root < 0 || tmp < 0)
        goto out;

    expect_dirfd_resolves("a relative path resolves from a descriptor", root,
                          "tmp/leaf");
    expect_dirfd_resolves("an escaping \"..\" clamps at the guest root", root,
                          "../../../../../../../../../../../../tmp/leaf");
    expect_dirfd_resolves("an interior \"..\" keeps the walk's depth honest",
                          root, "tmp/../../tmp/leaf");
    expect_dirfd_resolves("the clamp seeds from the descriptor's own depth",
                          tmp, "../../tmp/leaf");

    /* The counterweight: a clamped '..' must not blind the walk to the symlink
     * sitting where it lands.
     */
    TEST("a symlink is still ELOOP through a clamped \"..\"");
    EXPECT_ERRNO(openat2_no_symlinks(root, "../link/leaf"), ELOOP,
                 "a symlink component must be rejected");

out:
    if (root >= 0)
        close(root);
    if (tmp >= 0)
        close(tmp);
}

int main(void)
{
    printf("test-sysroot-openat2-walk: no-symlinks path walking\n");

    expect_resolves("a chain longer than the link budget resolves", "deep",
                    DEEP_COMPONENTS);
    expect_resolves("a chain under the budget resolves behind the sysroot",
                    "shallow", SHALLOW_COMPONENTS);

    /* The counterweight: with no budget left in the walk, the flag still has to
     * reject the thing it exists to reject.
     */
    TEST("a symlink component is still ELOOP");
    EXPECT_ERRNO(open_no_symlinks("/link/leaf"), ELOOP,
                 "a symlink component must be rejected");

    /* Guards the byte-for-byte copy of interior '..': a resolver that collapsed
     * it would spell this path /real/leaf and hide the link from the walk.
     */
    TEST("an interior .. keeps the symlink before it visible");
    EXPECT_ERRNO(open_no_symlinks("/link/../real/leaf"), ELOOP,
                 "the link must stay in the walked spelling");

    section_relative();

    SUMMARY("test-sysroot-openat2-walk");
    return fails > 0 ? 1 : 0;
}
