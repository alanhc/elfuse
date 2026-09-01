/*
 * test-shim-futex-toctou.c -- futex EL1 fault recovery survives concurrent
 * mprotect(PROT_NONE) of the futex word.
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A sibling vCPU revokes the page while the EL1 LDTR reads a mismatched futex
 * word. The recovery path must return EFAULT rather than halting the VM.
 */

#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "raw-syscall.h"

#define PAGE_SIZE 4096
#define ITERATIONS 200000

/* The word is initialized to this and the waits expect 0, so a readable page
 * always answers EAGAIN and the loop never parks.
 */
#define NEVER_EXPECTED 0x11223344

static atomic_int stop;
static void *shared_page;

static void *protect_flipper(void *arg)
{
    (void) arg;
    int prot = PROT_READ | PROT_WRITE;

    while (!atomic_load_explicit(&stop, memory_order_acquire)) {
        prot ^= (PROT_READ | PROT_WRITE);
        if (mprotect(shared_page, PAGE_SIZE, prot) != 0) {
            fprintf(stderr, "mprotect failed: %s\n", strerror(errno));
            return (void *) (uintptr_t) 1;
        }
    }
    /* Leave the page accessible at exit. */
    mprotect(shared_page, PAGE_SIZE, PROT_READ | PROT_WRITE);
    return NULL;
}

int main(void)
{
    pthread_t flipper;
    long eagain = 0, efault = 0, other = 0;
    void *flipper_rc;

    shared_page = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (shared_page == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return 1;
    }
    *(int *) shared_page = NEVER_EXPECTED;

    if (pthread_create(&flipper, NULL, protect_flipper, NULL) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        return 1;
    }

    for (int i = 0; i < ITERATIONS; i++) {
        long rc = raw_futex_wait((int *) shared_page, 0);
        if (rc == -EAGAIN)
            eagain++;
        else if (rc == -EFAULT)
            efault++;
        else {
            if (other == 0)
                fprintf(stderr, "FAIL: unexpected rc %ld at iteration %d\n", rc,
                        i);
            other++;
        }
    }

    atomic_store_explicit(&stop, 1, memory_order_release);
    pthread_join(flipper, &flipper_rc);
    if (flipper_rc != NULL)
        return 1;

    if (other) {
        fprintf(stderr, "FAIL: %ld unexpected returns\n", other);
        return 1;
    }

    /* Both outcomes must appear, or the race never landed and the run proved
     * nothing about the recovery path.
     */
    if (eagain == 0 || efault == 0) {
        fprintf(stderr, "FAIL: race did not land (eagain=%ld efault=%ld)\n",
                eagain, efault);
        return 1;
    }

    printf("OK: futex EL1 fault recovery (eagain=%ld efault=%ld)\n", eagain,
           efault);
    return 0;
}
