/*
 * Escape-shaped names are literal in directories the sysroot does not own
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The escape encoding is scoped to the sysroot: only there did elfuse choose
 * the stored spelling, so only there may a listing decode one. A host
 * directory reached through the fallback holds names elfuse never wrote. A
 * file named ".ef=464f4f" in such a directory is a file named ".ef=464f4f"
 * and nothing else; decoding it would report a name the directory does not
 * contain and that no open() can resolve, while hiding the entry's real name
 * behind it. Lookups already treat these directories literally, so a decoding
 * listing also disagrees with the resolver about what the directory holds.
 *
 * Code under test: path_translate_dirent_name and its per-directory scoping
 * in src/syscall/path.c, reached from the getdents64 loop in
 * src/syscall/fs.c via the host fallback in proc_resolve_sysroot_path_flags.
 * A regression shows up as ls of a host directory inventing a name no open()
 * resolves, and as two real entries collapsing into one listed name.
 *
 * argv[1] is a host directory staged by the make recipe, outside the sysroot
 * the recipe also creates. Run under --sysroot on a case-folding volume; the
 * recipe asserts host-side that the control file the guest creates inside
 * the sysroot was stored escaped, so a pass on a byte-exact volume cannot be
 * vacuous.
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

/* dir_contains answers membership only; the collapse regression needs the
 * count, because the decoded spelling of one entry equals the literal
 * spelling of another.
 */
static int dir_count_name(const char *dir, const char *name)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    int n = 0;

    if (!d)
        return -1;
    while ((de = readdir(d)))
        if (!strcmp(de->d_name, name))
            n++;
    closedir(d);
    return n;
}

int main(int argc, char **argv)
{
    char path[PATH_MAX];
    int fd;

    if (argc < 2) {
        printf("test-sysroot-outside-names: no directory given\n");
        return 1;
    }

    printf("test-sysroot-outside-names: literal names outside the sysroot\n");

    TEST("an escape-shaped name outside the sysroot lists as its own bytes");
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

    /* Creates outside the sysroot are literal too, so after this the
     * directory really holds both spellings. A decoding listing folds them
     * into two entries named DECODES_TO: the file just written plus the
     * decode of the staged escape, with the escape's own bytes gone.
     */
    TEST("a created literal name does not collapse with the escape");
    fd = open(path, O_CREAT | O_WRONLY, 0644);
    if (fd < 0) {
        FAIL("create");
    } else {
        close(fd);
        EXPECT_TRUE(dir_count_name(argv[1], DECODES_TO) == 1 &&
                        dir_count_name(argv[1], ESCAPE_SHAPED) == 1,
                    "each spelling must list exactly once");
        unlink(path);
    }

    /* Host-side control for the recipe: stored escaped only if the volume
     * folds and the sysroot scope still decodes, proving the assertions
     * above did not pass merely because nothing was escaping anywhere.
     */
    TEST("a control name inside the sysroot still escapes");
    fd = open("/Ctrl", O_CREAT | O_WRONLY, 0644);
    EXPECT_TRUE(fd >= 0, "create /Ctrl in the sysroot");
    if (fd >= 0) {
        close(fd);
        EXPECT_TRUE(dir_contains("/", "Ctrl"),
                    "the sysroot listing still decodes");
    }

    SUMMARY("test-sysroot-outside-names");
    return fails > 0 ? 1 : 0;
}
