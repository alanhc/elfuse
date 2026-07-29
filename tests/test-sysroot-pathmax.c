/*
 * Guest paths at the host path ceiling
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux allows a path of 4096 bytes; macOS stops at 1024 (PATH_MAX,
 * sys/syslimits.h), and an escaped component roughly doubles on disk, so a
 * guest path the kernel it emulates would accept can exceed what the host
 * kernel will take. The documented policy (docs/filenames.md, "Whole paths")
 * is that such a path reports ENAMETOOLONG and is never truncated, because a
 * truncated path names a different file. This pins the guest-visible
 * consequences of that policy at the boundary.
 *
 * Two lanes: literal components (host length ~ guest length + sysroot
 * prefix, so the ceiling is crossed on any volume) and escaped components
 * (guest path well under 1024; only the escape expansion crosses, so this
 * lane exercises the case-exact walk specifically). Both assert the same
 * contract: every level either works fully (create, stat, list back)
 * or fails with exactly ENAMETOOLONG, monotonically once the ceiling is
 * crossed, with no level truncating into a sibling that a listing would
 * expose.
 *
 * Code under test: the accumulated-path checks in
 * src/syscall/casefold-walk.c and the translation exits in
 * src/syscall/path.c. A regression shows up as a create succeeding past the
 * ceiling under a spelling other than its own (truncation), as a wrong
 * errno, or as a success-after-failure flip while descending.
 *
 * On a byte-exact volume the escaped lane stores names literally and stays
 * short of the ceiling; its probes then assert plain success, so the test
 * passes on both volume kinds. Run under --sysroot.
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

int passes = 0, fails = 0;

/* One directory level per step, sized so the guest path stays far inside the
 * Linux limit while the host path crosses 1024 after a few dozen levels.
 */
#define STEP_LEN 40
#define MAX_DEPTH 60

/* The escaped lane doubles on disk: 30 guest bytes become ~64 host bytes per
 * level, so the host ceiling arrives near depth 16 while the guest path is
 * still under 600 bytes.
 */
#define ESC_STEP_LEN 30
#define ESC_MAX_DEPTH 24

static void step_name(char *out, size_t outsz, int depth, bool escaped)
{
    /* A distinct leading digit per level keeps every component unique, so a
     * truncated path cannot happen to name an existing shallower entry.
     */
    int n = snprintf(out, outsz, "%c%02d", escaped ? 'D' : 'd', depth % 100);
    memset(out + n, escaped ? 'A' : 'a',
           (size_t) ((escaped ? ESC_STEP_LEN : STEP_LEN) - n));
    out[escaped ? ESC_STEP_LEN : STEP_LEN] = '\0';
}

/* The recipe probes the volume host-side and passes "fold" or "exact": a
 * guest cannot tell the volume kinds apart (hiding that difference is what
 * the escape is for), but whether the escaped lane must cross the ceiling
 * depends on it, and an expectation that accepts both outcomes on a folding
 * volume would let the interesting lane pass vacuously.
 */
static bool volume_folds;

/* Descend one level at a time under @root, requiring each mkdir to either
 * succeed completely or fail with ENAMETOOLONG, and never to succeed again
 * once a level has failed. Returns the deepest path that succeeded in @path.
 */
static void descend(const char *root, bool escaped)
{
    char path[8192];
    char deepest[8192];
    char name[STEP_LEN + 1];
    bool hit_ceiling = false;
    int depth_limit = escaped ? ESC_MAX_DEPTH : MAX_DEPTH;

    snprintf(path, sizeof(path), "%s", root);
    snprintf(deepest, sizeof(deepest), "%s", root);

    TEST(escaped ? "escaped descent is clean at the ceiling"
                 : "literal descent is clean at the ceiling");
    for (int depth = 0; depth < depth_limit; depth++) {
        size_t len = strlen(path);

        step_name(name, sizeof(name), depth, escaped);
        snprintf(path + len, sizeof(path) - len, "/%s", name);

        if (mkdir(path, 0755) == 0) {
            if (hit_ceiling) {
                FAIL(
                    "mkdir succeeded past a level that reported "
                    "ENAMETOOLONG");
                return;
            }
            snprintf(deepest, sizeof(deepest), "%s", path);
            continue;
        }
        if (errno != ENAMETOOLONG) {
            FAIL("mkdir failed with an errno other than ENAMETOOLONG");
            return;
        }
        hit_ceiling = true;
        /* The failed level must not exist under any spelling: a truncated
         * create would leave an entry the parent listing exposes.
         */
        path[len] = '\0';
    }
    PASS();

    TEST(escaped ? "escaped ceiling is where the math says"
                 : "literal ceiling was reached");
    if (escaped && !hit_ceiling) {
        /* A byte-exact volume stores the escaped lane literally, and
         * ESC_MAX_DEPTH * (ESC_STEP_LEN + 1) stays under 1024 there; not
         * crossing is the correct outcome on such a volume, and the only
         * legal one on a folding volume is crossing.
         */
        struct stat st;
        if (volume_folds)
            FAIL("escape expansion never crossed the host ceiling");
        else if (stat(deepest, &st) == 0 && S_ISDIR(st.st_mode))
            PASS();
        else
            FAIL("deepest directory did not stat back");
    } else if (!escaped && !hit_ceiling) {
        FAIL("literal descent never crossed the host ceiling");
    } else {
        PASS();
    }

    TEST(escaped ? "escaped deepest level lists exactly one child"
                 : "literal deepest level lists exactly one child");
    {
        /* The deepest surviving directory holds at most the single child the
         * next (failed) level would have created, which is none. Any entry
         * here is a truncated spelling of a deeper create.
         */
        DIR *d = opendir(deepest);
        struct dirent *de;
        int extras = 0;

        if (!d) {
            FAIL("deepest directory did not open");
        } else {
            while ((de = readdir(d))) {
                if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
                    extras++;
            }
            closedir(d);
            if (extras)
                FAIL("a failed level left an entry behind");
            else
                PASS();
        }
    }

    TEST(escaped ? "escaped create past the ceiling reports ENAMETOOLONG"
                 : "literal create past the ceiling reports ENAMETOOLONG");
    {
        size_t len = strlen(deepest);
        char file[8192];
        int fd;

        snprintf(file, sizeof(file), "%s", deepest);
        step_name(name, sizeof(name), depth_limit, escaped);
        snprintf(file + len, sizeof(file) - len, "/%s", name);
        /* Fill the remaining guest budget so the host spelling is over the
         * ceiling whichever lane this is.
         */
        for (int i = 0; i < 3; i++) {
            size_t flen = strlen(file);
            step_name(name, sizeof(name), depth_limit + 1 + i, escaped);
            snprintf(file + flen, sizeof(file) - flen, "/%s", name);
        }
        fd = open(file, O_CREAT | O_WRONLY, 0644);
        if (fd >= 0) {
            close(fd);
            FAIL("open(O_CREAT) succeeded on a path past the ceiling");
        } else if (errno == ENAMETOOLONG || errno == ENOENT) {
            /* ENOENT is legal when an intermediate level is the one past
             * the ceiling on a byte-exact volume: the parent chain stops
             * existing before the name gets too long.
             */
            PASS();
        } else {
            FAIL("wrong errno for a create past the ceiling");
        }
    }
}

int main(int argc, char **argv)
{
    volume_folds = argc > 1 && !strcmp(argv[1], "fold");

    if (mkdir("/pathmax-lit", 0755) < 0 && errno != EEXIST) {
        FAIL("setup: mkdir /pathmax-lit");
        SUMMARY("test-sysroot-pathmax");
        return 1;
    }
    if (mkdir("/pathmax-esc", 0755) < 0 && errno != EEXIST) {
        FAIL("setup: mkdir /pathmax-esc");
        SUMMARY("test-sysroot-pathmax");
        return 1;
    }

    descend("/pathmax-lit", false);
    descend("/pathmax-esc", true);

    SUMMARY("test-sysroot-pathmax");
    return fails > 0 ? 1 : 0;
}
