"""The one reader of mk/analysis.mk's VERIFY_<T>_* variables.

check-mutants.py and check-proof-targets.py both need the proof-target table,
and each had grown its own regex over the same lines: three patterns spelling
"VERIFY_<T>_SRC" three ways, differing only in which capture group they kept.
They agree today, so nothing was broken; they are three places to update when
the variable naming changes, in a pair of scripts whose entire job is catching
exactly that kind of drift somewhere else.

Load this instead of re-deriving it. The filename is kebab-case per CLAUDE.md,
which no import statement can name, so both consumers pull it in by path with
importlib; see _load_analysis_mk in either script.
"""

import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent
ANALYSIS_MK = ROOT / "mk" / "analysis.mk"


def text():
    """mk/analysis.mk as text."""
    return ANALYSIS_MK.read_text()


def target_sources():
    """{target: source path} for every VERIFY_<T>_SRC, target lowercased.

    The name class matches what make accepts: its own list comes from
    $(patsubst VERIFY_%_SRC,%,...), and % spans digits and underscores too. A
    narrower pattern here would drop such a target silently, taking its source
    and its mutations out of every consumer while make still proved it.
    """
    return {
        m.group(1).lower(): m.group(2)
        for m in re.finditer(r"^VERIFY_([A-Z0-9_]+)_SRC\s*:=\s*(\S+)", text(),
                             re.M)
    }


def targets():
    """The proof target names, lowercased."""
    return set(target_sources())


def sources():
    """The proved source paths, without their target names."""
    return set(target_sources().values())
