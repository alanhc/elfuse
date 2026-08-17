/*
 * Exercise the AF_NETLINK getsockname, send and receive dispatch paths in every
 * spelling: sendto, sendmsg, write, writev, recvfrom, recvmsg, read and readv.
 *
 * Copyright 2026 elfuse contributors
 * Copyright 2025 Moritz Angermann, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression guard for the netlink socket emulation. Before getsockname,
 * sendto, and recvfrom were routed to the netlink handlers, these calls fell
 * through to the host socket syscalls on the underlying pipe fd and failed with
 * ENOTSOCK (errno 88), which in turn broke glibc getifaddrs(). write() and
 * writev() kept falling through afterwards and reached the read end of the
 * readiness pipe, which reports EBADF and broke senders that use write() in
 * place of sendto(), busybox "ip" among them. The test drives each syscall
 * directly against a NETLINK_ROUTE socket and then validates the end-to-end
 * getifaddrs() path that originally regressed.
 *
 * The assertions hold for both the elfuse emulation and a real Linux kernel
 * (the test matrix runs the same binary under qemu-aarch64), so only
 * implementation-independent netlink semantics are checked.
 */

#include <errno.h>
#include <ifaddrs.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/uio.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

/* Upper bound on how long one dump message may take to show up. */
#define DUMP_POLL_MS 5000

/* First readv() entry: smaller than any RTM_NEWLINK message, so a receive that
 * ignores the entries past it cannot return more than this.
 */
#define READV_HEAD 16

static int pass, fail;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            printf("PASS: %s\n", (msg));                                       \
            pass++;                                                            \
        } else {                                                               \
            printf("FAIL: %s (errno=%d %s)\n", (msg), errno, strerror(errno)); \
            fail++;                                                            \
        }                                                                      \
    } while (0)

/* RTM_GETLINK dump request: nlmsghdr immediately followed by ifinfomsg. */
struct getlink_req {
    struct nlmsghdr nlh;
    struct ifinfomsg ifi;
};

/* Drain one dump and report whether it carried an RTM_NEWLINK. Both the
 * emulation and a real kernel end a dump with NLMSG_DONE.
 *
 * Every receive waits through poll() first. A request that never reached the
 * netlink layer leaves nothing to read, and a bare blocking recvfrom() would
 * turn that regression into a hung test run instead of a failed assertion.
 */
static int dump_has_newlink(int fd)
{
    int saw_newlink = 0, saw_done = 0;
    for (int iter = 0; iter < 64 && !saw_done; iter++) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, DUMP_POLL_MS) <= 0)
            break;

        char buf[8192];
        struct sockaddr_nl src = {0};
        socklen_t srclen = sizeof(src);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *) &src,
                             &srclen);
        if (n <= 0)
            break;
        for (struct nlmsghdr *nlh = (struct nlmsghdr *) buf;
             NLMSG_OK(nlh, (unsigned) n); nlh = NLMSG_NEXT(nlh, n)) {
            if (nlh->nlmsg_type == RTM_NEWLINK)
                saw_newlink = 1;
            else if (nlh->nlmsg_type == NLMSG_DONE ||
                     nlh->nlmsg_type == NLMSG_ERROR)
                saw_done = 1;
        }
    }
    return saw_newlink;
}

int main(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0) {
        printf("FAIL: socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE): %s\n",
               strerror(errno));
        /* Without a socket none of the dispatch paths can be reached. */
        printf("\n%d passed, %d failed\n", pass, fail + 1);
        return 1;
    }
    printf("PASS: socket(AF_NETLINK, NETLINK_ROUTE) = %d\n", fd);
    pass++;

    /* bind() with nl_pid=0 lets the kernel/emulation assign a port id. */
    struct sockaddr_nl local = {.nl_family = AF_NETLINK};
    CHECK(bind(fd, (struct sockaddr *) &local, sizeof(local)) == 0,
          "bind(AF_NETLINK)");

    /* 1. getsockname(): previously ENOTSOCK on the pipe fd. */
    struct sockaddr_nl got = {0};
    socklen_t gotlen = sizeof(got);
    int rc = getsockname(fd, (struct sockaddr *) &got, &gotlen);
    CHECK(rc == 0 && gotlen >= sizeof(struct sockaddr_nl) &&
              got.nl_family == AF_NETLINK,
          "getsockname() returns an AF_NETLINK address");
    CHECK(rc == 0 && got.nl_pid != 0,
          "getsockname() reports a non-zero port id");

    /* 2. sendto(): flat request buffer, no msghdr. */
    struct getlink_req req = {0};
    req.nlh.nlmsg_len = sizeof(req);
    req.nlh.nlmsg_type = RTM_GETLINK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq = 1;
    req.ifi.ifi_family = AF_UNSPEC;

    struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
    ssize_t sent = sendto(fd, &req, req.nlh.nlmsg_len, 0,
                          (struct sockaddr *) &kernel, sizeof(kernel));
    CHECK(sent == (ssize_t) req.nlh.nlmsg_len,
          "sendto(RTM_GETLINK) accepts the request");

    /* 3. recvfrom(): drain the dump, expecting RTM_NEWLINK then NLMSG_DONE. */
    int saw_newlink = 0, saw_done = 0, src_ok = 0;
    for (int iter = 0; iter < 64 && !saw_done; iter++) {
        char buf[8192];
        struct sockaddr_nl src = {0};
        socklen_t srclen = sizeof(src);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *) &src,
                             &srclen);
        if (n <= 0)
            break;
        if (srclen >= sizeof(struct sockaddr_nl) && src.nl_family == AF_NETLINK)
            src_ok = 1;
        for (struct nlmsghdr *nlh = (struct nlmsghdr *) buf;
             NLMSG_OK(nlh, (unsigned) n); nlh = NLMSG_NEXT(nlh, n)) {
            if (nlh->nlmsg_type == RTM_NEWLINK)
                saw_newlink = 1;
            else if (nlh->nlmsg_type == NLMSG_DONE)
                saw_done = 1;
            else if (nlh->nlmsg_type == NLMSG_ERROR)
                saw_done = 1; /* stop draining on error terminator */
        }
    }
    CHECK(saw_newlink, "recvfrom() returns at least one RTM_NEWLINK");
    CHECK(src_ok, "recvfrom() fills an AF_NETLINK source address");

    /* 4. write(): Linux treats it as sendto() with no explicit destination, so
     * a request sent this way has to reach the same dump. busybox "ip" sends
     * its rtnetlink requests with write().
     */
    req.nlh.nlmsg_seq = 2;
    ssize_t wrote = write(fd, &req, req.nlh.nlmsg_len);
    CHECK(wrote == (ssize_t) req.nlh.nlmsg_len,
          "write(RTM_GETLINK) accepts the request");
    CHECK(dump_has_newlink(fd), "write() request produces an RTM_NEWLINK dump");

    /* 5. Sends carrying nothing. netlink_sendmsg() refuses an empty message,
     * but a vectored write never reaches it: do_readv_writev() returns 0 for a
     * zero total first. The asymmetry is the point of testing both.
     */
    errno = 0;
    CHECK(write(fd, &req, 0) == -1 && errno == ENODATA,
          "write() of zero bytes fails with ENODATA");
    struct iovec empty[2] = {
        {.iov_base = &req, .iov_len = 0},
        {.iov_base = &req, .iov_len = 0},
    };
    CHECK(writev(fd, empty, 2) == 0, "writev() of zero bytes returns 0");

    /* 6. writev(): the request split mid-header across two entries. A receiver
     * that reads only the first entry gets a runt it cannot parse, so this
     * fails unless the entries are gathered into one request.
     */
    req.nlh.nlmsg_seq = 3;
    struct iovec iov[2] = {
        {.iov_base = &req, .iov_len = 8},
        {.iov_base = (char *) &req + 8, .iov_len = req.nlh.nlmsg_len - 8},
    };
    ssize_t wrotev = writev(fd, iov, 2);
    CHECK(wrotev == (ssize_t) req.nlh.nlmsg_len,
          "writev(RTM_GETLINK) accepts a split request");
    CHECK(dump_has_newlink(fd),
          "writev() gathers the split request into one dump");

    /* 7. readv(): a first entry far too small for the response. A receive that
     * stops at entry 0 cannot exceed READV_HEAD bytes. The entries are adjacent
     * halves of one buffer, so the scattered bytes stay contiguous and parse
     * as an ordinary message stream.
     */
    req.nlh.nlmsg_seq = 4;
    sent = sendto(fd, &req, req.nlh.nlmsg_len, 0, (struct sockaddr *) &kernel,
                  sizeof(kernel));
    CHECK(sent == (ssize_t) req.nlh.nlmsg_len,
          "sendto() before the readv() drain accepts the request");

    static char rbuf[8192];
    struct iovec riov[2] = {
        {.iov_base = rbuf, .iov_len = READV_HEAD},
        {.iov_base = rbuf + READV_HEAD, .iov_len = sizeof(rbuf) - READV_HEAD},
    };
    struct pollfd rpfd = {.fd = fd, .events = POLLIN};
    ssize_t rn = poll(&rpfd, 1, DUMP_POLL_MS) > 0 ? readv(fd, riov, 2) : -1;
    CHECK(rn > (ssize_t) READV_HEAD, "readv() fills past the first entry");

    int rv_newlink = 0;
    for (struct nlmsghdr *nlh = (struct nlmsghdr *) rbuf;
         rn > 0 && NLMSG_OK(nlh, (unsigned) rn); nlh = NLMSG_NEXT(nlh, rn))
        if (nlh->nlmsg_type == RTM_NEWLINK)
            rv_newlink = 1;
    CHECK(rv_newlink, "the scattered readv() bytes parse as RTM_NEWLINK");

    dump_has_newlink(fd); /* leave the socket idle for the next case */

    /* 8. sendmsg(): the same split request through msg_iov. All four send
     * spellings gather, so a first entry shorter than an nlmsghdr is a split
     * request rather than a malformed one.
     */
    req.nlh.nlmsg_seq = 5;
    struct iovec siov[2] = {
        {.iov_base = &req, .iov_len = 8},
        {.iov_base = (char *) &req + 8, .iov_len = req.nlh.nlmsg_len - 8},
    };
    struct msghdr smsg = {
        .msg_name = &kernel,
        .msg_namelen = sizeof(kernel),
        .msg_iov = siov,
        .msg_iovlen = 2,
    };
    CHECK(sendmsg(fd, &smsg, 0) == (ssize_t) req.nlh.nlmsg_len,
          "sendmsg() gathers a split request");

    /* 9. recvmsg(): a first entry far too small for the response, as in 7. */
    static char mbuf[8192];
    struct iovec mriov[2] = {
        {.iov_base = mbuf, .iov_len = READV_HEAD},
        {.iov_base = mbuf + READV_HEAD, .iov_len = sizeof(mbuf) - READV_HEAD},
    };
    struct sockaddr_nl from = {0};
    struct msghdr rmsg = {
        .msg_name = &from,
        .msg_namelen = sizeof(from),
        .msg_iov = mriov,
        .msg_iovlen = 2,
    };
    struct pollfd mpfd = {.fd = fd, .events = POLLIN};
    ssize_t mn = poll(&mpfd, 1, DUMP_POLL_MS) > 0 ? recvmsg(fd, &rmsg, 0) : -1;
    CHECK(mn > (ssize_t) READV_HEAD, "recvmsg() fills past the first entry");

    int rm_newlink = 0;
    for (struct nlmsghdr *nlh = (struct nlmsghdr *) mbuf;
         mn > 0 && NLMSG_OK(nlh, (unsigned) mn); nlh = NLMSG_NEXT(nlh, mn))
        if (nlh->nlmsg_type == RTM_NEWLINK)
            rm_newlink = 1;
    CHECK(rm_newlink, "the scattered recvmsg() bytes parse as RTM_NEWLINK");

    dump_has_newlink(fd);

    /* 10. sendmsg() carrying nothing. ___sys_sendmsg() hands an empty vector
     * down to the socket instead of answering it above, so this reports what
     * write(2) of nothing reports and not what writev(2) does.
     */
    errno = 0;
    struct msghdr emsg = {.msg_iov = siov, .msg_iovlen = 0};
    CHECK(sendmsg(fd, &emsg, 0) == -1 && errno == ENODATA,
          "sendmsg() of an empty iovec fails with ENODATA");

    close(fd);

    /* 11. End-to-end: glibc getifaddrs() drives getsockname + sendto + recv
     * internally. This is the exact call that regressed with ENOTSOCK.
     */
    struct ifaddrs *ifa = NULL;
    rc = getifaddrs(&ifa);
    CHECK(rc == 0, "getifaddrs() succeeds");
    int n_ifaces = 0;
    for (struct ifaddrs *p = ifa; p; p = p->ifa_next)
        if (p->ifa_name && p->ifa_name[0])
            n_ifaces++;
    CHECK(rc == 0 && n_ifaces > 0,
          "getifaddrs() enumerates at least one interface");
    if (ifa)
        freeifaddrs(ifa);

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
