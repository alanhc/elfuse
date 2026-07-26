/*
 * One representation per guest name
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * On a case-folding sysroot a name whose spelling the volume cannot hold is
 * stored escaped, so a directory can end up holding a mixture of literal and
 * escaped entries. What must hold throughout is that each guest name is
 * reachable through exactly one of them: every spelling opens its own file,
 * a spelling that was never created reports ENOENT, and a listing reports each
 * name once and never leaks an on-disk spelling.
 *
 * The sequence below is adversarial on purpose. It creates the members of a
 * case-colliding set in an order that makes each one take a different kind of
 * slot, then deletes and recreates them so the literal slot changes hands
 * while the others stay put. No assertion here may depend on *which* spelling
 * won the literal slot: that follows arrival order and is not part of the
 * contract.
 *
 * Code under test: the resolver in src/syscall/casefold-walk.c reached through
 * src/syscall/path.c, and the mutating handlers in src/syscall/fs.c that use
 * its result. A regression shows up as one spelling opening another's file, a
 * name surviving its own unlink, or a listing that disagrees with what can be
 * opened.
 *
 * Run under --sysroot. The host-side shape check in the make recipe asserts
 * the on-disk half.
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

#define DIR_R "/name-unique"

/* Sorted, comma-joined listing of DIR_R, so an expectation reads literally and
 * a duplicate or a leaked on-disk spelling shows up in the diff.
 */
static const char *listing(void)
{
    static char out[512];
    char names[32][NAME_MAX + 1];
    int n = 0;
    DIR *d = opendir(DIR_R);
    struct dirent *de;
    size_t len = 0;

    out[0] = '\0';
    if (!d)
        return "<opendir failed>";
    while ((de = readdir(d)) && n < 32) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        snprintf(names[n++], NAME_MAX + 1, "%s", de->d_name);
    }
    closedir(d);

    for (int i = 1; i < n; i++) {
        char key[NAME_MAX + 1];
        int j = i - 1;
        snprintf(key, sizeof(key), "%s", names[i]);
        while (j >= 0 && strcmp(names[j], key) > 0) {
            snprintf(names[j + 1], NAME_MAX + 1, "%s", names[j]);
            j--;
        }
        snprintf(names[j + 1], NAME_MAX + 1, "%s", key);
    }
    for (int i = 0; i < n; i++)
        len += (size_t) snprintf(out + len, sizeof(out) - len, "%s%s",
                                 i ? "," : "", names[i]);
    return out;
}

static void expect_listing(const char *label, const char *want)
{
    const char *got = listing();

    TEST(label);
    if (strcmp(got, want)) {
        printf("\n    got      %s\n    expected %s\n  ", got, want);
        FAIL("listing mismatch");
        return;
    }
    PASS();
}

static void expect_absent(const char *label, const char *path)
{
    TEST(label);
    EXPECT_ERRNO(open(path, O_RDONLY), ENOENT, "should not resolve");
}

static void expect_content(const char *label,
                           const char *path,
                           const char *want)
{
    TEST(label);
    EXPECT_TRUE(file_content_is(path, want) == 0, "wrong content");
}

int main(void)
{
    TEST("fixture mkdir");
    EXPECT_TRUE(mkdir(DIR_R, 0755) == 0 || errno == EEXIST, "mkdir");

    /* The first member takes the literal slot on a folding volume, or simply
     * its own name on a byte-exact one. Either way only that spelling exists.
     */
    TEST("create Foo");
    EXPECT_TRUE(file_write(DIR_R "/Foo", "A") == 0, "create Foo");
    expect_absent("foo absent before it is made", DIR_R "/foo");
    expect_absent("FOO absent before it is made", DIR_R "/FOO");
    expect_listing("listing holds Foo alone", "Foo");

    /* The second member cannot take the same slot, so it is stored escaped,
     * and the guest must not be able to tell.
     */
    TEST("create foo beside Foo");
    EXPECT_TRUE(file_write(DIR_R "/foo", "B") == 0, "create foo");
    expect_content("Foo keeps its own content", DIR_R "/Foo", "A");
    expect_content("foo has its own content", DIR_R "/foo", "B");
    expect_absent("FOO still absent", DIR_R "/FOO");
    expect_listing("listing holds both", "Foo,foo");

    /* A third spelling nobody created must not resolve to either of them. */
    TEST("create FOO exclusively");
    EXPECT_TRUE(open(DIR_R "/FOO", O_CREAT | O_EXCL | O_WRONLY, 0644) >= 0,
                "O_EXCL create of a third spelling");
    EXPECT_TRUE(file_write(DIR_R "/FOO", "C") == 0, "write FOO");
    expect_content("Foo unaffected", DIR_R "/Foo", "A");
    expect_content("foo unaffected", DIR_R "/foo", "B");
    expect_listing("listing holds all three", "FOO,Foo,foo");

    TEST("O_EXCL on an existing spelling");
    EXPECT_ERRNO(open(DIR_R "/foo", O_CREAT | O_EXCL | O_WRONLY, 0644), EEXIST,
                 "should already exist");

    /* Removing whichever member holds the literal slot must leave the others
     * reachable under their own names.
     */
    TEST("unlink Foo");
    EXPECT_TRUE(unlink(DIR_R "/Foo") == 0, "unlink Foo");
    expect_absent("Foo gone", DIR_R "/Foo");
    expect_content("foo survives", DIR_R "/foo", "B");
    expect_content("FOO survives", DIR_R "/FOO", "C");
    expect_listing("listing holds the survivors", "FOO,foo");

    /* Recreating it must not disturb the survivors, whichever slot it lands in
     * the second time round.
     */
    TEST("recreate Foo");
    EXPECT_TRUE(file_write(DIR_R "/Foo", "D") == 0, "recreate Foo");
    expect_content("Foo has its new content", DIR_R "/Foo", "D");
    expect_content("foo still its own", DIR_R "/foo", "B");
    expect_content("FOO still its own", DIR_R "/FOO", "C");
    expect_listing("listing holds all three again", "FOO,Foo,foo");

    /* Rename across the collision set: the source name goes, the destination
     * takes the source's content, and the third member is untouched.
     */
    TEST("rename Foo onto foo");
    EXPECT_TRUE(rename(DIR_R "/Foo", DIR_R "/foo") == 0, "rename");
    expect_absent("Foo gone after rename", DIR_R "/Foo");
    expect_content("foo took the content", DIR_R "/foo", "D");
    expect_content("FOO untouched by the rename", DIR_R "/FOO", "C");
    expect_listing("listing after rename", "FOO,foo");

    /* A hard link is a second name for one file, and both must resolve to it
     * even when one is stored literally and the other escaped.
     */
    TEST("hard link fOO to foo");
    EXPECT_TRUE(link(DIR_R "/foo", DIR_R "/fOO") == 0, "link");
    expect_content("the link reads the same file", DIR_R "/fOO", "D");
    {
        struct stat a, b;
        TEST("link shares an inode");
        EXPECT_TRUE(stat(DIR_R "/foo", &a) == 0 &&
                        stat(DIR_R "/fOO", &b) == 0 && a.st_ino == b.st_ino &&
                        a.st_nlink == 2,
                    "link should share the inode");
    }
    expect_listing("listing after link", "FOO,fOO,foo");

    /* Taking the set apart one at a time: each removal leaves the rest
     * readable under their own names.
     */
    TEST("unlink fOO");
    EXPECT_TRUE(unlink(DIR_R "/fOO") == 0, "unlink fOO");
    expect_content("foo survives the link removal", DIR_R "/foo", "D");
    TEST("unlink FOO");
    EXPECT_TRUE(unlink(DIR_R "/FOO") == 0, "unlink FOO");
    expect_content("foo is the last one standing", DIR_R "/foo", "D");
    expect_listing("listing at the end", "foo");
    TEST("unlink foo");
    EXPECT_TRUE(unlink(DIR_R "/foo") == 0, "unlink foo");
    expect_listing("listing is empty", "");

    SUMMARY("test-sysroot-name-unique");
    return fails > 0 ? 1 : 0;
}
