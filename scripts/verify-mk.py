"""The one reader of mk/verify.mk's VERIFY_<T>_* variables.

check-mutants.py, check-proof-targets.py and check-char-signedness.py all need
the proof-target table, and each had grown its own regex over the same lines:
patterns spelling "VERIFY_<T>_SRC" three ways, differing only in which capture
group they kept. They agree today, so nothing was broken; they are three places
to update when the variable naming changes, in scripts whose entire job is
catching exactly that kind of drift somewhere else.

Load this instead of re-deriving it. The filename is kebab-case per CLAUDE.md,
which no import statement can name, so each consumer pulls it in by path with
importlib; see _load_verify_mk there.
"""

import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent
VERIFY_MK = ROOT / "mk" / "verify.mk"
KNOWN_PROOF_WORKFLOWS = {
    ".github/workflows/lint.yml",
    ".github/workflows/verify.yml",
}


def text():
    """mk/verify.mk as text."""
    return VERIFY_MK.read_text()


def joined_text():
    """mk/verify.mk with make line continuations joined."""
    return re.sub(r"\\\n", " ", text())


def _target_var(suffix):
    """{target: [word]} for every VERIFY_<T>_<suffix>, target lowercased.

    The name class matches what make accepts: its own target list comes from
    $(patsubst VERIFY_%_SRC,%,...), and % spans digits and underscores too. A
    narrower pattern here would drop such a target silently, taking it out of
    every consumer while make still proved it. That reasoning is why this
    pattern lives once rather than once per accessor.

    Reads the continuation-joined text, since a list assignment can wrap.
    """
    return {
        m.group(1).lower(): m.group(2).split()
        for m in re.finditer(
            rf"^VERIFY_([A-Z0-9_]+)_{suffix}\s*:=\s*(.*)$", joined_text(), re.M
        )
    }


def target_sources():
    """{target: source path} for every VERIFY_<T>_SRC, target lowercased.

    One source per target, and a second one is an error rather than a
    truncation. make would accept "VERIFY_X_SRC := a.c b.c" and hand both to
    the prover, so keeping the first word would drop the second out of every
    consumer: out of the include closure, out of the CI scope, out of the
    mutation runner's idea of what the target analyzes. That is the silent
    narrowing this module exists to prevent, so it fails loudly instead.
    Supporting several sources means teaching those consumers first.
    """
    out = {}
    for target, words in _target_var("SRC").items():
        if len(words) != 1:
            raise RuntimeError(
                f"VERIFY_{target.upper()}_SRC names {words}; every consumer of "
                f"this table assumes exactly one source"
            )
        out[target] = words[0]
    return out


def target_scans():
    """{target: [scanned path]} for every VERIFY_<T>_SCAN, target lowercased."""
    return _target_var("SCAN")


def target_cpp_defs():
    """{target: [preprocessor flag]} for every VERIFY_<T>_CPP_DEFS.

    These reach the prover through FRAMAC_CPP_ARGS, so a header included behind
    one of them is part of the target's real input set. Anything reconstructing
    that set with its own preprocessor has to pass them or it sees a different
    file.
    """
    return _target_var("CPP_DEFS")


def recipe_scripts():
    """The scripts/*.py files mk/verify.mk names, recipes and comments alike.

    Every script a recipe runs can change a proof's verdict, and none can
    appear in an include closure, so a scoping decision has to widen when one
    changes. Read rather than hand-listed for the same reason as stub_dir: the
    direction that rots is a NEW script wired into a recipe and forgotten,
    which no test can catch from the other side, because nothing distinguishes
    "not listed" from "cannot affect a proof".

    Matching a mention rather than an invocation is deliberate. Telling the two
    apart means parsing recipe lines, and being wrong there drops a script,
    which narrows. Being wrong the way this is wrong adds a script named only
    in a comment, which widens. Only one of those two errors is safe.
    """
    return set(re.findall(r"scripts/[A-Za-z0-9._-]+\.py", text()))


def discovered_proof_workflows():
    """The .github/workflows files whose text names the proof machinery.

    A workflow that names this module, proof-scope.py, the target list, or a
    verify target either decides which proofs run or checks the thing that
    decides. Both can change a proof's verdict without touching any file an
    include closure can see, and both are invisible from the other side: a
    THIRD workflow that starts running one of these is exactly what nobody
    remembers to add to a hand-kept list. Same reasoning, and same safe
    direction, as recipe_scripts: matching a mention over-widens at worst.

    KNOWN_PROOF_WORKFLOWS is the floor, because the two mechanisms fail in
    opposite directions. The marks are matched against YAML and prose, so
    rewording an invocation, or moving it behind a variable, could silently
    drop a participant; the floor cannot. A hand-kept floor in turn cannot
    grow by itself, which is what the derivation is for. Renaming one of the
    two files is the case the floor makes loud rather than silent: the entry
    stops naming a file in the tree, and the self-test in proof-scope.py
    fails on it instead of quietly scoping without it.
    """
    marks = (
        "scripts/proof-scope.py",
        "scripts/verify-mk.py",
        "print-verify-targets",
        "make verify",
    )
    # Both extensions: Actions accepts .yaml as readily as .yml, and a proof
    # workflow written with the other one would be discovered by neither the
    # glob nor the floor.
    workflows = ROOT / ".github" / "workflows"
    return {
        str(p.relative_to(ROOT))
        for p in sorted(workflows.glob("*.yml")) + sorted(workflows.glob("*.yaml"))
        if any(mark in p.read_text() for mark in marks)
    }


def proof_workflows():
    """discovered_proof_workflows() with the hand-kept floor under it.

    Callers scoping a diff want the union; only the self-test wants the two
    apart, so it can say when the derivation has stopped finding what the
    floor is quietly carrying.
    """
    return KNOWN_PROOF_WORKFLOWS | discovered_proof_workflows()


def proof_actions():
    """The composite actions a proof workflow calls.

    One of them installs Frama-C and the provers, so what it does decides what
    the prover is: the packages, the switch, the cache key. Nothing here can
    appear in an include closure either.

    Derived from the "uses: ./.github/actions/<name>" lines in the workflows
    proof_workflows() already returned, rather than from what the directory
    happens to hold. A glob would pull in an action added for the build or the
    analyzers, and every pull request touching it would then re-prove and
    re-mutate everything, which is the cost this scoping exists to avoid. The
    fallback keeps the safe direction: a directory with actions in it and no
    "uses:" line found means the scan is the thing that broke, so widen.
    """
    used = set()
    for wf in proof_workflows():
        path = ROOT / wf
        if not path.exists():
            continue
        used.update(
            re.findall(
                r"uses:\s*\./(\.github/actions/[A-Za-z0-9._/-]+)", path.read_text()
            )
        )
    present = {
        str(p.relative_to(ROOT))
        for p in sorted((ROOT / ".github" / "actions").rglob("action.y*ml"))
    }
    named = {a for a in present if any(a.startswith(u + "/") or a == u for u in used)}
    return named if named or not present else present


def stub_dir():
    """FRAMAC_STUB_DIR, the directory holding the analyzer's stub headers.

    Read rather than hardcoded by the consumer: those headers reach every
    proof through -include and -I, where no include scan can see them, so a
    scoping decision has to widen on any change under this directory. A copy
    of the name elsewhere would keep pointing at the old one after a rename
    and stop widening, silently.
    """
    m = re.search(r"^FRAMAC_STUB_DIR\s*:=\s*(\S+)", text(), re.M)
    if not m:
        # Loud on purpose. A default here would be a guess about the very path
        # a caller uses to decide what to re-verify.
        raise RuntimeError("FRAMAC_STUB_DIR not found in mk/verify.mk")
    return m.group(1)


def targets():
    """The proof target names, lowercased."""
    return set(target_sources())


def sources():
    """The proved source paths, without their target names."""
    return set(target_sources().values())
