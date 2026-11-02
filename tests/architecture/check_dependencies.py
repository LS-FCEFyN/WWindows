#!/usr/bin/env python3
"""Architecture conformance checks.

Run as part of the CTest suite (`wwindows_architecture_check`).

Three rules are enforced:

  1. Layer dependency direction -- core must not know about XCB or the X11
     adapter; input must additionally not know about core.
  2. Policy liveness -- every function declared in a `*/policy/` header must be
     referenced from production code.
  3. Composition-root containment -- only src/application.cpp may name a
     concrete adapter type.

The previous version substring-matched raw file text, which was wrong in both
directions: a comment mentioning "xcb/" failed the build, while a genuine
`#include <xcb/xcb.h>` in a header under include/ was never looked at, because
only src/ was scanned. This version parses actual #include directives with
comments and string literals stripped, and scans headers as well as sources.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
INCLUDE = ROOT / "include"

CXX_SUFFIXES = {".cpp", ".hpp", ".h", ".cc"}

BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
LINE_COMMENT = re.compile(r"//[^\n]*")
STRING_LITERAL = re.compile(r'"(?:[^"\\\n]|\\.)*"')
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)


def strip_noise(text: str) -> str:
    """Remove comments so they cannot trigger or mask a rule."""
    text = BLOCK_COMMENT.sub(" ", text)
    text = LINE_COMMENT.sub(" ", text)
    return text


def includes_of(path: Path) -> list[str]:
    return INCLUDE_RE.findall(strip_noise(path.read_text()))


def code_of(path: Path) -> str:
    """File text with comments and string literals removed."""
    return STRING_LITERAL.sub('""', strip_noise(path.read_text()))


def sources_under(*roots: Path):
    for root in roots:
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if path.suffix in CXX_SUFFIXES:
                yield path


violations: list[str] = []


def rel(path: Path) -> str:
    return str(path.relative_to(ROOT))


# --- rule 1: layer dependency direction -------------------------------------
# Matched against #include targets only, so prose can say whatever it likes.
FORBIDDEN = {
    "core": (
        ("xcb/", "xkbcommon/", "adapters/x11/", "platform/xwindows"),
        (SRC / "core", INCLUDE / "wwindows" / "core"),
    ),
    "input": (
        ("xcb/", "adapters/x11/", "platform/xwindows", "wwindows/core/"),
        (SRC / "input", INCLUDE / "wwindows" / "input"),
    ),
}

for layer, (forbidden_prefixes, roots) in FORBIDDEN.items():
    for path in sources_under(*roots):
        for inc in includes_of(path):
            for bad in forbidden_prefixes:
                if bad in inc:
                    violations.append(
                        f"{rel(path)}: {layer} layer may not include '{inc}'"
                    )

# --- rule 2: policy liveness ------------------------------------------------
# A policy that production code never calls is dead code whose unit tests
# report coverage the compositor does not actually have. Four such functions
# existed before this rule was added.
POLICY_DECL = re.compile(
    r"^\s*(?:inline\s+|constexpr\s+|static\s+)*"
    r"(?:[\w:<>,\s*&]+?)\s+"       # return type
    r"(\w+)\s*\([^;{]*\)\s*(?:noexcept)?\s*[;{]",
    re.MULTILINE,
)

KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "catch"}

policy_headers = [
    p for p in sources_under(INCLUDE) if "policy" in p.parts and p.suffix == ".hpp"
]

production_text = "\n".join(
    code_of(p) for p in sources_under(SRC) if "policy" not in p.parts
)
# A policy may legitimately be a helper for another policy rather than a
# direct entry point, so calls from inside the policy layer count as use too.
policy_text = "\n".join(code_of(p) for p in policy_headers)


def called_in(name: str, text: str) -> bool:
    return re.search(rf"\b{re.escape(name)}\s*\(", text) is not None


for header in policy_headers:
    body = code_of(header)
    names = [n for n in sorted(set(POLICY_DECL.findall(body))) if n not in KEYWORDS]
    if not names:
        continue

    # Every declared policy must be reachable: called from production, or
    # from another policy that is itself reachable.
    for name in names:
        # Strip this function's own definition before looking for callers,
        # so a recursive-looking self match cannot mark it live.
        others = policy_text.replace(f"{name}(", "", 1)
        if not (called_in(name, production_text) or called_in(name, others)):
            violations.append(
                f"{rel(header)}: policy '{name}' is never called by production "
                f"code -- either wire it in or delete it (a tested-but-unused "
                f"policy overstates real coverage)"
            )

    # ...and the header as a whole must have at least one production caller,
    # otherwise the entire group is dead however tidily it calls itself.
    if not any(called_in(name, production_text) for name in names):
        violations.append(
            f"{rel(header)}: no policy in this header is called by production "
            f"code -- the whole header is dead"
        )

# --- rule 3: composition-root containment -----------------------------------
# Only the composition root may name a concrete adapter.
CONCRETE_ADAPTERS = ("X11Backend", "I3Watcher")
COMPOSITION_ROOT = SRC / "application.cpp"

for path in sources_under(SRC):
    if path == COMPOSITION_ROOT or "adapters" in path.parts:
        continue
    body = code_of(path)
    for adapter in CONCRETE_ADAPTERS:
        if re.search(rf"\b{adapter}\b", body):
            violations.append(
                f"{rel(path)}: names concrete adapter '{adapter}'; only "
                f"{rel(COMPOSITION_ROOT)} may do so"
            )

# --- report -----------------------------------------------------------------
if violations:
    print("\n".join(violations), file=sys.stderr)
    raise SystemExit(1)

print(
    f"architecture checks passed "
    f"({len(policy_headers)} policy header(s), "
    f"{sum(1 for _ in sources_under(SRC, INCLUDE))} file(s) scanned)"
)
