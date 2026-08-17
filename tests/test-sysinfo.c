/*
 * Test process/system info syscalls (Batch 2)
 *
 * Copyright 2026 elfuse contributors
 * Copyright 2025 Moritz Angermann, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests: sysinfo, getrusage, getgroups, prlimit64, getppid, sched_getaffinity
 */

#include <string.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <sys/resource.h>
#include <sched.h>

#include "test-harness.h"

int main(void)
{
    int passes = 0, fails = 0;

    printf("test-sysinfo: Batch 2 process/system info tests\n");

    /* Test sysinfo */
    TEST("sysinfo");
    {
        struct sysinfo si;
        if (sysinfo(&si) == 0) {
            EXPECT_TRUE(si.uptime > 0 && si.totalram > 0 && si.mem_unit > 0,
                        "invalid values");
        } else
            FAIL("sysinfo failed");
    }

    /* sysinfo(2) and /proc/meminfo must describe the same machine.
     *
     * A guest that reads both subtracts one against the other: busybox free
     * takes total and free from sysinfo and buff/cache from /proc/meminfo, then
     * computes used = total - free - cached. While elfuse capped sysinfo's
     * total at a hardcoded 4GiB but let /proc/meminfo report the host's real
     * memory, Cached exceeded the sysinfo total and that subtraction wrapped,
     * printing a 24-digit "used". The wrap happens in the guest, past any
     * saturating arithmetic elfuse does on its own side, so the surfaces have
     * to agree here.
     *
     * The second assertion is the one busybox actually performs, so it is
     * stated in sysinfo's terms rather than MemFree's: freeram is what gets
     * subtracted, and it is a different read from the MemFree in this file.
     * Their exact equality is deliberately not asserted -- under a real kernel
     * the two reads are separate samples of a moving figure, and elfuse only
     * promises they come from one snapshot, not that no snapshot ever changes.
     *
     * Host-dependent by nature: the totals are whatever the machine has, so the
     * test asserts their agreement rather than any particular figure. Under a
     * real kernel both come from one si_meminfo() and agree by construction,
     * which is what the test-matrix column checks. Under the pre-fix elfuse the
     * first assertion fails on any host with more than 4GiB of RAM, and passes
     * vacuously below that.
     */
    TEST("sysinfo agrees with /proc/meminfo");
    {
        struct sysinfo si;
        if (sysinfo(&si) != 0)
            FAIL("sysinfo failed");
        else {
            FILE *f = fopen("/proc/meminfo", "r");
            if (!f)
                FAIL("could not open /proc/meminfo");
            else {
                unsigned long long total_kb = 0, free_kb = 0, cached_kb = 0;
                int seen = 0;
                char line[256];
                while (fgets(line, sizeof(line), f)) {
                    if (sscanf(line, "MemTotal: %llu kB", &total_kb) == 1)
                        seen |= 1;
                    else if (sscanf(line, "MemFree: %llu kB", &free_kb) == 1)
                        seen |= 2;
                    else if (sscanf(line, "Cached: %llu kB", &cached_kb) == 1)
                        seen |= 4;
                }
                fclose(f);

                unsigned long long si_total_kb =
                    (unsigned long long) si.totalram * si.mem_unit / 1024;
                unsigned long long si_free_kb =
                    (unsigned long long) si.freeram * si.mem_unit / 1024;

                if (seen != 7)
                    FAIL("MemTotal/MemFree/Cached missing from /proc/meminfo");
                else
                    EXPECT_TRUE(si_total_kb == total_kb &&
                                    si_free_kb + cached_kb <= si_total_kb &&
                                    free_kb + cached_kb <= total_kb,
                                "sysinfo and /proc/meminfo disagree");
            }
        }
    }

    /* Test getrusage */
    TEST("getrusage");
    {
        struct rusage usage;
        if (getrusage(RUSAGE_SELF, &usage) == 0) {
            EXPECT_TRUE(usage.ru_maxrss >= 0, "invalid maxrss");
        } else
            FAIL("getrusage failed");
    }

    /* Test getgroups */
    TEST("getgroups");
    {
        int ngroups = getgroups(0, NULL);
        if (ngroups >= 0) {
            /* Verify the test can also read the actual groups */
            if (ngroups > 0) {
                gid_t groups[64];
                int n = getgroups(ngroups > 64 ? 64 : ngroups, groups);
                EXPECT_TRUE(n >= 0, "getgroups read failed");
            } else
                PASS();
        } else
            FAIL("getgroups count failed");
    }

    /* Test prlimit64 (via getrlimit/setrlimit) */
    TEST("prlimit64 (RLIMIT_NOFILE)");
    {
        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
            EXPECT_TRUE(rl.rlim_cur > 0, "invalid rlim_cur");
        } else
            FAIL("getrlimit failed");
    }

    /* Test getppid */
    TEST("getppid");
    {
        pid_t ppid = getppid();
        /* In the current single-process model, ppid should be 0 */
        EXPECT_TRUE(ppid >= 0, "getppid returned negative");
    }

    /* Test sched_getaffinity */
    TEST("sched_getaffinity");
    {
        cpu_set_t set;
        CPU_ZERO(&set);
        if (sched_getaffinity(0, sizeof(set), &set) == 0) {
            /* Should have at least 1 CPU */
            int count = CPU_COUNT(&set);
            EXPECT_TRUE(count >= 1, "no CPUs in affinity mask");
        } else
            FAIL("sched_getaffinity failed");
    }

    /* Test getpid / gettid */
    TEST("getpid");
    {
        pid_t pid = getpid();
        EXPECT_TRUE(pid > 0, "invalid pid");
    }

    SUMMARY("test-sysinfo");
    return fails > 0 ? 1 : 0;
}
