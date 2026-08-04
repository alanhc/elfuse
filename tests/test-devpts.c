/* devpts identification and Unix98 pty allocation.
 *
 * glibc's posix_openpt() opens /dev/ptmx and then confirms devpts is mounted
 * before handing the master back (sysdeps/unix/sysv/linux/getpt.c). If statfs
 * does not identify /dev/pts as devpts, glibc closes the master it just opened
 * and every glibc pty consumer fails -- terminal emulators, script(1), expect,
 * tmux, screen, sshd, anything using openpty()/forkpty().
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <termios.h>
#include <unistd.h>

#define DEVPTS_SUPER_MAGIC 0x1cd1

/* Fail loudly instead of hanging if the pty never delivers the payload. */
static void on_alarm(int sig)
{
    (void) sig;
    static const char msg[] = "\ntest-devpts: TIMEOUT waiting on pty read\n";
    ssize_t ignored = write(2, msg, sizeof(msg) - 1);
    (void) ignored;
    _exit(1);
}

int main(void)
{
    int failures = 0;

    /* 1. statfs(/dev/pts) must report devpts -- the check glibc performs. */
    printf("test-devpts: 1. statfs(/dev/pts) reports devpts... ");
    struct statfs pts_fs;
    if (statfs("/dev/pts", &pts_fs) != 0) {
        printf("FAIL (statfs: %m)\n");
        failures++;
    } else if ((unsigned long) pts_fs.f_type != DEVPTS_SUPER_MAGIC) {
        printf("FAIL (f_type=0x%lx, want 0x%x)\n",
               (unsigned long) pts_fs.f_type, DEVPTS_SUPER_MAGIC);
        failures++;
    } else {
        printf("PASS (f_type=0x%lx)\n", (unsigned long) pts_fs.f_type);
    }

    /* 2. posix_openpt() must hand back a usable master. */
    printf("test-devpts: 2. posix_openpt()... ");
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        printf("FAIL (%m)\n");
        failures++;
        /* Everything below needs the master; report what we have. */
        printf("test-devpts: %s\n", failures ? "FAILED" : "all tests passed");
        return failures != 0;
    }
    printf("PASS (fd=%d)\n", master);

    /* No assertion on fstatfs(master): Linux answers from whatever filesystem
     * provides /dev/ptmx, which is devpts only when it is the bind-mounted
     * /dev/pts/ptmx and tmpfs or devtmpfs otherwise. There is no portable
     * value to expect.
     *
     * The slave fd is not ambiguous that way -- on Linux it really does live on
     * devpts -- and elfuse still does not answer devpts for it. That is left
     * alone knowingly: sys_fstatfs passes the macOS f_type through untranslated
     * for every filesystem, so a guest sniffing fs types sees macOS mount-type
     * indices whatever it opens, and fixing the slave alone would be one more
     * spot patch on the mapping gap that is the other half of the issue this
     * change addresses. grantpt(3) is unaffected: the fd it checks is the
     * master.
     */

    /* 3. The rest of the Unix98 sequence must work on that master. */
    printf("test-devpts: 3. grantpt/unlockpt/ptsname... ");
    char slave_name[256];
    int seq_ok = 1;
    if (grantpt(master) != 0) {
        printf("FAIL (grantpt: %m)\n");
        seq_ok = 0;
    } else if (unlockpt(master) != 0) {
        printf("FAIL (unlockpt: %m)\n");
        seq_ok = 0;
    } else if (ptsname_r(master, slave_name, sizeof(slave_name)) != 0) {
        printf("FAIL (ptsname_r: %m)\n");
        seq_ok = 0;
    } else {
        printf("PASS (%s)\n", slave_name);
    }
    if (!seq_ok)
        failures++;

    /* 4. Data must survive a round trip through the pair. */
    printf("test-devpts: 4. master/slave round trip... ");
    if (!seq_ok) {
        printf("SKIP (no slave name)\n");
    } else {
        int slave = open(slave_name, O_RDWR | O_NOCTTY);
        if (slave < 0) {
            printf("FAIL (open slave: %m)\n");
            failures++;
        } else {
            /* Raw mode: in canonical mode the slave read would block until a
             * newline arrives, and ECHO would race the payload back at the
             * master. Both would hang the run rather than fail it, so a
             * termios failure has to stop here -- carrying on would spend the
             * whole alarm window below only to report a round-trip failure
             * that is really a setup failure.
             */
            struct termios tio;
            if (tcgetattr(slave, &tio) != 0) {
                printf("FAIL (tcgetattr on slave: %m)\n");
                failures++;
                close(slave);
                goto round_trip_done;
            }
            cfmakeraw(&tio);
            if (tcsetattr(slave, TCSANOW, &tio) != 0) {
                printf("FAIL (tcsetattr raw mode: %m)\n");
                failures++;
                close(slave);
                goto round_trip_done;
            }
            /* Belt and braces: a stuck read must fail the test, not wedge CI.
             */
            signal(SIGALRM, on_alarm);
            alarm(10);

            static const char msg[] = "devpts";
            const size_t want = sizeof(msg) - 1;
            char buf[sizeof(msg)];
            memset(buf, 0, sizeof(buf));

            /* A pty may transfer fewer bytes per call than asked for, in
             * either direction, so drive both to completion instead of
             * assuming one call moves the whole payload -- a legal short read
             * would otherwise fail a working pair. The alarm above bounds the
             * exchange, so a stall still fails rather than looping forever.
             */
            size_t sent = 0;
            while (sent < want) {
                ssize_t n = write(master, msg + sent, want - sent);
                if (n <= 0)
                    break;
                sent += (size_t) n;
            }
            size_t got = 0;
            while (sent == want && got < want) {
                ssize_t n = read(slave, buf + got, want - got);
                if (n <= 0)
                    break;
                got += (size_t) n;
            }
            alarm(0);
            if (sent != want) {
                printf("FAIL (wrote %zu of %zu: %m)\n", sent, want);
                failures++;
            } else if (got != want || memcmp(buf, msg, want) != 0) {
                printf("FAIL (read %zu of %zu, buf='%s')\n", got, want, buf);
                failures++;
            } else {
                printf("PASS\n");
            }
            close(slave);
        }
    round_trip_done:;
    }

    /* 5. An unallocated or malformed slave must not claim devpts: a prefix
     * match on /dev/pts would make these succeed, where Linux reports ENOENT.
     */
    printf("test-devpts: 5. bogus slave paths are ENOENT... ");
    {
        /* The last four are aliases of a live slave that strtoul would take:
         * leading zeros, a sign, leading whitespace. Linux answers ENOENT for
         * every one of them, and accepting any would let one slave answer
         * under several names -- for stat, for devpts identity, and for
         * whether chmod and chown are intercepted at all.
         */
        char alias_zero[64], alias_plus[64], alias_space[64];
        snprintf(alias_zero, sizeof(alias_zero), "/dev/pts/0%s",
                 seq_ok ? slave_name + 9 : "0");
        snprintf(alias_plus, sizeof(alias_plus), "/dev/pts/+%s",
                 seq_ok ? slave_name + 9 : "0");
        snprintf(alias_space, sizeof(alias_space), "/dev/pts/ %s",
                 seq_ok ? slave_name + 9 : "0");
        const char *const bogus[] = {"/dev/pts/99999", "/dev/pts/bogus",
                                     "/dev/pts/1x",    alias_zero,
                                     alias_plus,       alias_space};
        int bad = 0;
        for (size_t i = 0; i < sizeof(bogus) / sizeof(bogus[0]); i++) {
            struct statfs b;
            errno = 0;
            if (statfs(bogus[i], &b) == 0) {
                printf("FAIL (%s resolved, f_type=0x%lx) ", bogus[i],
                       (unsigned long) b.f_type);
                bad++;
            } else if (errno != ENOENT) {
                /* Failing is not enough: EACCES or ENOTDIR would mean the
                 * lookup went wrong somewhere else rather than the slave
                 * simply not existing.
                 */
                printf("FAIL (%s errno=%d (%s), want ENOENT) ", bogus[i], errno,
                       strerror(errno));
                bad++;
            }
        }
        if (bad)
            failures++;
        else
            printf("PASS\n");
    }

    /* 6. chown must not claim to have given the slave away. Keeping the
     * reported owner is what grantpt(3) asks for and must succeed; handing it
     * to another uid is refused rather than silently discarded.
     */
    printf("test-devpts: 6. chown semantics on the slave... ");
    if (!seq_ok) {
        printf("SKIP (no slave name)\n");
    } else {
        struct stat st;
        if (stat(slave_name, &st) != 0) {
            printf("FAIL (stat slave: %m)\n");
            failures++;
        } else if (chown(slave_name, st.st_uid, st.st_gid) != 0) {
            printf("FAIL (no-op chown rejected: %m)\n");
            failures++;
        } else if (chown(slave_name, (uid_t) -1, (gid_t) -1) != 0) {
            printf("FAIL (unchanged chown rejected: %m)\n");
            failures++;
        } else {
            /* Giving the slave to another uid must not silently "succeed". */
            uid_t other = st.st_uid == 0 ? 12345 : 0;
            int rc = chown(slave_name, other, (gid_t) -1);
            struct stat after;
            if (rc == 0 && stat(slave_name, &after) == 0 &&
                after.st_uid != other) {
                printf("FAIL (chown reported success but owner unchanged)\n");
                failures++;
            } else {
                printf("PASS\n");
            }
        }
    }

    /* 7. chmod on the slave must succeed. This is the branch grantpt(3) needs
     * on glibc: it derives the mode it wants from getgrnam("tty"), and on a
     * macOS host that group is gid 4 while the synthesized slave reports gid 5
     * (PTY_SLAVE_TTY_GID, what Linux devpts uses), so the group never matches,
     * the wanted mode comes out 0600 against the synthesized 0620, and grantpt
     * chmods every time rather than accepting the mode as-is. Passing that
     * through to the host would fail ENOENT and send grantpt to pt_chown.
     *
     * Succeeding is the whole portable contract, so the follow-up stat accepts
     * two answers. A real kernel keeps the new mode and reports 0600. elfuse
     * accepts the request without retaining it -- keeping it would need
     * per-slave state that also has to cross the fork-IPC boundary -- and goes
     * on reporting the synthesized 0620. Anything else means the chmod landed
     * somewhere it should not have.
     */
    printf("test-devpts: 7. chmod on the slave is accepted... ");
    if (!seq_ok) {
        printf("SKIP (no slave name)\n");
    } else if (chmod(slave_name, 0600) != 0) {
        printf("FAIL (chmod rejected: %m)\n");
        failures++;
    } else {
        struct stat after;
        unsigned mode = 0;
        if (stat(slave_name, &after) != 0) {
            printf("FAIL (stat after chmod: %m)\n");
            failures++;
        } else if ((mode = after.st_mode & 07777) == 0600) {
            printf("PASS (mode retained 0600)\n");
        } else if (mode == 0620) {
            printf("PASS (accepted, stat still reports 0620)\n");
        } else {
            printf("FAIL (mode=%04o, want 0600 or the synthesized 0620)\n",
                   mode);
            failures++;
        }
    }

    /* 8. Negative control: an ordinary directory must not claim devpts, so a
     * blanket f_type would not pass this file.
     */
    printf("test-devpts: 8. ordinary path is not devpts... ");
    struct statfs root_fs;
    if (statfs("/", &root_fs) != 0) {
        printf("FAIL (statfs /: %m)\n");
        failures++;
    } else if ((unsigned long) root_fs.f_type == DEVPTS_SUPER_MAGIC) {
        printf("FAIL (/ reports devpts)\n");
        failures++;
    } else {
        printf("PASS (f_type=0x%lx)\n", (unsigned long) root_fs.f_type);
    }

    close(master);

    if (failures == 0)
        printf("test-devpts: all tests passed -- PASS\n");
    else
        printf("test-devpts: %d failed\n", failures);
    return failures != 0;
}
