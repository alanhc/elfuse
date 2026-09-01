/*
 * test-ptrace-interrupt.c -- what PTRACE_INTERRUPT shows when it lands in the
 * EL1 shim.
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * PTRACE_INTERRUPT kicks the tracee out of hv_vcpu_run. The kick can land while
 * the tracee is inside an EL1 fast path rather than in EL0 code, and there the
 * live registers are the shim's scratch: the guest's own values are in the
 * saved SVC frame and ELR_EL1 holds the EL0 return the shim still means to ERET
 * through. A stop taken from that state reports scratch through
 * PTRACE_GETREGSET and discards what PTRACE_SETREGSET writes, because the shim
 * restores its frame over it on the way out.
 *
 * The tracee here does nothing but calls the shim answers inline: getpid off
 * the identity cache and a 256-byte urandom read off the entropy ring. Neither
 * reaches the host and neither can block, so the thread is only ever in EL0 or
 * inside the shim, which is what gives the kick somewhere useful to land.
 *
 * Two things are checked at every stop. The reported PC has to be an EL0
 * address, not a shim one: the shim lives just under the interpreter base, at
 * 60 GiB on a 36-bit IPA and 1020 GiB on a 40-bit one, and this guest's own
 * text is under 4 GiB. And the tracee keeps a known value in x21, a
 * callee-saved register its loop never writes, so a snapshot of shim scratch
 * shows something else there.
 *
 * This is an elfuse-internal test. PTRACE_SEIZE of a thread in the caller's own
 * thread group fails with EPERM on Linux, so the reference kernel cannot
 * adjudicate the shape, and what is under test is elfuse's own register
 * snapshot rather than a kernel behavior.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "raw-syscall.h"

#define PTRACE_CONT 7
#define PTRACE_GETREGSET 0x4204
#define PTRACE_SETREGSET 0x4205
#define PTRACE_SEIZE 0x4206
#define PTRACE_INTERRUPT 0x4207
#define NT_PRSTATUS 1

#define CLONE_THREAD_FLAGS 0x00050f00 /* VM|FS|FILES|SIGHAND|THREAD */

/* The tracee's loop never writes x21, so anything else there came from outside
 * the guest's own code.
 */
#define CANARY 0x5ec0ffee5ec0ffeeULL

/* Between the highest address this guest maps and the lowest the shim uses, so
 * a PC at or above it can only be shim state.
 *
 * Both bounds matter and the earlier value satisfied only one. 64 GiB sits
 * above the shim rather than below it on the common configuration: a 36-bit IPA
 * host gives a 64 GiB guest whose interpreter base is 60 GiB, with the infra
 * reserve and the shim just beneath that, so no shim PC ever reached the
 * comparison and this assertion could not fail there. It separated the two only
 * on a 40-bit guest, whose interpreter base is 1020 GiB.
 *
 * 16 GiB clears this guest's own mappings, whose text is at 4 MiB and whose
 * mmap allocations start at the 8 GiB RW base, and stays far under the shim on
 * either configuration. A guest that mapped 8 GiB of address space would need
 * this raised, which no test here does.
 */
#define EL0_PC_CEILING 0x400000000ULL

/* Enough interrupts that the kick lands inside the shim repeatedly. A
 * regression does not need all of them: one stop reporting scratch fails.
 */
#define ROUNDS 600

typedef struct {
    uint64_t regs[31];
    uint64_t sp, pc, pstate;
} user_pt_regs_t;

typedef struct {
    void *base;
    unsigned long len;
} iovec_t;

static char tracee_stack[64 * 1024] __attribute__((aligned(16)));
static volatile int tracee_ready;
static volatile int tracee_stop;

/* Only calls the shim answers without ever reaching the host: getpid off the
 * identity cache, and a 256-byte urandom read off the entropy ring, which is
 * the longest EL1 run the shim has that cannot block. Nothing here parks, so
 * the thread is always either in EL0 or inside the shim and a kick has
 * somewhere useful to land.
 */
static int tracee_fn(void)
{
    register uint64_t canary asm("x21") = CANARY;
    unsigned char buf[256];
    int fd = (int) raw_syscall4(56, -100 /* AT_FDCWD */, (long) "/dev/urandom",
                                0 /* O_RDONLY */, 0);

    tracee_ready = 1;
    while (!tracee_stop) {
        raw_getpid();
        if (fd >= 0)
            raw_syscall3(63, fd, (long) buf, sizeof(buf)); /* read */
        raw_getpid();
    }

    /* Keep the register live to the end of the loop, or the compiler is free to
     * stop keeping the value there and the check above becomes a tautology.
     */
    __asm__ volatile("" : : "r"(canary));
    return 0;
}

static long spawn(char *stack_top, int (*fn)(void))
{
    long ret = raw_syscall5(220, CLONE_THREAD_FLAGS, (long) stack_top, 0, 0, 0);
    if (ret == 0) {
        fn();
        raw_syscall1(93, 0); /* exit */
    }
    return ret;
}

static long getregs(long tid, user_pt_regs_t *regs)
{
    iovec_t iov = {regs, sizeof(*regs)};
    return raw_syscall4(117, PTRACE_GETREGSET, tid, NT_PRSTATUS, (long) &iov);
}

int main(void)
{
    long stops = 0, el1_pc = 0, bad_canary = 0;

    long tracee = spawn(tracee_stack + sizeof(tracee_stack), tracee_fn);
    if (tracee < 0) {
        printf("FAIL: clone failed (%ld)\n", tracee);
        return 1;
    }
    while (!tracee_ready)
        raw_syscall0(124); /* sched_yield */

    if (raw_syscall4(117, PTRACE_SEIZE, tracee, 0, 0) != 0) {
        printf("FAIL: PTRACE_SEIZE refused\n");
        return 1;
    }

    for (int r = 0; r < ROUNDS; r++) {
        if (raw_syscall4(117, PTRACE_INTERRUPT, tracee, 0, 0) != 0)
            continue;

        /* WNOHANG with a bounded poll rather than a blocking wait4: a stop that
         * never arrives is the regression this file is about, and it should be
         * reported rather than hung on.
         */
        int status = 0;
        long w = -1;
        for (int spin = 0; spin < 200000; spin++) {
            w = raw_syscall4(260, tracee, (long) &status, 1 /* WNOHANG */, 0);
            if (w > 0)
                break;
            raw_syscall0(124);
        }
        if (w <= 0) {
            printf("FAIL: no ptrace-stop within the poll at round %d\n", r);
            return 1;
        }

        user_pt_regs_t regs;
        memset(&regs, 0, sizeof(regs));
        if (getregs(tracee, &regs) != 0) {
            printf("FAIL: GETREGSET refused at round %d\n", r);
            return 1;
        }

        stops++;
        if (regs.pc >= EL0_PC_CEILING)
            el1_pc++;
        if (regs.regs[21] != CANARY)
            bad_canary++;

        raw_syscall4(117, PTRACE_CONT, tracee, 0, 0);
    }

    tracee_stop = 1;

    if (stops == 0) {
        printf("FAIL: no ptrace-stop was ever taken\n");
        return 1;
    }
    if (el1_pc) {
        printf("FAIL: %ld of %ld stops reported a PC above EL0 (shim state)\n",
               el1_pc, stops);
        return 1;
    }
    if (bad_canary) {
        printf("FAIL: %ld of %ld stops reported x21 as scratch\n", bad_canary,
               stops);
        return 1;
    }

    printf("OK: ptrace-stop reports EL0 state (%ld stops)\n", stops);
    return 0;
}
