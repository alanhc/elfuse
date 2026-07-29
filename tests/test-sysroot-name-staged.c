/*
 * Host-staged names the guest could not have created
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A sysroot is an ordinary directory tree and anything may write into it, so
 * the guest view has to be well defined for names elfuse itself would never
 * produce. Two questions matter.
 *
 * A name that is a well-formed escape means the name it decodes to, wherever
 * it came from, which is what makes the mapping a property of the name and
 * not of who wrote it. A name that merely resembles one means itself:
 * uppercase hex, an odd number of digits, a payload decoding to "/" or ".."
 * are all ordinary files, and the guest must be able to open them under the
 * bytes they are spelled with.
 *
 * Code under test: casefold_is_escaped and casefold_to_guest in
 * src/syscall/casefold.c, reached through the directory-entry and lookup paths
 * in src/syscall/path.c. A regression shows up as a staged file the guest
 * cannot open under any name, or as a name that resembles an escape being
 * decoded into something else.
 *
 * The recipe stages all of this host-side, because a guest cannot create a
 * name that elfuse would store under a different spelling. Run under
 * --sysroot; the fixtures are staged by the make recipe.
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

#define DIR_S "/staged"

/* Everything in this lane lives under one directory and is addressed by name,
 * so the shared helpers are reached through a path built here. Only the
 * composition is local; the I/O is not duplicated.
 */
static int content_is(const char *name, const char *want)
{
    char path[PATH_MAX];

    /* The recipe stages these with printf, which appends a newline the
     * assertion is not about, so the comparison is a prefix.
     */
    snprintf(path, sizeof(path), "%s/%s", DIR_S, name);
    return file_content_starts_with(path, want);
}

static bool listed(const char *name)
{
    return dir_contains(DIR_S, name);
}

static void expect_reachable(const char *label,
                             const char *name,
                             const char *want)
{
    TEST(label);
    if (content_is(name, want) < 0) {
        FAIL("should be readable under this spelling");
        return;
    }
    if (!listed(name)) {
        FAIL("should appear in a listing under this spelling");
        return;
    }
    PASS();
}

/* Absent has to mean absent both ways, because expect_reachable requires both.
 * An ENOENT from open alone would also be satisfied by a name the listing still
 * advertises, which is the worse failure of the two: a guest that reads the
 * directory is then told about a file it cannot open.
 */
static void expect_absent(const char *label, const char *name)
{
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "%s/%s", DIR_S, name);
    TEST(label);
    if (listed(name)) {
        FAIL("still advertised by the listing");
        return;
    }
    EXPECT_ERRNO(open(path, O_RDONLY), ENOENT, "should not resolve");
}

/* A name whose slot is taken by a differently-spelled sibling is absent to
 * Linux, and must stay inside the sysroot rather than falling through to the
 * host filesystem. The sysroot holds the other spelling and the host holds a
 * real file at the very path the fallback would reach, so a leak reads as that
 * file's contents instead of ENOENT.
 *
 * Run against two guest paths, because a path that misses in the sysroot takes
 * one of two very different exits: paths under /tmp, /var/tmp and ccache are
 * forced back into the sysroot, and everything else falls through to the host.
 * Only the second can write outside the sysroot, and only the first can write
 * into the folded entry, so one path exercises one branch and proves nothing
 * about the other. The guard under test sits above the split; running both is
 * what keeps a later change from sliding it below.
 *
 * @guest_path comes from argv because the recipe has to pick a name unique to
 * the run before it can stage the host side of it. The fall-through path has to
 * be entirely lowercase so every component above the folded one resolves
 * exactly, and must avoid both the redirect list and the guest system
 * directories; /private/tmp is the one macOS location that is all three.
 */
static void section_folded_stays_inside(const char *guest_path,
                                        const char *which)
{
    char child[PATH_MAX];
    char label[128];
    int fd;

    /* Visible, so a manual run without the recipe's fixture paths reads as
     * fewer tests run, not as the section passing.
     */
    if (!guest_path || !guest_path[0]) {
        printf("  (folded-name section skipped: no %s path given)\n", which);
        return;
    }

    snprintf(label, sizeof(label), "%s: a folded name does not reach the host",
             which);
    TEST(label);
    snprintf(child, sizeof(child), "%s/planted", guest_path);
    fd = open(child, O_RDONLY);
    if (fd >= 0) {
        close(fd);
        FAIL("a wrong-case lookup reached the host filesystem");
    } else {
        EXPECT_ERRNO(fd, ENOENT, "should be ENOENT, not the host file");
    }

    /* Creating below the same folded component must fail the same way. Lookup
     * and create are answered by separate resolvers, so closing the hole for
     * one does not close it for the other, and a create does damage a lookup
     * does not: it writes.
     */
    snprintf(label, sizeof(label),
             "%s: a create below a folded component fails", which);
    TEST(label);
    snprintf(child, sizeof(child), "%s/created", guest_path);
    fd = open(child, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) {
        close(fd);
        FAIL("a create landed under a folded component");
    } else {
        EXPECT_ERRNO(fd, ENOENT, "the named parent does not exist");
    }
}

int main(int argc, char **argv)
{
    /* The control: an ordinary host-staged name, mixed case, kept under its
     * real spelling. This is what makes a rootfs unpacked from a tarball
     * reachable at all.
     */
    expect_reachable("plain host-staged name", "Plain.Host", "plain");

    /* A well-formed escape staged by the host decodes like any other, so the
     * guest sees the name it stands for.
     */
    expect_reachable("staged escape decodes", "FOO", "escaped-foo");

    /* And the on-disk spelling is not itself a guest name: asking for it names
     * something else entirely, which is absent.
     */
    expect_absent("the on-disk spelling is not a guest name", ".ef=464f4f");

    /* Shapes that only resemble an escape mean themselves. Each is staged
     * host-side under exactly these bytes.
     */
    expect_reachable("uppercase hex is not an escape", ".ef=5A5A",
                     "literal-upper");
    expect_reachable("odd digit count is not an escape", ".ef=464f4",
                     "literal-odd");
    expect_reachable("non-hex payload is not an escape", ".ef=zzzz",
                     "literal-nonhex");
    expect_reachable("a payload decoding to a slash is not an escape", ".ef=2f",
                     "literal-slash");
    expect_reachable("a payload decoding to dotdot is not an escape",
                     ".ef=2e2e", "literal-dotdot");
    expect_reachable("the bare prefix is not an escape",
                     ".ef=", "literal-bare");
    expect_reachable("a different separator is not an escape", ".ef_464f4f",
                     "literal-legacy");

    /* Both spellings of one name staged together. Only something outside
     * elfuse can produce this, and the rule is that a lookup takes the literal
     * spelling: the escaped one is then unreachable under any guest name. The
     * listing reports the name twice, which is the visible cost of letting a
     * host tool write a tree elfuse also manages.
     */
    expect_reachable("literal wins over an escape of the same name", "Bar",
                     "literal-bar");

    section_folded_stays_inside(argc > 1 ? argv[1] : NULL, "redirected");
    section_folded_stays_inside(argc > 2 ? argv[2] : NULL, "host-fallback");

    SUMMARY("test-sysroot-name-staged");
    return fails > 0 ? 1 : 0;
}
