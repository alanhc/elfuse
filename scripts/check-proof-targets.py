#!/usr/bin/env python3
"""Fail when the three lists naming proof targets drift apart.

Three places name the same set of proved sources, and nothing but this script
keeps them in agreement:

  1. mk/verify.mk's VERIFY_<T>_SRC entries -- the targets themselves.
  2. .github/workflows/verify.yml's verify-mutants matrix -- the CI sharding.
  3. src/proved/ -- the directory the proved headers live in.

The third is the one the directory name rests on. src/proved/ claims its
contents are machine-checked, but a header dropped in there is proved only if
some verify-<name> target names it: the Makefile drives the prover, not the
path. Without this check the directory could hold an unproved file and still
read as a guarantee, which is worse than no directory at all.

.github/workflows/verify.yml's verify-mutants job shards one runner per proof
target, and its matrix is fromJson of "make print-verify-targets" rather than
a list of its own. What is checked here is that it stays that way, and that
the list make generates is the whole list: a VERIFY_<T>_SRC block written
below the line that snapshots them is invisible to make and to CI while
still reading as a target in that file.

Usage:
    check-proof-targets.py
"""

import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


# scripts/ filenames are kebab-case per CLAUDE.md, which no plain "import"
# statement can name, so the shared reader is loaded by path. The alternative
# was an underscore in the filename, which the tree does not use anywhere.
def _load_verify_mk():
    import importlib.util

    path = pathlib.Path(__file__).resolve().parent / "verify-mk.py"
    spec = importlib.util.spec_from_file_location("verify_mk", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


verify_mk = _load_verify_mk()


PROVED_DIR = ROOT / "src" / "proved"


def proved_dir_sources():
    """Header paths under src/proved/, relative to the tree root."""
    return {f"src/proved/{p.name}" for p in sorted(PROVED_DIR.glob("*.h"))}


def tracked_sources():
    """Paths under src/proved/ that git has in the index."""
    out = subprocess.run(
        ["git", "-C", str(ROOT), "ls-files", "src/proved"],
        capture_output=True,
        text=True,
    )
    return set(out.stdout.split())


def make_target_names():
    """The target names make actually generates rules for.

    Deliberately asks make rather than reading the file, because the two can
    disagree. mk/verify.mk derives its list with
    "VERIFY_TARGETS := $(filter VERIFY_%_SRC,$(.VARIABLES))", and := is
    immediate: .VARIABLES holds only what make has read so far, so a
    VERIFY_<T>_SRC block written below that line is invisible to it. The block
    parses, this script sees it, and make silently generates no rule and prints
    no warning. That is the whole failure mode below.
    """
    out = subprocess.run(
        ["make", "-C", str(ROOT), "print-verify-targets"],
        capture_output=True,
        text=True,
    )
    if out.returncode != 0:
        print(
            "  'make print-verify-targets' failed, so the target list CI "
            f"builds its matrix from cannot be read:\n{out.stderr.strip()}",
            file=sys.stderr,
        )
        return None
    return set(out.stdout.split())


def workflow_matrix_is_derived():
    """Whether the verify-mutants matrix is built from mk/verify.mk.

    It used to be a hand-kept copy of the target list and this function
    compared the two. The copy is gone: a proof-targets job emits the list
    (from "make print-verify-targets", or from proof-scope.py narrowed to what
    a pull request's diff can reach) and the matrix is fromJson of its output,
    so the matrix cannot drift from what make generates. What is worth checking
    now is that nobody has quietly gone back to a literal list, which would
    restore the drift this script exists to prevent.

    That leaves one gap this cannot see, which make_target_names covers: the
    matrix faithfully reproduces a target list that silently dropped a block.
    """
    expected = "${{ fromJson(needs.proof-targets.outputs.mutants) }}"
    text = (ROOT / ".github" / "workflows" / "verify.yml").read_text()
    # [^\n]* rather than .*, because re.S would run the capture to the end of
    # the file and accept a literal list here on the strength of an unrelated
    # fromJson in a later job.
    m = re.search(r"^  verify-mutants:.*?^        target:([^\n]*)$", text, re.M | re.S)
    if not m:
        print(
            "  could not find verify-mutants' matrix.target in "
            ".github/workflows/verify.yml; the job may have been renamed or "
            "restructured, so update this check to match",
            file=sys.stderr,
        )
        return False
    if m.group(1).strip() != expected:
        shape = m.group(1).strip() or "a literal list on the following lines"
        print(
            "  verify-mutants' matrix.target is not derived: "
            f"{shape}\n"
            f"  It should be exactly {expected} so the target list has one "
            "home in mk/verify.mk. Another job's output would be derived "
            "too, but from something this script does not read.",
            file=sys.stderr,
        )
        return False
    return True


def verdict_covers_every_prover_job():
    """Whether the historic check name still covers every job that proves.

    "Frama-C WP proofs (make verify)" aggregated a matrix whose legs ran the
    proofs AND their mutations, so a branch rule requiring it enforced both.
    Splitting those halves into separate jobs already moved that name once,
    and a reviewer had to notice; the next split should not need one. Every
    macOS job in the workflow does prover work, so each must be reachable from
    that job's needs, or the name goes green without it.
    """
    # Regex rather than a YAML parser, matching what this file already does
    # above and keeping the lint job free of a PyYAML dependency it does not
    # otherwise need. The shapes read here are the ones this workflow uses:
    # a two-space job key, and needs as either a scalar or a flow list.
    #
    # Split the "jobs:" section rather than the whole file, and accept any
    # leading identifier character rather than a lower-case letter. A key class
    # of [a-z] silently drops a job named "Verify-extra" or "_probe" from BOTH
    # sides of the comparison below, so a macOS job the check name does not
    # reach reads as no job at all -- the exact miss this function exists to
    # make loud. Splitting the whole file also read the sub-keys of "on:",
    # "concurrency:" and "permissions:" as jobs.
    text = (ROOT / ".github" / "workflows" / "verify.yml").read_text()
    section = re.split(r"^jobs:[ \t]*$", text, maxsplit=1, flags=re.M)
    if len(section) != 2:
        print(
            "  no top-level 'jobs:' key in .github/workflows/verify.yml",
            file=sys.stderr,
        )
        return False
    blocks = re.split(r"^  (?=[A-Za-z_])", section[1], flags=re.M)[1:]
    jobs = {}
    for block in blocks:
        job = block.split(":", 1)[0]
        needs = re.search(r"^    needs:\s*(.+)$", block, re.M)
        jobs[job] = {
            "name": (re.search(r"^    name:\s*(.+)$", block, re.M) or [None, ""])[1],
            "runs-on": (re.search(r"^    runs-on:\s*(.+)$", block, re.M) or [None, ""])[
                1
            ],
            "needs": re.findall(r"[A-Za-z][\w-]*", needs.group(1)) if needs else [],
        }
    verdict = [j for j, v in jobs.items() if v["name"].endswith("(make verify)")]
    if len(verdict) != 1:
        print(
            "  expected exactly one job named '... (make verify)' in "
            f".github/workflows/verify.yml, found {verdict}",
            file=sys.stderr,
        )
        return False

    seen, stack = set(), list(jobs[verdict[0]]["needs"])
    while stack:
        job = stack.pop()
        if job in seen or job not in jobs:
            continue
        seen.add(job)
        stack.extend(jobs[job]["needs"])
    # Substring, case-folded, rather than a prefix on "macos". The hosted
    # runner is "macos-15", but the self-hosted one build.yml already uses is
    # the flow list "[self-hosted, macOS, arm64]", which a prefix test reads as
    # a non-macOS job and stops enforcing on the day this workflow moves there.
    # Over-matching only demands one more "needs" edge; under-matching drops a
    # prover job out from under the required check with nothing said.
    prover_jobs = {j for j, v in jobs.items() if "macos" in v["runs-on"].lower()}
    missing = sorted(prover_jobs - seen)
    if missing:
        print(
            f"  {missing} run on macOS but the '{jobs[verdict[0]]['name']}' check "
            "does not reach them through needs, so requiring that check would "
            "not enforce them",
            file=sys.stderr,
        )
        return False
    return True


def main():
    mk = verify_mk.targets()
    if not verdict_covers_every_prover_job():
        return 1
    if not workflow_matrix_is_derived():
        return 2

    # The matrix being derived only helps if what it derives from is complete.
    generated = make_target_names()
    if generated is None:
        return 2
    dropped = {t.lower() for t in mk} - generated
    if dropped:
        print(
            "  VERIFY_<T>_SRC block(s) that make generates no rule for. "
            "Nothing fails today: the proof simply never runs, here or in "
            "CI. Move the block above the 'VERIFY_TARGETS :=' line, which "
            "snapshots the target list at the point it appears:",
            file=sys.stderr,
        )
        for t in sorted(dropped):
            print(f"    verify-{t}", file=sys.stderr)
        return 1

    # Both directions. A target naming a file that is not in the tree is the
    # more damaging drift of the two: it survives locally, where the file
    # exists but is untracked, and breaks every fresh clone and CI checkout.
    missing_files = {
        src
        for src in verify_mk.sources()
        if src.startswith("src/proved/") and not (ROOT / src).exists()
    }
    if missing_files:
        print(
            "  VERIFY_<T>_SRC entries in mk/verify.mk naming a file that "
            "does not exist. The build and the proofs reference it, so a "
            "fresh clone fails even though this tree works:",
            file=sys.stderr,
        )
        for src in sorted(missing_files):
            print(f"    {src}", file=sys.stderr)
        return 1

    untracked = {
        src
        for src in verify_mk.sources()
        if src.startswith("src/proved/") and src not in tracked_sources()
    }
    if untracked:
        print(
            "  VERIFY_<T>_SRC entries naming a file git does not track. It "
            "exists here and nowhere else, which is the same failure one "
            "commit later:",
            file=sys.stderr,
        )
        for src in sorted(untracked):
            print(f"    {src}", file=sys.stderr)
        return 1

    unproved = proved_dir_sources() - verify_mk.sources()
    if unproved:
        print(
            "  file(s) under src/proved/ that no verify-<name> target "
            "proves. The directory name says otherwise, so either add a "
            "VERIFY_<T>_SRC block in mk/verify.mk or move the file out:",
            file=sys.stderr,
        )
        for f in sorted(unproved):
            print(f"    {f}", file=sys.stderr)
        return 1

    print(
        f"  {len(mk)} proof target(s) in mk/verify.mk, all with a proved "
        f"source; the CI matrix is derived from that list, and all "
        f"{len(proved_dir_sources())} file(s) under src/proved/ are proved "
        "by one"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
