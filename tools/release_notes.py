"""Prints one release's section of CHANGELOG.md, for the release's notes on GitHub.

A section starts at its `## <version>` heading (`## 2026.11.20 - 2026-11-20`) and
ends before the next one. The heading itself is left out: the release's page has
the version as its title. The tag may have a leading v, as release.yml allows.
Links to files in the repository (docs/PEERS.md) become links to them at the tag.
Exits with 1, and says so, when the changelog has no section for the version, so
a release isn't made with empty notes (docs/RELEASE.md section 8).

    python tools/release_notes.py 2026.11.20 > notes.md
    gh release create 2026.11.20 --target master --title 2026.11.20 --notes-file notes.md --generate-notes
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

CHANGELOG = Path(__file__).resolve().parent.parent / "CHANGELOG.md"
REPOSITORY = "https://github.com/olihey/usdcc"


def section(text: str, version: str) -> str | None:
    """The body of `version`'s section in the changelog `text`, or None without one."""
    version = version.removeprefix("v")
    body: list[str] | None = None
    for line in text.splitlines():
        if line.startswith("## "):
            if body is not None:
                break
            words = line[3:].split()
            if words and words[0].removeprefix("v") == version:
                body = []
        elif body is not None:
            body.append(line)
    if body is None:
        return None
    return "\n".join(body).strip() + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("version", help="the release's tag, such as 2026.11.20")
    parser.add_argument("--changelog", type=Path, default=CHANGELOG, help="the file to read (CHANGELOG.md)")
    parser.add_argument("--repository", default=REPOSITORY, help="where the changelog's relative links point")
    args = parser.parse_args()

    notes = section(args.changelog.read_text(encoding="utf-8"), args.version)
    if notes is None:
        print(f"{args.changelog.name} has no section '## {args.version.removeprefix('v')}'.", file=sys.stderr)
        return 1
    # The changelog's links are relative to the repository, and a release's page isn't
    # in it: point them at the file as it is in the release.
    tag = args.version
    notes = re.sub(r"\]\((?![a-z][a-z0-9+.-]*:|#|/)([^)]+)\)", rf"]({args.repository}/blob/{tag}/\1)", notes)
    sys.stdout.buffer.write(notes.encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
