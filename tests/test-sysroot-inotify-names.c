/*
 * inotify names under a case-fold sysroot
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A guest name the volume cannot hold as itself is stored escaped, and the
 * inotify emulation reads directory snapshots straight off the volume, so both
 * halves of a watch cross the name boundary: the watched path must be resolved
 * like every other guest path, and the names carried inside IN_CREATE/IN_DELETE
 * events must be the guest's bytes, never the stored spelling.
 *
 * Linux contract pinned: inotify(7). The name field of an event is the filename
 * within the watched directory, as the process would use it. A name the process
 * never wrote and cannot stat is not that.
 *
 * Code under test: sys_inotify_add_watch and dir_snapshot_fd in
 * src/syscall/inotify.c, routing through src/syscall/path.c. A regression shows
 * up as inotify_add_watch reporting ENOENT for a directory the guest can open,
 * or as an event naming an .ef= spelling the guest cannot resolve, which is how
 * a file watcher (editors, build daemons) sees phantom files appear.
 *
 * The emulation pumps its event queue when the descriptor is read, so events
 * are collected by polling a nonblocking fd with read(2), valid against a real
 * kernel too. A pass therefore does not prove poll(2) reports readiness without
 * an intervening read; that gap predates the name handling and is not what this
 * lane pins.
 *
 * Run under --sysroot on a case-folding volume.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test-harness.h"
#include "test-util.h"

int passes = 0, fails = 0;

#define DIR_W "/watch"

/* Collected event names, flattened; the emulation may batch or split reads, so
 * assertions are about membership rather than arrival order.
 */
typedef struct {
    char names[16][NAME_MAX + 1];
    uint32_t masks[16];
    int count;
} events_t;

/* Set as soon as any drained event carries a stored spelling, across the whole
 * run: leaking is a property of the stream, not of one lane. The one exception
 * is the watch on a directory outside the sysroot, where the stored spelling is
 * the correct name; the flag below suspends the check for exactly that section.
 */
static bool escape_leaked;
static bool stored_names_expected;

/* Drain events until @want names arrived or @timeout_ms passed with nothing
 * new. The snapshot diff behind IN_CREATE/IN_DELETE runs when the queue is
 * pumped, so the fd is nonblocking and read in a paced loop rather than
 * poll(2)ed; see the header.
 */
static void drain(int fd, events_t *ev, int want, int timeout_ms)
{
    char buf[2048];
    int waited_ms = 0;

    while (ev->count < want) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            if (waited_ms >= timeout_ms)
                return;
            usleep(50 * 1000);
            waited_ms += 50;
            continue;
        }
        for (ssize_t off = 0; off < n;) {
            struct inotify_event *e = (struct inotify_event *) (buf + off);
            if (e->len > 0 && ev->count < 16) {
                snprintf(ev->names[ev->count], sizeof(ev->names[0]), "%s",
                         e->name);
                ev->masks[ev->count] = e->mask;
                if (!stored_names_expected && !strncmp(e->name, ".ef=", 4))
                    escape_leaked = true;
                ev->count++;
            }
            off += (ssize_t) (sizeof(*e) + e->len);
        }
    }
}

static bool saw(const events_t *ev, uint32_t mask, const char *name)
{
    for (int i = 0; i < ev->count; i++)
        if ((ev->masks[i] & mask) && !strcmp(ev->names[i], name))
            return true;
    return false;
}

int main(int argc, char **argv)
{
    char path[PATH_MAX];
    events_t ev;
    int fd, wd;

    printf("test-sysroot-inotify-names: guest names in inotify events\n");

    TEST("fixture mkdir");
    EXPECT_TRUE(mkdir(DIR_W, 0755) == 0 || errno == EEXIST, "mkdir");

    fd = inotify_init1(IN_NONBLOCK);
    TEST("inotify_init1");
    EXPECT_TRUE(fd >= 0, "inotify_init1");

    /* The watched path is a guest path: it exists in the sysroot and nowhere
     * else, so a watch that bypasses translation opens the host's namespace and
     * reports ENOENT for a directory the guest can chdir into.
     */
    TEST("a watch on an absolute sysroot directory can be added");
    wd = inotify_add_watch(fd, DIR_W, IN_CREATE | IN_DELETE);
    EXPECT_TRUE(wd >= 0, "inotify_add_watch");

    /* The event name is the guest's spelling. On this volume the file below is
     * stored escaped, so an undecoded snapshot diff would name .ef=..., bytes
     * the guest never wrote and cannot stat.
     */
    TEST("IN_CREATE carries the guest name for an escaped file");
    snprintf(path, sizeof(path), "%s/MixedCase.txt", DIR_W);
    memset(&ev, 0, sizeof(ev));
    if (wd < 0) {
        FAIL("no watch");
    } else if (file_write(path, "x") != 0) {
        FAIL("create");
    } else {
        drain(fd, &ev, 1, 2000);
        EXPECT_TRUE(saw(&ev, IN_CREATE, "MixedCase.txt"),
                    "expected IN_CREATE MixedCase.txt");
    }

    TEST("IN_DELETE carries the guest name for an escaped file");
    memset(&ev, 0, sizeof(ev));
    if (wd < 0) {
        FAIL("no watch");
    } else if (unlink(path) != 0) {
        FAIL("unlink");
    } else {
        drain(fd, &ev, 1, 2000);
        EXPECT_TRUE(saw(&ev, IN_DELETE, "MixedCase.txt"),
                    "expected IN_DELETE MixedCase.txt");
    }

    /* Names differing only by case are distinct files, and their events must be
     * distinct too: one stored literally, one escaped, both reported under the
     * bytes the guest used.
     */
    TEST("a colliding pair produces two distinctly named events");
    memset(&ev, 0, sizeof(ev));
    if (wd < 0) {
        FAIL("no watch");
    } else {
        char lower[PATH_MAX], upper[PATH_MAX];
        snprintf(lower, sizeof(lower), "%s/file", DIR_W);
        snprintf(upper, sizeof(upper), "%s/File", DIR_W);
        if (file_write(lower, "l") != 0 || file_write(upper, "u") != 0) {
            FAIL("create pair");
        } else {
            drain(fd, &ev, 2, 2000);
            EXPECT_TRUE(
                saw(&ev, IN_CREATE, "file") && saw(&ev, IN_CREATE, "File"),
                "expected IN_CREATE for both spellings");
        }
    }

    /* A directory whose own name is stored escaped is still watchable by its
     * guest name, and events inside it decode the same way.
     */
    TEST("a watch on an escaped directory can be added");
    snprintf(path, sizeof(path), "%s/CaseDir", DIR_W);
    {
        int wd2 = -1;
        EXPECT_TRUE(
            mkdir(path, 0755) == 0 &&
                (wd2 = inotify_add_watch(fd, path, IN_CREATE | IN_DELETE)) >= 0,
            "mkdir + add_watch");

        TEST("  and events inside it carry guest names");
        memset(&ev, 0, sizeof(ev));
        if (wd2 < 0) {
            FAIL("no watch");
        } else {
            snprintf(path, sizeof(path), "%s/CaseDir/Left.Behind", DIR_W);
            if (file_write(path, "keep") != 0) {
                FAIL("create");
            } else {
                drain(fd, &ev, 1, 2000);
                EXPECT_TRUE(saw(&ev, IN_CREATE, "Left.Behind"),
                            "expected IN_CREATE Left.Behind");
            }
        }
    }

    /* A relative watch resolves against the guest cwd, which sits inside the
     * sysroot; the dirfd-relative leg of translation must land it on the same
     * directory the absolute spelling names.
     */
    TEST("a relative watch reaches the same directory");
    memset(&ev, 0, sizeof(ev));
    {
        int wd3;
        if (chdir(DIR_W) != 0 ||
            (wd3 = inotify_add_watch(fd, ".", IN_CREATE | IN_DELETE)) < 0) {
            FAIL("chdir + add_watch");
        } else if (file_write("Rel.Made", "r") != 0) {
            FAIL("create");
        } else {
            drain(fd, &ev, 1, 2000);
            EXPECT_TRUE(saw(&ev, IN_CREATE, "Rel.Made"),
                        "expected IN_CREATE Rel.Made");
        }
    }

    /* A watch is refused only for an object kqueue cannot observe: a FUSE node
     * or a synthetic /proc file, both of which elfuse answers itself with no
     * host vnode behind them. /dev/shm is neither (the leaf is redirected to a
     * real host file that kqueue watches like any other), so refusing it would
     * deny a watch Linux grants on tmpfs. The same over-refusal reaches every
     * path the open-intercept prefilter merely *might* claim: /etc/passwd with
     * no sysroot copy, /sys/devices/system/cpu, and, because that filter
     * compares four bytes, any name beginning "/dev".
     */
    TEST("a watch on a /dev/shm leaf is granted, not refused");
    {
        int shm = open("/dev/shm/inotify-probe", O_CREAT | O_RDWR, 0600);
        int wd4 = -1;

        if (shm < 0) {
            FAIL("open /dev/shm leaf");
        } else {
            wd4 = inotify_add_watch(fd, "/dev/shm/inotify-probe", IN_ATTRIB);
            EXPECT_TRUE(wd4 >= 0, "inotify_add_watch on a /dev/shm leaf");
            close(shm);
            unlink("/dev/shm/inotify-probe");
        }
    }

    /* The other half of that rule. A real shm leaf is watchable, but the guest
     * can also write a symlink into the backing directory, and following one
     * leads wherever its target names. is_guest_system_path() keeps /etc out of
     * reach precisely so a guest cannot address the host's copy, and a watch on
     * it hands back that file's existence and every change to it. Every other
     * consumer of a shm leaf already refuses to follow: stat reports the link
     * itself and open reports ELOOP. Watching was the one that followed, so a
     * guest could observe any host path it could name as a link target.
     */
    TEST("a watch does not follow a shm symlink out of the backing dir");
    {
        unlink("/dev/shm/inotify-escape");
        if (symlink("/etc/passwd", "/dev/shm/inotify-escape") != 0) {
            FAIL("symlink into /dev/shm");
        } else {
            int wd5;

            errno = 0;
            wd5 = inotify_add_watch(fd, "/dev/shm/inotify-escape", IN_ATTRIB);
            if (wd5 >= 0) {
                inotify_rm_watch(fd, wd5);
                FAIL("watched a host file through a shm symlink");
            } else {
                EXPECT_ERRNO(wd5, ELOOP, "should refuse to follow the link");
            }
            unlink("/dev/shm/inotify-escape");
        }
    }

    /* Decoding is scoped the way listings are: a watch on a host directory the
     * sysroot does not own carries entry names as stored, because those names
     * are the host's and an escape-shaped literal there means itself. A
     * decoding snapshot would name a file the guest cannot stat in that
     * directory, and would misreport a genuine literal ".ef=" file the guest
     * itself created there.
     */
    TEST("an outside-sysroot watch reports names as stored");
    if (argc < 2) {
        FAIL("no outside directory given");
    } else {
        int wd6;

        memset(&ev, 0, sizeof(ev));
        stored_names_expected = true;
        if ((wd6 = inotify_add_watch(fd, argv[1], IN_CREATE | IN_DELETE)) < 0) {
            FAIL("add_watch outside the sysroot");
        } else {
            snprintf(path, sizeof(path), "%s/.ef=424152", argv[1]);
            int outfd = open(path, O_CREAT | O_WRONLY, 0644);
            if (outfd < 0) {
                FAIL("create outside literal");
            } else {
                close(outfd);
                drain(fd, &ev, 1, 2000);
                EXPECT_TRUE(saw(&ev, IN_CREATE, ".ef=424152") &&
                                !saw(&ev, IN_CREATE, "BAR"),
                            "expected IN_CREATE under the literal bytes");

                TEST("  and IN_DELETE names the same literal");
                memset(&ev, 0, sizeof(ev));
                if (unlink(path) != 0) {
                    FAIL("unlink outside literal");
                } else {
                    drain(fd, &ev, 1, 2000);
                    EXPECT_TRUE(saw(&ev, IN_DELETE, ".ef=424152") &&
                                    !saw(&ev, IN_DELETE, "BAR"),
                                "expected IN_DELETE under the literal bytes");
                }
            }
            inotify_rm_watch(fd, wd6);
        }
        stored_names_expected = false;
    }

    /* The catch-all: whatever arrived above, nothing may look like a stored
     * spelling. This is what fails first when the snapshot diff stops decoding.
     */
    TEST("no event name was escape-shaped");
    EXPECT_TRUE(!escape_leaked, "an event leaked an .ef= spelling");

    if (fd >= 0)
        close(fd);

    SUMMARY("test-sysroot-inotify-names");
    return fails > 0 ? 1 : 0;
}
