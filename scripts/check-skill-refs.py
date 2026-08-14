#!/usr/bin/env python3
"""Fail when a skill file points at something that no longer exists.

The files under .claude/skills/ are working summaries. Every one of them says
so, and every one closes by naming the tracked file that wins when the two
disagree. That structure only works while the pointers resolve: a reference to
a renamed source file, a deleted make target, or a docs section that has been
retitled does not read as stale, it reads as authoritative and sends the
reader somewhere that does not exist.

This is the failure mode a careful read does not catch. The reference that
prompted this script named src/syscall/sidecar.c, which had been split into
casefold.c and casefold-walk.c; the sentence around it was still true, the
file name was not, and it survived review by a human and by two agents.
Nothing else in the tree checks it, because the skills are documentation and
documentation has no compiler.

What is checked, per file:

  1. Frontmatter: name matches the directory, description is non-empty.
  2. Backticked path-like tokens resolve: exactly one file in the tree ends
     with the path as written, or the path resolves beside the skill itself,
     which is how references/*.md pointers are written. Two files ending that
     way is a failure too, because the reference is ambiguous and wants a
     longer path. A path inside a fenced block is checked the same way; it
     carries no backticks, so it needs a directory component to be recognized.
  3. "make <target>" names a target the makefiles define, including the
     verify-<name> targets that mk/verify.mk instantiates from a template.
  4. A quoted section name attached to the word "section" exists as a heading
     in the docs file nearest it.
  5. A cross-reference to a sibling skill names a skill that exists. Only
     prose counts: an inline span holding a directory component is a path, so
     cmd/elfuse-container is not read as a missing skill.

Only typeset references are checked: a path in backticks, a make command in
backticks or in a fenced block. A file named in running prose, or in the
frontmatter description, is invisible here by design, because the alternative
is guessing which words are meant to be paths.

Three more things are deliberately not checked. Whether the prose is true: a
pointer that resolves can still describe behavior that changed, and this
catches only the mechanical half, which is the half that rots silently. A bare
markdown name at the repo root, which may be a per-developer working doc that
no clone carries; see unverifiable() for why that stays out of the script
rather than becoming a list of somebody's private filenames.

The compact "fd.c/h" shorthand for a pair of files is rejected rather than
ignored. Nothing here can resolve it, so allowing it would leave a reference
that looks checked and is not.

The skills are per-developer files today and may be absent. A missing
.claude/skills/ is not a failure, it is a clone that does not carry them, and
this exits 0 with a note so the check can be wired into a build without
becoming a dependency on untracked files.

Usage:
    check-skill-refs.py [file ...]
    check-skill-refs.py --self-test

Every SKILL.md and every references/*.md under .claude/skills/ is checked,
and so is any extra file named on the command line, which is how a local
routing document that points at the skills gets covered without this script
having to know it exists.

--self-test is the script's other mode, and it reads no skill file at all.
It writes synthetic skills whose references are known broken, runs them
through the same check_file() the normal mode uses, and requires each one to
be rejected. Nothing else separates "nothing is stale" from "nothing is
checked": a pass whose pattern has stopped matching prints the same clean
line as a tree where every reference resolves. make check-skill-refs runs
both modes, self-test first. See self_test() for the case list.
"""

import functools
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SKILL_DIR = ROOT / ".claude" / "skills"

# Directories that hold no source this tree owns. externals/ is vendored for
# evaluation and gitignored; a reference resolving into it would be a false
# pass on a machine that happens to have it checked out.
PRUNE = {".git", "build", "externals", "node_modules", ".claude", ".agents"}

PATH_EXTS = ("c", "h", "S", "md", "tbl", "py", "sh", "mk", "yml")

# elfuse-prefixed names that are test-matrix lanes rather than skills.
LANES = {"elfuse-aarch64", "elfuse-x86_64"}

# Headers the macOS SDK or the compiler supplies. The skills name them as the
# things Frama-C cannot model, so they must not resolve in tree. The shape is
# narrow on purpose: exempting everything under sys/ would wave through a
# stale sys/anything.c as well.
SYSTEM_HEADER_RE = re.compile(r"^(?:sys|Hypervisor)/[A-Za-z0-9_-]+\.h$")

# The leading dot matters: dot directories carry real references, .github for
# the CI job that enforces the proof targets and .claude for the skills
# themselves, and a pattern anchored on an alphanumeric first character skips
# exactly the references that break when either one moves. The trailing group
# drops a shell-function suffix such as test-runner.sh::run.
PATH_RE = re.compile(
    r"`(\.?[A-Za-z0-9_][A-Za-z0-9_./+-]*\.(?:"
    + "|".join(PATH_EXTS)
    + r"))(?:::[A-Za-z_][A-Za-z0-9_]*)?`"
)

# A make target is only a reference when it is typeset as one: backticked
# inline, or a command line inside a fenced block. Plain prose says things like
# "no make target may reference it", and treating that as a target name is how
# a checker earns the reputation that gets it disabled.
#
# Both fenced patterns tolerate leading whitespace: a fence inside a numbered
# step is indented to the step's content column, which is most of the fences
# here, and a column-0 anchor skipped those blocks whole. The width is
# unbounded rather than markdown's top-level three, since a fence one list
# deeper is indented further still.
MAKE_RE = re.compile(r"`make ([a-z][a-z0-9-]*)")
MAKE_FENCED_RE = re.compile(r"^[ \t]*make ([a-z][a-z0-9-]*)", re.M)
FENCE_RE = re.compile(r"^[ \t]*```.*?^[ \t]*```", re.M | re.S)

# A path inside a fenced block carries no backticks, so PATH_RE cannot see it.
# Requiring a directory component keeps this off ordinary words: build/elfuse and
# ./binary have no extension and are not references to a file this tree owns.
FENCED_PATH_RE = re.compile(
    r"(?<![\w./-])((?:[A-Za-z0-9_.+-]+/)+[A-Za-z0-9_.+-]+\.(?:"
    + "|".join(PATH_EXTS)
    + r"))\b"
)

# An inline span holding a directory component is a path, not a skill name, so
# the cross-reference pass must not read cmd/elfuse-container as a skill that
# went missing. A bare `elfuse-guest-abi` span stays visible to it.
INLINE_PATH_RE = re.compile(r"`[^`]*/[^`]*`")

DOCPATH_RE = re.compile(r"docs/[a-z0-9-]+\.md")
QUOTED_RE = re.compile(r'"([^"]+)"')
# A run of section names: the quotes that immediately follow the keyword,
# joined by commas or "and", stopping at the first word that is not one.
RUN_RE = re.compile(r'^\s*"[^"]+"(?:\s*(?:,|and)\s*"[^"]+")*')
PRE_RUN_RE = re.compile(r'"[^"]+"(?:\s*(?:,|and)\s*"[^"]+")*\s*$')
# The "fd.c/h" shorthand for a pair of files. It is not a path, so nothing
# below can resolve it, and a reader reaches for it because the working docs
# use it freely. Rejecting it is cheaper than teaching every pattern here to
# expand it, and it keeps the blind spot from being reintroduced silently.
SHORTHAND_RE = re.compile(r"`([A-Za-z0-9_-]+\.[chS](?:/[chS])+)`")
SKILLREF_RE = re.compile(r"\b(elfuse-[a-z0-9-]+)\b")
HEADING_RE = re.compile(r"^#+\s+(.*?)\s*$", re.M)


def system_header(token):
    """True for a system header the tree is not supposed to contain."""
    return bool(SYSTEM_HEADER_RE.match(token))


def unverifiable(token):
    """True for a reference this script cannot honestly resolve.

    A bare markdown name at the repo root may be a per-developer working doc.
    Those are private by design: they are excluded locally rather than
    gitignored, so a clone has no record that they were ever meant to exist,
    and their absence proves nothing either way. Listing the current ones here
    would put one developer's private filenames into a script everybody reads,
    and would be wrong again the day someone keeps a different set.

    Everything else stays checked, including the include-style paths the build
    resolves through -Isrc, which resolve() handles by matching a trailing
    path segment rather than a whole path.
    """
    return "/" not in token and token.endswith(".md")


def section_refs(sentence):
    """(docs path, section names) pairs a sentence claims.

    Only quotes tied to the word "section" or "sections" count. A sentence can
    name a docs file and quote something else entirely, and treating that
    quote as a section name produces a failure the author cannot act on. Both
    orders occur in practice: 'docs/x.md, section "A"' and 'the "A" section of
    docs/x.md', and one keyword can introduce a list of several. The run stops
    at the first item that is not another quoted name.

    Each run is attributed to the docs file nearest the keyword that
    introduced it. Checking against every docs file the sentence happens to
    name would pass a heading that belongs to the other one, which is a real
    shape here: a sentence can cite internals.md for a mechanism and
    testing.md for its baselines.
    """
    docs = [(m.start(), m.group(0)) for m in DOCPATH_RE.finditer(sentence)]
    if not docs:
        return []

    refs = []
    for m in re.finditer(r"\bsections?\b", sentence):
        names = []
        before = sentence[: m.start()].rstrip()
        prerun = PRE_RUN_RE.search(before)
        if prerun:
            names.extend(QUOTED_RE.findall(prerun.group(0)))
        run = RUN_RE.match(sentence[m.end() :])
        if run:
            names.extend(QUOTED_RE.findall(run.group(0)))
        if names:
            nearest = min(docs, key=lambda d: abs(d[0] - m.start()))[1]
            refs.append((nearest, names))
    return refs


def tree_paths():
    """Every in-tree file, as a repo-relative path string."""
    paths = []
    for path in ROOT.rglob("*"):
        if not path.is_file():
            continue
        rel = path.relative_to(ROOT)
        if PRUNE & set(rel.parts):
            continue
        paths.append(rel.as_posix())
    return paths


def resolve(token, paths):
    """Files a reference could name, matched on a trailing path segment.

    The skills cite files three ways: a repo path, an include-style path the
    build resolves through -Isrc such as core/guest.h, and a bare name. A
    suffix match covers all three and, unlike a basename match, still rejects
    a path that names the wrong directory: src/wrong/guest.c ends no tracked
    path, while a bare guest.c ends exactly one.
    """
    return [p for p in paths if p == token or p.endswith("/" + token)]


def make_targets():
    """Static rule names from the makefiles, plus the generated proof targets.

    mk/verify.mk instantiates verify-<name> from a template, so those names
    exist only after make expands it. print-verify-targets is the list make
    itself reports, which is what CI consumes.
    """
    names = set()
    for mk in [ROOT / "Makefile"] + sorted((ROOT / "mk").glob("*.mk")):
        for line in mk.read_text(errors="replace").splitlines():
            if line.startswith("\t"):
                continue
            if line.startswith(".PHONY:"):
                names.update(line.split(":", 1)[1].split())
                continue
            m = re.match(r"^([A-Za-z0-9._%-]+(?:\s+[A-Za-z0-9._%-]+)*)\s*:(?!=)", line)
            if m:
                names.update(m.group(1).split())
    try:
        out = subprocess.run(
            ["make", "print-verify-targets"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=120,
        )
        names.update(out.stdout.split())
    except (OSError, subprocess.SubprocessError):
        pass
    return names


@functools.lru_cache(maxsize=None)
def headings_of(doc):
    """Heading text of a docs file, backticks stripped, or None if absent.

    Cached because a docs file is cited from several sentences and several
    skills, and re-reading internals.md per citation is the only part of this
    script that would notice.
    """
    path = ROOT / doc
    if not path.exists():
        return None
    body = path.read_text(errors="replace")
    return frozenset(h.replace("`", "") for h in HEADING_RE.findall(body))


def check_frontmatter(path, raw):
    """A skill's declared name has to match the directory that carries it.

    Claude Code loads a skill by directory; a drifted name means the file is
    silently not the skill it says it is, which no reader would notice.
    """
    if path.name != "SKILL.md":
        return []

    head = raw.split("---")[1] if raw.startswith("---") else ""
    name = re.search(r"^name:\s*(\S+)", head, re.M)
    desc = re.search(r"^description:\s*(\S.*)", head, re.M)

    messages = []
    if not name or name.group(1) != path.parent.name:
        messages.append("frontmatter name does not match its directory")
    if not desc:
        messages.append("frontmatter has no description")
    return messages


def local_bases(path):
    """Directories a skill's own pointers are written relative to.

    A skill cites its own reference files as references/x.md from SKILL.md and
    from a sibling under references/, so both the file's directory and the
    skill root have to be tried. The skills directory is third, which is what
    lets one skill cite another's file as elfuse-verify/SKILL.md.
    """
    parent = path.parent
    root = parent.parent if parent.name == "references" else parent
    return [parent, root, SKILL_DIR]


def check_paths(text, raw, paths, bases):
    """Every typeset path names exactly one file in the tree.

    @bases are the directories a skill's own pointers resolve against, which is
    how references/*.md is written. .claude stays pruned from the tree walk, so
    no unrelated file can satisfy one of those by accident.
    """
    messages = []
    for token in sorted(set(SHORTHAND_RE.findall(text))):
        names = token.split("/")
        stem = names[0].rsplit(".", 1)[0]
        spelled = " and ".join(f"`{stem}.{ext}`" for ext in [names[0][-1]] + names[1:])
        messages.append(f"writes {token}, which nothing resolves; write {spelled}")

    tokens = set(PATH_RE.findall(text))
    for block in FENCE_RE.findall(raw):
        tokens.update(FENCED_PATH_RE.findall(block))

    # is_file() rather than exists() throughout: resolve() matches against
    # tree_paths(), which is files only, so a directory that happens to be
    # named like a reference would otherwise satisfy one here and nowhere else.
    for token in sorted(tokens):
        if unverifiable(token) or (ROOT / token).is_file():
            continue
        if any((b / token).is_file() for b in bases):
            continue
        # A reference has to identify one file. Accepting "something in the
        # tree ends this way" would pass a deleted src/a/foo.c because an
        # unrelated src/b/foo.c survived, which is the exact failure this
        # script exists to catch. Two matches is not a pass either: the
        # reference is ambiguous and wants a longer path.
        matches = resolve(token, paths)
        if len(matches) == 1:
            continue
        # Consulted after resolution so a stub the tree really does carry,
        # like the Hypervisor header under frama-c-stubs/, is verified rather
        # than waved through by the exemption.
        if system_header(token):
            continue
        if len(matches) > 1:
            where = ", ".join(sorted(matches)[:3])
            messages.append(
                f"{token} is ambiguous, {len(matches)} files carry that "
                f"name ({where}); write the path"
            )
            continue
        messages.append(f"references {token}, which does not exist")
    return messages


def check_targets(text, raw, targets):
    """Every typeset make command names a rule the makefiles define."""
    named = set(MAKE_RE.findall(text))
    for block in FENCE_RE.findall(raw):
        named.update(MAKE_FENCED_RE.findall(block))

    messages = []
    for target in sorted(named):
        # "make verify-<name>" is a form, not a target: the capture stops at
        # the angle bracket and leaves a trailing hyphen no real rule has.
        if target.endswith("-"):
            continue
        if target not in targets:
            messages.append(f"names 'make {target}', which no makefile defines")
    return messages


def check_sections(text):
    """Every quoted section name is a heading in the docs file it cites.

    The match is exact once the heading's backticks are stripped: accepting a
    prefix would pass "Validation Strategy" for a section actually titled
    "Validation Strategy By Change Type", which is the kind of near-miss that
    sends a reader to the wrong table.
    """
    messages = []
    for sentence in text.split(". "):
        for doc, names in section_refs(sentence):
            known = headings_of(doc)
            if known is None:
                continue
            for name in names:
                if name not in known:
                    messages.append(f'no section "{name}" in {doc}')
    return messages


def check_skill_refs(text, skills):
    """Every sibling skill named in prose exists as a skill directory."""
    messages = []
    for ref in sorted(set(SKILLREF_RE.findall(INLINE_PATH_RE.sub(" ", text)))):
        if ref in LANES or ref in skills:
            continue
        messages.append(f"refers to skill {ref}, which does not exist")
    return messages


def check_file(path, paths, targets, skills, errors):
    """Run every pass over one file, prefixing each finding with its path."""
    raw = path.read_text(errors="replace")
    # An explicitly named file may sit outside the repo, where relative_to
    # raises rather than returning something printable.
    try:
        rel = path.relative_to(ROOT)
    except ValueError:
        rel = path

    # Prose wrapped at 79 columns splits references across lines: `make\n
    # check-format` and a section name broken mid-title are the same reference
    # a reader sees, so every pass below runs on the text with runs of
    # whitespace collapsed. Checking the raw text instead silently skips any
    # reference unlucky enough to land on a line boundary, which is most of
    # them in a file this shape. Frontmatter stays raw because its regexes are
    # line-anchored, and the fenced-block scan needs real line starts.
    text = re.sub(r"\s+", " ", raw)

    messages = (
        check_frontmatter(path, raw)
        + check_paths(text, raw, paths, local_bases(path))
        + check_targets(text, raw, targets)
        + check_sections(text)
        + check_skill_refs(text, skills)
    )
    errors.extend(f"{rel}: {message}" for message in messages)


# Each case is (what it exercises, body, substring of the expected error or
# None when the body must produce no error at all).
SELF_TEST_CASES = [
    ("wrong directory", "See `src/wrong/guest.c` for it.", "does not exist"),
    ("ambiguous bare name", "See `fuse.h` for it.", "is ambiguous"),
    ("dead make target", "Run `make\ncheck-fmt` first.", "check-fmt"),
    ("non-header under sys/", "Frama-C lacks `sys/bogus.c`.", "does not exist"),
    (
        "section near-miss",
        '`docs/testing.md`, section "Validation Strategy" is the table.',
        "no section",
    ),
    (
        "stale first pre-keyword section",
        'the "Not A Real Heading" and "Build Requirements" sections of '
        "`docs/testing.md` are useful.",
        "no section",
    ),
    (
        "section attributed to the wrong doc",
        "`docs/internals.md` has it and `docs/testing.md`, section "
        '"Memory Layout", does not.',
        "no section",
    ),
    ("unknown sibling skill", "See the `elfuse-nope` skill.", "refers to skill"),
    (
        "unresolvable shorthand",
        "Helpers in `fd.c/h` classify it.",
        "write `fd.c` and `fd.h`",
    ),
    (
        "sibling reference file that is missing",
        "Load `references/gone.md` at step 3.",
        "does not exist",
    ),
    ("path in a fenced block", "```sh\npython3 scripts/nope.py\n```", "does not exist"),
    (
        "path in a fenced block inside a numbered step",
        "1. Run it:\n\n   ```sh\n   python3 scripts/nope.py\n   ```",
        "does not exist",
    ),
    (
        "make target in a fenced block inside a numbered step",
        "1. Run it:\n\n   ```sh\n   make check-fmt\n   ```",
        "check-fmt",
    ),
    (
        "sibling reference that resolves to a directory",
        "Load `references/dirlike.md` at step 3.",
        "does not exist",
    ),
    ("valid repo path", "See `src/core/guest.c`.", None),
    (
        "sibling reference file beside the skill",
        "Load `references/fixture.md` at step 3.",
        None,
    ),
    (
        "valid path in a fenced block",
        "```sh\npython3 scripts/check-mutants.py\n```",
        None,
    ),
    (
        "valid path in a fenced block inside a numbered step",
        "1. Run it:\n\n   ```sh\n   python3 scripts/check-mutants.py\n   ```",
        None,
    ),
    (
        "repo path that looks like a skill name",
        "The Go `cmd/elfuse-container` CLI.",
        None,
    ),
    ("valid include-style path", "Include it as `core/guest.h`.", None),
    ("valid system header", "Frama-C cannot model `sys/mount.h`.", None),
    ("valid line-wrapped target", "Run `make\ncheck-format` first.", None),
    ("make in prose is not a target", "No make target may reference it.", None),
    ("target placeholder", "Run `make verify-<name>` for one.", None),
    ("private root markdown", "`CLAUDE.md` is untracked.", None),
    (
        "unrelated quote after a real section",
        '`docs/testing.md`, section "Validation Strategy By Change Type", is '
        'a "wild guess" table.',
        None,
    ),
    (
        "two docs, section attributed correctly",
        '`docs/internals.md` section "Memory Layout" has the map, and '
        "`docs/testing.md` has the baselines.",
        None,
    ),
]

FRONTMATTER = "---\nname: {name}\ndescription: fixture\n---\n\n"


def self_test():
    """Assert this script still rejects each class of stale reference.

    Every case is a bug this script shipped with: a reference broken across
    a line boundary was invisible to every pass, a section name that was a
    prefix of a real heading passed as a match. The negative cases carry the
    same weight, since a checker that flags valid prose gets switched off and
    then protects nothing.
    """
    paths, targets, skills = tree_paths(), make_targets(), {"elfuse-syscall"}
    failures = []

    with tempfile.TemporaryDirectory() as tmp:
        skill_dir = pathlib.Path(tmp) / "elfuse-syscall"
        (skill_dir / "references").mkdir(parents=True)
        (skill_dir / "references" / "fixture.md").write_text("fixture\n")
        # A directory named like a reference: the case that separates
        # is_file() from exists() in check_paths().
        (skill_dir / "references" / "dirlike.md").mkdir()
        fixture = skill_dir / "SKILL.md"

        for label, body, expect in SELF_TEST_CASES:
            fixture.write_text(FRONTMATTER.format(name="elfuse-syscall") + body + "\n")
            errors = []
            check_file(fixture, paths, targets, skills, errors)
            found = " | ".join(e.split(": ", 1)[-1] for e in errors)
            if expect is None and errors:
                failures.append(f"{label}: expected no error, got: {found}")
            elif expect is not None and expect not in found:
                failures.append(f"{label}: expected {expect!r}, got: {found or 'none'}")

        # A reference file cites its siblings the way SKILL.md does, from one
        # directory deeper, so it needs a fixture that actually sits there.
        sibling = skill_dir / "references" / "sibling.md"
        sibling.write_text("See `references/fixture.md`.\n")
        errors = []
        check_file(sibling, paths, targets, skills, errors)
        if errors:
            failures.append(f"reference sibling: expected no error, got: {errors}")

        # Frontmatter drift needs its own directory: the name is checked
        # against the directory the file sits in.
        other = pathlib.Path(tmp) / "elfuse-other"
        other.mkdir()
        drift = other / "SKILL.md"
        drift.write_text(FRONTMATTER.format(name="elfuse-syscall") + "Body.\n")
        errors = []
        check_file(drift, paths, targets, skills, errors)
        if not any("frontmatter name" in e for e in errors):
            failures.append("frontmatter drift: expected a name mismatch, got none")

    total = len(SELF_TEST_CASES) + 2
    if failures:
        print(
            f"  {len(failures)} of {total} self-test case(s) failed:", file=sys.stderr
        )
        for f in failures:
            print(f"    {f}", file=sys.stderr)
        return 1
    print(f"  self-test: {total} cases, all pass")
    return 0


def main():
    if "--self-test" in sys.argv[1:]:
        print("  SKILL   self-test", flush=True)
        return self_test()

    print("  SKILL   .claude/skills/", flush=True)

    skills, files = set(), []
    if SKILL_DIR.is_dir():
        skills = {d.name for d in SKILL_DIR.iterdir() if (d / "SKILL.md").is_file()}
        files = sorted(SKILL_DIR.glob("*/SKILL.md")) + sorted(
            SKILL_DIR.glob("*/references/*.md")
        )

    # A routing file that points at the skills gets checked the same way, but
    # only when its owner names it. Whether one exists, and what it is called,
    # is local to a working copy and not this script's business. A named file
    # is checked even in a clone with no skills directory, because that is
    # where its references are most likely to have gone stale.
    for extra in sys.argv[1:]:
        path = pathlib.Path(extra)
        if not path.is_absolute():
            path = ROOT / extra
        if not path.is_file():
            print(f"  {extra}: not a file", file=sys.stderr)
            return 1
        files.insert(0, path)

    if not files:
        print("  no .claude/skills/ in this clone; nothing to check")
        return 0

    paths, targets, errors = tree_paths(), make_targets(), []
    for path in files:
        check_file(path, paths, targets, skills, errors)

    # A file may name the same missing thing twice; report each once.
    errors = list(dict.fromkeys(errors))
    if errors:
        print(f"  {len(errors)} stale reference(s):", file=sys.stderr)
        for e in errors:
            print(f"    {e}", file=sys.stderr)
        return 1

    print(
        f"  {len(files)} file(s), every path, target, section, and "
        "cross-reference resolves"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
