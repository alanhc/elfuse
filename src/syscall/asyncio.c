/*
 * Signal-driven I/O (O_ASYNC / SIGIO / SIGURG)
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * See asyncio.h for the design. The watcher owns one kqueue; arm/disarm
 * register EVFILT_READ (and EVFILT_EXCEPT/NOTE_OOB for sockets) as EV_CLEAR
 * edge-triggered knotes so a persistently-ready fd fires once per readiness
 * transition instead of storming signals. kevent() registration from a syscall
 * thread while the watcher blocks in kevent() on the same kqueue is safe on
 * macOS/BSD.
 */

#include "syscall/asyncio.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <unistd.h>

#include "utils.h"

#include "proved/asyncudata.h"

#include "runtime/thread.h"
#include "syscall/linux-wire.h"
#include "syscall/internal.h"
#include "syscall/proc.h"
#include "syscall/signal.h"

static int async_kq = -1;
static pthread_once_t async_once = PTHREAD_ONCE_INIT;

/* fd types that can generate SIGIO. Regular files and directories are always
 * "ready" and never carry ->fasync in Linux, so registering them would storm.
 * FD_FUSE_FILE/FD_FUSE_DIR are omitted for the same reason and because they
 * carry host_fd == -1 (no readiness fd to hand kqueue). Only /dev/fuse itself
 * (FD_FUSE_DEV) has a real host fd worth watching.
 */
static bool async_is_pollable(int fd_type)
{
    switch (fd_type) {
    case FD_SOCKET:
    case FD_PIPE:
    case FD_STDIO:
    case FD_FUSE_DEV:
        return true;
    default:
        return false;
    }
}

/* True when the fd owner names this guest process (its pid, its process group,
 * or one of its live threads). elfuse runs one guest process per host process,
 * so a foreign pid belongs to a forked child that this watcher cannot reach.
 */
static bool async_owner_is_local(int owner_type, int owner)
{
    switch (owner_type) {
    case FASYNC_OWNER_PID:
        return owner == (int) proc_get_pid();
    case FASYNC_OWNER_PGRP:
        return owner == (int) proc_get_pgid();
    case FASYNC_OWNER_TID:
        return thread_find(owner) != NULL;
    default:
        return false;
    }
}

/* kqueue udata carries the guest fd and the fd slot generation at arm time. The
 * generation guards against ABA: if the slot was closed and reused between arm
 * and a stale event firing, the generation no longer matches and the event is
 * dropped, so a SIGIO cannot land on a later, unrelated occupant of the same
 * guest fd number.
 *
 * The packing itself is proved in proved/asyncudata.h -- that whatever goes in
 * comes back out is the whole of the guard, and it used to rest on a comment
 * asserting that 1024 fds fit 16 bits and that 48 bits of generation will not
 * wrap. make verify-asyncudata now discharges both, and that the multiply
 * cannot overflow.
 */
static void *async_pack(int guest_fd, uint64_t generation)
{
    return (void *) (uintptr_t) async_udata_pack(guest_fd, generation);
}

static void async_unpack(void *udata, int *guest_fd, uint64_t *generation)
{
    uint64_t v = (uint64_t) (uintptr_t) udata;
    *guest_fd = async_udata_fd(v);
    *generation = async_udata_gen(v);
}

static void async_deliver(void *udata, int signum)
{
    int guest_fd;
    uint64_t generation;
    async_unpack(udata, &guest_fd, &generation);

    fd_entry_t snap;
    if (!fd_snapshot(guest_fd, &snap))
        return; /* slot closed since the knote fired */
    if (snap.generation % ASYNC_UDATA_GEN_SPAN != generation)
        return; /* slot reused (ABA): this event belongs to the prior open */
    if (signum != LINUX_SIGURG && !(snap.linux_flags & LINUX_O_ASYNC))
        return; /* disarmed between fire and here (EV_CLEAR raced a disarm) */
    if (!async_owner_is_local(snap.fasync_owner_type, snap.fasync_owner))
        return; /* no owner, or a foreign guest pid */

    /* Owner delivery is process-wide: F_OWNER_TID does not target a single
     * thread, and a foreign guest pid is not forwarded across the fork IPC
     * boundary. Per-thread signal queueing and cross-process forwarding are
     * what a workload needing directed SIGIO would require.
     */
    signal_queue(signum);
}

void asyncio_fire(int guest_fd)
{
    fd_entry_t snap;
    if (!fd_snapshot(guest_fd, &snap))
        return;
    if (!(snap.linux_flags & LINUX_O_ASYNC))
        return;
    if (!async_owner_is_local(snap.fasync_owner_type, snap.fasync_owner))
        return;
    signal_queue(LINUX_SIGIO);
}

static void *async_watcher(void *arg)
{
    (void) arg;
    struct kevent evs[32];
    for (;;) {
        int n = kevent(async_kq, NULL, 0, evs, 32, NULL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return NULL; /* kqueue closed or fatal; disable delivery */
        }
        for (int i = 0; i < n; i++) {
            int signum =
                (evs[i].filter == EVFILT_EXCEPT) ? LINUX_SIGURG : LINUX_SIGIO;
            async_deliver(evs[i].udata, signum);
        }
    }
}

static void async_init_once(void)
{
    async_kq = kqueue();
    if (async_kq < 0)
        return;
    fcntl(async_kq, F_SETFD, FD_CLOEXEC);
    pthread_t t;
    if (pthread_create(&t, NULL, async_watcher, NULL) != 0) {
        close(async_kq);
        async_kq = -1;
        return;
    }
    pthread_detach(t);
}

void asyncio_init(void)
{
    pthread_once(&async_once, async_init_once);
}

void asyncio_arm(int guest_fd, uint64_t generation, int host_fd, int fd_type)
{
    if (async_kq < 0 || host_fd < 0 || !async_is_pollable(fd_type))
        return;
    void *udata = async_pack(guest_fd, generation);
    struct kevent ch[2];
    int n = 0;
    EV_SET(&ch[n++], host_fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, udata);
    if (fd_type == FD_SOCKET)
        EV_SET(&ch[n++], host_fd, EVFILT_EXCEPT, EV_ADD | EV_CLEAR, NOTE_OOB, 0,
               udata);
    kevent(async_kq, ch, n, NULL, 0, NULL);
}

void asyncio_disarm(int host_fd)
{
    if (async_kq < 0)
        return;

    /* Issue each delete on its own kevent() call: with a NULL eventlist kevent
     * stops at the first failing change, and ENOENT is expected here (the
     * EVFILT_EXCEPT knote exists only for sockets, and a closed host fd has
     * already auto-removed both).
     */
    struct kevent ch;
    EV_SET(&ch, host_fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent(async_kq, &ch, 1, NULL, 0, NULL);
    EV_SET(&ch, host_fd, EVFILT_EXCEPT, EV_DELETE, 0, 0, NULL);
    kevent(async_kq, &ch, 1, NULL, 0, NULL);
}

/* Recompute one alias slot's readiness watch after its O_ASYNC bit or fasync
 * owner changed. A slot is armed iff O_ASYNC is set (SIGIO on readiness) or it
 * is a socket with an owner (SIGURG on OOB), matching the delivery-side checks
 * in async_deliver. Caller holds fd_lock. This single rule replaces the two
 * hand-rolled arm/disarm variants fasync_owner_set and asyncio_apply used to
 * carry, which had diverged (the owner path relied on the O_ASYNC path having
 * already armed non-socket slots rather than stating the invariant itself).
 */
static void async_reeval_slot_locked(int i)
{
    bool async = (fd_table[i].linux_flags & LINUX_O_ASYNC) != 0;
    bool owned = fd_table[i].fasync_owner_type != FASYNC_OWNER_NONE;
    if (async || (fd_table[i].type == FD_SOCKET && owned))
        asyncio_arm(i, fd_table[i].generation, fd_table[i].host_fd,
                    fd_table[i].type);
    else
        asyncio_disarm(fd_table[i].host_fd);
}

typedef struct {
    int32_t owner_type, owner;
} owner_set_ctx_t;

/* Both sweeps below run under fd_lock inside fd_for_each_alias_locked, and both
 * re-evaluate the slot's kevent registration after the change: arming inside
 * the lock is what closes the close+reuse race, since a sibling cannot retire
 * and reopen a host fd number while the scan holds it.
 */
static void owner_set_slot(int guest_fd, void *ctx)
{
    const owner_set_ctx_t *o = ctx;
    fd_table[guest_fd].fasync_owner_type = o->owner_type;
    fd_table[guest_fd].fasync_owner = o->owner;
    async_reeval_slot_locked(guest_fd);
}

/* True when F_SETFL(O_ASYNC) sticks, so F_GETFL reports it afterwards.
 *
 * Linux does not carry FASYNC in SETFL_MASK: setfl() lands the bit only by
 * calling file_operations->fasync, so an object whose fops lack one keeps
 * O_ASYNC clear however often the guest sets it. The set was measured against
 * qemu-aarch64, not read off the kernel source, which reads as though the bit
 * sticks everywhere:
 *
 *   keeps it: pipe, fifo, socket, netlink, inotify, tty, /dev/urandom
 *   drops it: timerfd, eventfd, signalfd, epoll, pidfd, regular file,
 *             directory, /dev/null, /dev/zero
 *
 * The type alone cannot answer for FD_REGULAR and FD_STDIO, which may be a
 * fifo, a socket, a tty, another character device or a plain file, so those two
 * ask the host object what it is. can_block was the first answer here and was
 * wrong in both directions: it takes in every character device, which put
 * O_ASYNC on /dev/null, and it says nothing about /dev/urandom, which Linux
 * does let the flag stick on (random_fasync). This runs on F_SETFL of O_ASYNC
 * and nowhere else.
 */
static bool fd_keeps_fasync(int type, int host_fd)
{
    switch (type) {
    case FD_PIPE:
    case FD_SOCKET:
    case FD_NETLINK:
    case FD_INOTIFY:
    case FD_FUSE_DEV:
    case FD_URANDOM:
        return true;
    case FD_REGULAR:
    case FD_STDIO:
        break;
    default:
        return false;
    }

    struct stat st;
    if (host_fd < 0 || fstat(host_fd, &st) != 0)
        return false;
    if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode))
        return true;
    if (S_ISCHR(st.st_mode))
        return isatty(host_fd) == 1;
    return false;
}

static void async_flag_slot(int guest_fd, void *ctx)
{
    /* Setting the bit is conditional, clearing it never is: Linux lands FASYNC
     * only through file_operations->fasync, so on an object without one the
     * flag stays clear and F_GETFL keeps reporting 0. elfuse used to record the
     * request for every type, which made a timerfd, an eventfd and a plain file
     * all claim O_ASYNC they would never deliver.
     */
    bool on = *(bool *) ctx && fd_keeps_fasync(fd_table[guest_fd].type,
                                               fd_table[guest_fd].host_fd);
    if (on)
        fd_table[guest_fd].linux_flags |= LINUX_O_ASYNC;
    else
        fd_table[guest_fd].linux_flags &= ~LINUX_O_ASYNC;
    async_reeval_slot_locked(guest_fd);
}

void fasync_owner_set(int guest_fd,
                      uint64_t expect_gen,
                      int owner_type,
                      int owner)
{
    if (!RANGE_CHECK(guest_fd, 0, FD_TABLE_SIZE))
        return;

    /* Arm/disarm under fd_lock with the live host fd and generation. Doing the
     * kevent registration inside the lock (rather than snapshotting aliases and
     * acting after unlock) closes the close+reuse race: a sibling close cannot
     * retire and reopen a host fd number while this scan holds fd_lock, so a
     * disarm can never clobber the fresh registration of a reused fd. kevent()
     * registration does not block, and the watcher thread never holds fd_lock
     * while parked in kevent(), so there is no deadlock.
     */
    owner_set_ctx_t ctx = {.owner_type = owner_type, .owner = owner};
    pthread_mutex_lock(&fd_lock);
    fd_for_each_alias_locked(guest_fd, expect_gen, owner_set_slot, &ctx);
    pthread_mutex_unlock(&fd_lock);
}

void fasync_owner_get(int guest_fd,
                      uint64_t expect_gen,
                      int *owner_type_out,
                      int *owner_out)
{
    *owner_type_out = FASYNC_OWNER_NONE;
    *owner_out = 0;
    if (!RANGE_CHECK(guest_fd, 0, FD_TABLE_SIZE))
        return;
    pthread_mutex_lock(&fd_lock);
    if (fd_table[guest_fd].type != FD_CLOSED &&
        fd_table[guest_fd].generation == expect_gen) {
        *owner_type_out = fd_table[guest_fd].fasync_owner_type;
        *owner_out = fd_table[guest_fd].fasync_owner;
    }
    pthread_mutex_unlock(&fd_lock);
}

void asyncio_apply(int guest_fd, uint64_t expect_gen, bool on)
{
    if (!RANGE_CHECK(guest_fd, 0, FD_TABLE_SIZE))
        return;

    /* Guard on generation, not just (type, host_fd): a close+reopen can reuse
     * the same guest fd number, host fd number, and type, so only the monotonic
     * generation distinguishes the caller's open from a new one. Arm/disarm run
     * under fd_lock so a host fd cannot be retired and reused mid-scan; see
     * fasync_owner_set for the deadlock argument.
     */
    pthread_mutex_lock(&fd_lock);
    fd_for_each_alias_locked(guest_fd, expect_gen, async_flag_slot, &on);
    pthread_mutex_unlock(&fd_lock);
}
