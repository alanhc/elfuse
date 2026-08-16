/*
 * process_vm_readv/process_vm_writev tests
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Syscalls exercised: process_vm_readv(270), process_vm_writev(271)
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#include "test-harness.h"
#include "raw-syscall.h"

int passes = 0, fails = 0;

#ifndef SYS_process_vm_readv
#define SYS_process_vm_readv 270
#endif

#ifndef SYS_process_vm_writev
#define SYS_process_vm_writev 271
#endif

static long raw_process_vm_readv(pid_t pid,
                                 const struct iovec *local_iov,
                                 unsigned long local_iovcnt,
                                 const struct iovec *remote_iov,
                                 unsigned long remote_iovcnt,
                                 unsigned long flags)
{
    return raw_syscall6(SYS_process_vm_readv, (long) pid, (long) local_iov,
                        (long) local_iovcnt, (long) remote_iov,
                        (long) remote_iovcnt, (long) flags);
}

static long raw_process_vm_writev(pid_t pid,
                                  const struct iovec *local_iov,
                                  unsigned long local_iovcnt,
                                  const struct iovec *remote_iov,
                                  unsigned long remote_iovcnt,
                                  unsigned long flags)
{
    return raw_syscall6(SYS_process_vm_writev, (long) pid, (long) local_iov,
                        (long) local_iovcnt, (long) remote_iov,
                        (long) remote_iovcnt, (long) flags);
}

static void test_readv_scatter(void)
{
    TEST("process_vm_readv scatter");

    char remote[] = "hello-world";
    char a[6] = {0};
    char b[6] = {0};
    struct iovec local[2] = {
        {.iov_base = a, .iov_len = 5},
        {.iov_base = b, .iov_len = 5},
    };
    struct iovec remote_iov[2] = {
        {.iov_base = remote, .iov_len = 5},
        {.iov_base = remote + 6, .iov_len = 5},
    };

    long n = raw_process_vm_readv(getpid(), local, 2, remote_iov, 2, 0);
    EXPECT_TRUE(n == 10 && !strcmp(a, "hello") && !strcmp(b, "world"),
                "scatter read failed");
}

static void test_writev_scatter(void)
{
    TEST("process_vm_writev scatter");

    char src0[] = "ab";
    char src1[] = "cd";
    char remote[] = "....";
    struct iovec local[2] = {
        {.iov_base = src0, .iov_len = 2},
        {.iov_base = src1, .iov_len = 2},
    };
    struct iovec remote_iov[2] = {
        {.iov_base = remote, .iov_len = 2},
        {.iov_base = remote + 2, .iov_len = 2},
    };

    long n = raw_process_vm_writev(getpid(), local, 2, remote_iov, 2, 0);
    EXPECT_TRUE(n == 4 && !memcmp(remote, "abcd", 4), "scatter write failed");
}

static void test_short_copy_on_remote_fault(void)
{
    TEST("process_vm_readv short remote fault");

    char remote[] = "abcd";
    char dst[8] = {0};
    struct iovec local = {.iov_base = dst, .iov_len = sizeof(dst)};
    struct iovec remote_iov[2] = {
        {.iov_base = remote, .iov_len = 4},
        {.iov_base = (void *) 1, .iov_len = 4},
    };

    long n = raw_process_vm_readv(getpid(), &local, 1, remote_iov, 2, 0);
    EXPECT_TRUE(n == 4 && !memcmp(dst, "abcd", 4), "short read fault");
}

static void test_short_copy_on_local_fault(void)
{
    TEST("process_vm_writev short local fault");

    char src[] = "wxyz";
    char remote[8] = {0};
    struct iovec local[2] = {
        {.iov_base = src, .iov_len = 4},
        {.iov_base = (void *) 1, .iov_len = 4},
    };
    struct iovec remote_iov = {.iov_base = remote, .iov_len = sizeof(remote)};

    long n = raw_process_vm_writev(getpid(), local, 2, &remote_iov, 1, 0);
    EXPECT_TRUE(n == 4 && !memcmp(remote, "wxyz", 4), "short write fault");
}

static void test_error_paths(void)
{
    char src[] = "x";
    char dst[] = ".";
    struct iovec src_iov = {.iov_base = src, .iov_len = 1};
    struct iovec dst_iov = {.iov_base = dst, .iov_len = 1};

    TEST("process_vm flags EINVAL");
    EXPECT_RAW_ERRNO(
        raw_process_vm_readv(getpid(), &dst_iov, 1, &src_iov, 1, 1), -EINVAL,
        "flags accepted");

    TEST("process_vm foreign pid ESRCH");
    EXPECT_RAW_ERRNO(
        raw_process_vm_writev(getpid() + 99999, &src_iov, 1, &dst_iov, 1, 0),
        -ESRCH, "foreign pid accepted");

    TEST("process_vm bad iov pointer EFAULT");
    EXPECT_RAW_ERRNO(raw_process_vm_readv(getpid(), (const struct iovec *) 1, 1,
                                          &src_iov, 1, 0),
                     -EFAULT, "bad local iov accepted");

    TEST("process_vm iovcnt EINVAL");
    EXPECT_RAW_ERRNO(
        raw_process_vm_readv(getpid(), &dst_iov, 1025, &src_iov, 1, 0), -EINVAL,
        "oversized iovcnt accepted");

    TEST("process_vm zero count");
    EXPECT_EQ(raw_process_vm_readv(getpid(), NULL, 0, NULL, 0, 0), 0,
              "zero count failed");
}

/* Overlapping ranges within one address space.
 *
 * sys_process_vm accepts the caller's own pid, so both iovecs can resolve into
 * the same guest memory and a guest can make them overlap. The host-side copy
 * has to stay defined there: copying an overlapping range forward is undefined
 * behavior, not merely a strange result, which is why the copy helper uses
 * memmove per step.
 *
 * This does not try to catch that undefined behavior by its output. Measured
 * on this host, the forward copy it replaced produced correct leading bytes and
 * a result that simply differed from memmove, so nothing observable separates
 * the two reliably; the fix rests on the language rule, not on a reproducer.
 *
 * What the case does pin is the part that is specified: the full count comes
 * back and nothing outside the destination is disturbed. The content of the
 * overlap is asserted nowhere, because it is unspecified and the reference
 * kernel matches neither memmove nor a page-wise move.
 */
static void test_overlapping_self_copy(void)
{
    static char buf[16384], original[16384];
    const size_t head = 512, span = 8192;

    for (size_t delta = 1; delta <= 64; delta *= 8) {
        for (size_t i = 0; i < sizeof(buf); i++)
            buf[i] = (char) (i * 7 + delta);
        memcpy(original, buf, sizeof(buf));

        char *dst = buf + head + delta;
        struct iovec dst_iov = {.iov_base = dst, .iov_len = span};
        struct iovec src_iov = {.iov_base = buf + head, .iov_len = span};

        TEST("process_vm_readv overlapping self copy");
        EXPECT_EQ(raw_process_vm_readv(getpid(), &dst_iov, 1, &src_iov, 1, 0),
                  (long) span, "overlapping copy reported short");
        /* head + delta, not head: the destination starts past the delta gap,
         * so those bytes are before it too and a stray write there must fail.
         */
        EXPECT_TRUE(memcmp(buf, original, head + delta) == 0,
                    "overlapping copy wrote before the destination");
        EXPECT_TRUE(memcmp(dst + span, original + head + delta + span,
                           sizeof(buf) - head - delta - span) == 0,
                    "overlapping copy wrote past the destination");
    }
}

int main(void)
{
    printf("test-process-vm: process_vm_readv/writev tests\n\n");

    test_readv_scatter();
    test_writev_scatter();
    test_short_copy_on_remote_fault();
    test_short_copy_on_local_fault();
    test_error_paths();
    test_overlapping_self_copy();

    SUMMARY("test-process-vm");
    return fails > 0 ? 1 : 0;
}
