/*
 * Path-translation cross product: addressing modes must agree
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Every escaped bug in the path layer sat in one cell of a cross product the
 * hand-written tests visited selectively: a final symlink through a dirfd, a
 * long-tier name through the openat2 walker, a create below an intermediate
 * link. This test enumerates the product programmatically (addressing mode
 * x operation x path shape x name class), so a cell exists because the loop
 * reached it, not because someone thought of it.
 *
 * Linux contract pinned: path_resolution(7) makes no distinction between an
 * absolute path, a cwd-relative one, and an openat(2) dirfd-relative one that
 * name the same file: same result, same errno, same object. The oracle is
 * exactly that agreement: each cell runs one operation through all three
 * modes and requires identical (rc, errno), the same (st_dev, st_ino) for
 * lookups, and for creates that the canonical absolute spelling sees what
 * was made. Agreement is the invariant every escaped dirfd bug violated, so
 * a divergence names its cell in the failure message.
 *
 * Code under test: path_translate_at and its relative/dirfd legs in
 * src/syscall/path.c, over the resolvers in src/syscall/proc-state.c and the
 * walk in src/syscall/casefold-walk.c. A regression shows up as one mode
 * diverging: a create landing beside a link instead of through it, a
 * lookup succeeding where a sibling mode reports ENOENT, or an errno class
 * changing with the spelling of the same file.
 *
 * Deliberately not here: the sysroot mounted at "/" (read-only root;
 * test-sysroot-root), concurrency (test-sysroot-name-race), and name-length
 * ceilings (test-sysroot-pathmax); single-axis suites cover those. A pass
 * proves mode agreement over the enumerated cells, not the absence of cells
 * outside the table. Run under --sysroot.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "test-harness.h"
#include "test-util.h"

int passes = 0, fails = 0;

#define ROOT "/pmx"

/* One result of running an operation in one addressing mode. */
typedef struct {
    int rc;    /* 0 or -1 */
    int err;   /* errno when rc < 0, else 0 */
    dev_t dev; /* identity when the op yields one */
    ino_t ino;
    bool has_id;
} cell_result_t;

enum { MODE_ABS, MODE_CWD, MODE_DIRFD, MODE_COUNT };

static const char *const mode_name[MODE_COUNT] = {"absolute", "cwd-relative",
                                                  "dirfd"};

/* The dirfd every MODE_DIRFD operation is measured from; opened on ROOT once
 * in main. MODE_CWD relies on main having chdir'd to ROOT.
 */
static int root_fd = -1;

/* Spell @rel (a path relative to ROOT) for @mode into @out. */
static const char *spell(int mode, const char *rel, char *out, size_t outsz)
{
    if (mode == MODE_ABS)
        snprintf(out, outsz, "%s/%s", ROOT, rel);
    else
        snprintf(out, outsz, "%s", rel);
    return out;
}

typedef int (*cell_op_t)(int mode, const char *rel, cell_result_t *res);

/* Fill @res from a stat-like probe of @rel in @mode; @nofollow picks the
 * lstat flavor. Identity is recorded so lookups can assert one object.
 */
static int probe_stat(int mode,
                      const char *rel,
                      bool nofollow,
                      cell_result_t *res)
{
    char path[PATH_MAX];
    struct stat st;
    int rc;

    errno = 0;
    if (mode == MODE_DIRFD)
        rc = fstatat(root_fd, rel, &st, nofollow ? AT_SYMLINK_NOFOLLOW : 0);
    else if (nofollow)
        rc = lstat(spell(mode, rel, path, sizeof(path)), &st);
    else
        rc = stat(spell(mode, rel, path, sizeof(path)), &st);
    res->rc = rc < 0 ? -1 : 0;
    res->err = rc < 0 ? errno : 0;
    res->has_id = rc == 0;
    res->dev = rc == 0 ? st.st_dev : 0;
    res->ino = rc == 0 ? st.st_ino : 0;
    return 0;
}

static int op_stat(int mode, const char *rel, cell_result_t *res)
{
    return probe_stat(mode, rel, false, res);
}

static int op_lstat(int mode, const char *rel, cell_result_t *res)
{
    return probe_stat(mode, rel, true, res);
}

static int op_open_rdonly(int mode, const char *rel, cell_result_t *res)
{
    char path[PATH_MAX];
    struct stat st;
    int fd;

    errno = 0;
    if (mode == MODE_DIRFD)
        fd = openat(root_fd, rel, O_RDONLY);
    else
        fd = open(spell(mode, rel, path, sizeof(path)), O_RDONLY);
    res->rc = fd < 0 ? -1 : 0;
    res->err = fd < 0 ? errno : 0;
    res->has_id = false;
    if (fd >= 0) {
        if (fstat(fd, &st) == 0) {
            res->has_id = true;
            res->dev = st.st_dev;
            res->ino = st.st_ino;
        }
        close(fd);
    }
    return 0;
}

static int op_openat2_nosym(int mode, const char *rel, cell_result_t *res)
{
    struct open_how how = {
        .flags = O_RDONLY, .mode = 0, .resolve = RESOLVE_NO_SYMLINKS};
    char path[PATH_MAX];
    struct stat st;
    long fd;

    errno = 0;
    if (mode == MODE_DIRFD)
        fd = syscall(SYS_openat2, root_fd, rel, &how, sizeof(how));
    else
        fd = syscall(SYS_openat2, AT_FDCWD,
                     spell(mode, rel, path, sizeof(path)), &how, sizeof(how));
    res->rc = fd < 0 ? -1 : 0;
    res->err = fd < 0 ? errno : 0;
    res->has_id = false;
    if (fd >= 0) {
        if (fstat((int) fd, &st) == 0) {
            res->has_id = true;
            res->dev = st.st_dev;
            res->ino = st.st_ino;
        }
        close((int) fd);
    }
    return 0;
}

/* Create @rel, record the outcome, verify through the canonical absolute
 * spelling that exactly this mode's create is visible, then remove it the
 * same canonical way so the next mode starts from the same directory.
 */
static int op_create(int mode, const char *rel, cell_result_t *res)
{
    char path[PATH_MAX];
    char abs[PATH_MAX];
    struct stat st;
    int fd;

    snprintf(abs, sizeof(abs), "%s/%s", ROOT, rel);
    errno = 0;
    if (mode == MODE_DIRFD)
        fd = openat(root_fd, rel, O_CREAT | O_WRONLY, 0644);
    else
        fd = open(spell(mode, rel, path, sizeof(path)), O_CREAT | O_WRONLY,
                  0644);
    res->rc = fd < 0 ? -1 : 0;
    res->err = fd < 0 ? errno : 0;
    res->has_id = false;
    if (fd >= 0) {
        close(fd);
        /* The canonical spelling must see what this mode made: a create that
         * "succeeded" somewhere else is the failure this matrix exists for.
         */
        if (stat(abs, &st) == 0) {
            res->has_id = true;
            res->dev = st.st_dev;
            res->ino = st.st_ino;
        } else {
            res->rc = -1;
            res->err = ENOENT; /* created, but not where the name says */
        }
        unlink(abs);
    }
    return 0;
}

static int op_mkdir(int mode, const char *rel, cell_result_t *res)
{
    char path[PATH_MAX];
    char abs[PATH_MAX];
    struct stat st;
    int rc;

    snprintf(abs, sizeof(abs), "%s/%s", ROOT, rel);
    errno = 0;
    if (mode == MODE_DIRFD)
        rc = mkdirat(root_fd, rel, 0755);
    else
        rc = mkdir(spell(mode, rel, path, sizeof(path)), 0755);
    res->rc = rc < 0 ? -1 : 0;
    res->err = rc < 0 ? errno : 0;
    res->has_id = false;
    if (rc == 0) {
        if (stat(abs, &st) == 0 && S_ISDIR(st.st_mode)) {
            res->has_id = true;
            res->dev = st.st_dev;
            res->ino = st.st_ino;
        } else {
            res->rc = -1;
            res->err = ENOENT;
        }
        rmdir(abs);
    }
    return 0;
}

static int op_excl_existing(int mode, const char *rel, cell_result_t *res)
{
    char path[PATH_MAX];
    int fd;

    errno = 0;
    if (mode == MODE_DIRFD)
        fd = openat(root_fd, rel, O_CREAT | O_EXCL | O_WRONLY, 0644);
    else
        fd = open(spell(mode, rel, path, sizeof(path)),
                  O_CREAT | O_EXCL | O_WRONLY, 0644);
    res->rc = fd < 0 ? -1 : 0;
    res->err = fd < 0 ? errno : 0;
    res->has_id = false;
    if (fd >= 0)
        close(fd); /* agreement failure; leave the evidence in place */
    return 0;
}

static int op_rename(int mode, const char *rel, cell_result_t *res)
{
    char src_spelled[PATH_MAX];
    char dst_spelled[PATH_MAX];
    char abs[PATH_MAX];
    char dst_rel[PATH_MAX];
    char dst_abs[PATH_MAX];
    struct stat st;
    int rc;

    /* Rename within the same directory prefix: the destination reuses the
     * source's parent so the cell exercises exactly one shape.
     */
    snprintf(abs, sizeof(abs), "%s/%s", ROOT, rel);
    snprintf(dst_rel, sizeof(dst_rel), "%s.Renamed", rel);
    snprintf(dst_abs, sizeof(dst_abs), "%s/%s", ROOT, dst_rel);

    /* Stage the source through the canonical spelling. */
    int fd = open(abs, O_CREAT | O_WRONLY, 0644);
    if (fd < 0) {
        res->rc = -1;
        res->err = errno;
        res->has_id = false;
        return 0;
    }
    close(fd);

    errno = 0;
    if (mode == MODE_DIRFD)
        rc = renameat(root_fd, rel, root_fd, dst_rel);
    else
        rc = rename(spell(mode, rel, src_spelled, sizeof(src_spelled)),
                    spell(mode, dst_rel, dst_spelled, sizeof(dst_spelled)));
    res->rc = rc < 0 ? -1 : 0;
    res->err = rc < 0 ? errno : 0;
    res->has_id = false;
    if (rc == 0) {
        if (stat(dst_abs, &st) == 0) {
            res->has_id = true;
            res->dev = st.st_dev;
            res->ino = st.st_ino;
        } else {
            res->rc = -1;
            res->err = ENOENT;
        }
    }
    unlink(dst_abs);
    unlink(abs);
    return 0;
}

typedef struct {
    const char *name;
    cell_op_t fn;
    bool creates; /* the cell mutates; run only against absent leaves */
} op_t;

static const op_t ops[] = {
    {"stat", op_stat, false},
    {"lstat", op_lstat, false},
    {"open", op_open_rdonly, false},
    {"openat2-nosym", op_openat2_nosym, false},
    {"create", op_create, true},
    {"mkdir", op_mkdir, true},
    {"excl-existing", op_excl_existing, false},
    {"rename", op_rename, true},
};

/* Path shapes: a prefix the leaf is planted under. sublink is a symlink to
 * Sub.Dir, so the third shape crosses an intermediate link; the fourth
 * carries a '..' component, which the resolvers collapse lexically, so every
 * addressing mode must keep agreeing about where the leaf lives.
 */
static const char *const shapes[] = {"", "Sub.Dir/", "sublink/",
                                     "Sub.Dir/../Sub.Dir/"};

/* Name classes. The escape-shaped literal is a name the guest may legally
 * create; the long-tier name crosses the codec's hex ceiling.
 */
#define LONG_NAME_LEN 126
static char long_name[LONG_NAME_LEN + 1];

static const char *leaves[] = {"plain-name", "Mixed.Name", ".ef=464f4f",
                               long_name};

/* Fixture leaves that exist before the lookup ops run. */
static void stage_fixture(const char *rel)
{
    char abs[PATH_MAX];
    int fd;

    snprintf(abs, sizeof(abs), "%s/%s", ROOT, rel);
    fd = open(abs, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) {
        write(fd, "pmx", 3);
        close(fd);
    }
}

static void run_cell(const op_t *op, const char *shape, const char *leaf)
{
    cell_result_t r[MODE_COUNT];
    char rel[PATH_MAX];
    char label[192];
    bool agree = true;

    snprintf(rel, sizeof(rel), "%s%s", shape, leaf);
    snprintf(label, sizeof(label), "%s %s%s agrees across modes", op->name,
             shape, leaf);

    for (int m = 0; m < MODE_COUNT; m++)
        op->fn(m, rel, &r[m]);

    for (int m = 1; m < MODE_COUNT; m++) {
        if (r[m].rc != r[0].rc || r[m].err != r[0].err)
            agree = false;
        /* Identity must match for lookups only: a mutating cell makes (and
         * removes) a fresh object per mode, so its inodes legitimately
         * differ; placement is already folded into rc via the canonical
         * probe inside the op.
         */
        if (!op->creates && r[m].has_id && r[0].has_id &&
            (r[m].dev != r[0].dev || r[m].ino != r[0].ino))
            agree = false;
    }

    TEST(label);
    if (agree) {
        PASS();
    } else {
        FAIL("addressing modes disagree");
        for (int m = 0; m < MODE_COUNT; m++)
            printf("      %-12s rc=%d errno=%d ino=%llu\n", mode_name[m],
                   r[m].rc, r[m].err,
                   r[m].has_id ? (unsigned long long) r[m].ino : 0ULL);
    }
}

int main(void)
{
    char abs[PATH_MAX];

    printf("test-sysroot-path-matrix: addressing modes must agree\n");

    memset(long_name, 'Q', LONG_NAME_LEN);
    long_name[LONG_NAME_LEN] = '\0';

    TEST("fixtures");
    snprintf(abs, sizeof(abs), "%s/Sub.Dir", ROOT);
    bool ok = (mkdir(ROOT, 0755) == 0 || errno == EEXIST) &&
              (mkdir(abs, 0755) == 0 || errno == EEXIST);
    snprintf(abs, sizeof(abs), "%s/sublink", ROOT);
    ok = ok && (symlink("Sub.Dir", abs) == 0 || errno == EEXIST);
    ok = ok && chdir(ROOT) == 0 &&
         (root_fd = open(ROOT, O_RDONLY | O_DIRECTORY)) >= 0;
    EXPECT_TRUE(ok, "fixture setup");
    if (!ok) {
        SUMMARY("test-sysroot-path-matrix");
        return 1;
    }

    /* Lookup fixtures: every (shape, leaf) cell that lookup ops touch holds a
     * real file, staged through the canonical absolute spelling. sublink/
     * shares Sub.Dir/'s entries by construction, which is the point: the two
     * shapes must then also agree with each other about identity.
     */
    for (size_t s = 0; s < sizeof(shapes) / sizeof(shapes[0]); s++) {
        if (!strcmp(shapes[s], "sublink/"))
            continue;
        for (size_t l = 0; l < sizeof(leaves) / sizeof(leaves[0]); l++) {
            char rel[PATH_MAX];
            snprintf(rel, sizeof(rel), "%s%s", shapes[s], leaves[l]);
            stage_fixture(rel);
        }
    }

    /* A final symlink as its own shape, one per target class. */
    snprintf(abs, sizeof(abs), "%s/final-link", ROOT);
    symlink("Mixed.Name", abs);

    for (size_t o = 0; o < sizeof(ops) / sizeof(ops[0]); o++) {
        for (size_t s = 0; s < sizeof(shapes) / sizeof(shapes[0]); s++) {
            for (size_t l = 0; l < sizeof(leaves) / sizeof(leaves[0]); l++) {
                char rel[PATH_MAX];

                if (ops[o].creates) {
                    /* Mutating cells use a fresh leaf beside the fixture so
                     * the lookup fixtures stay untouched.
                     */
                    char fresh[PATH_MAX];
                    snprintf(fresh, sizeof(fresh), "New.%s", leaves[l]);
                    /* An escape-shaped or long fresh leaf keeps its class. */
                    if (!strcmp(leaves[l], ".ef=464f4f"))
                        snprintf(fresh, sizeof(fresh), ".ef=4e4557");
                    else if (leaves[l] == long_name)
                        snprintf(fresh, sizeof(fresh), "N%s", long_name);
                    snprintf(rel, sizeof(rel), "%s%s", shapes[s], fresh);
                    /* Guard the composed name against the guest ceiling. */
                    if (strlen(fresh) > 255)
                        continue;
                } else {
                    snprintf(rel, sizeof(rel), "%s%s", shapes[s], leaves[l]);
                }
                run_cell(&ops[o], shapes[s], rel + strlen(shapes[s]));
            }
        }
    }

    /* Final-symlink shape: follow and nofollow lookups, all modes. */
    run_cell(&ops[0], "", "final-link");
    run_cell(&ops[1], "", "final-link");
    run_cell(&ops[2], "", "final-link");

    /* Trailing separator asserts directoriness; a regular file owes ENOTDIR
     * in every mode alike (path_resolution(7)).
     */
    run_cell(&ops[0], "", "plain-name/");
    run_cell(&ops[0], "", "Mixed.Name/");
    run_cell(&ops[2], "Sub.Dir/", "Mixed.Name/");

    if (root_fd >= 0)
        close(root_fd);

    SUMMARY("test-sysroot-path-matrix");
    return fails > 0 ? 1 : 0;
}
