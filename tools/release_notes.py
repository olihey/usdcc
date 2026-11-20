"""Prints one release's section of CHANGELOG.md, for the release's notes on GitHub.

A section starts at its `## <version>` heading (`## 26.12.1 - 2026-12-03`) and
ends before the next one. The heading itself is left out: the release's page has
the version as its title. The tag may have a leading v, as release.yml allows.
Links to files in the repository (docs/PEERS.md) become links to them at the tag.
Wrapped lines are joined: GitHub shows a line break in a release's notes as one,
which it doesn't in a file.
Exits with 1, and says so, when the changelog has no section for the version, so
a release isn't made with empty notes (docs/RELEASE.md section 8).

Under the section goes "Known issues": the repository's open issues labelled
known-issue, a line each, read with gh (docs/RELEASE.md section 9). Left out when
there are none. Exits with 1 when gh can't list them, unless --no-known-issues.

    python tools/release_notes.py 26.12.1 > notes.md
    gh release create 26.12.1 --target master --title 26.12.1 --notes-file notes.md --generate-notes
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

CHANGELOG = Path(__file__).resolve().parent.parent / "CHANGELOG.md"
REPOSITORY = "https://github.com/olihey/usdcc"
KNOWN_ISSUE_LABEL = "known-issue"


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


def unwrap(text: str) -> str:
    """`text` with each paragraph and list item on one line; code blocks stay as they are."""
    lines: list[str] = []
    in_code = False
    for line in text.splitlines():
        words = line.strip()
        if words.startswith("```"):
            in_code = not in_code
            lines.append(line)
        elif in_code or not words or not lines or not lines[-1].strip():
            lines.append(line)
        elif re.match(r"[-*+] |\d+[.)] |#|\||>", words) or lines[-1].lstrip().startswith(("#", "|", "```")):
            lines.append(line)
        else:
            lines[-1] += " " + words
    return "\n".join(lines) + "\n"


def known_issues(repository: str) -> list[dict]:
    """The open issues labelled known-issue in `repository` (its URL), oldest first."""
    result = subprocess.run(
        ["gh", "issue", "list", "--repo", repository.removeprefix("https://"), "--label", KNOWN_ISSUE_LABEL,
         "--state", "open", "--limit", "500", "--json", "number,title,url"],
        capture_output=True, text=True, encoding="utf-8")
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or f"gh exited with {result.returncode}")
    return sorted(json.loads(result.stdout), key=lambda issue: issue["number"])


def known_issues_section(issues: list[dict]) -> str:
    """The notes' "Known issues", a line per issue; empty without any."""
    if not issues:
        return ""
    lines = ["### Known issues", ""]
    lines += [f"- {issue['title']} ([#{issue['number']}]({issue['url']}))" for issue in issues]
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("version", help="the release's tag, such as 26.12.1")
    parser.add_argument("--changelog", type=Path, default=CHANGELOG, help="the file to read (CHANGELOG.md)")
    parser.add_argument("--repository", default=REPOSITORY, help="where the changelog's relative links point")
    parser.add_argument("--no-known-issues", action="store_true",
                        help=f"leave out the open issues labelled {KNOWN_ISSUE_LABEL} (no gh needed)")
    args = parser.parse_args()

    notes = section(args.changelog.read_text(encoding="utf-8"), args.version)
    if notes is None:
        print(f"{args.changelog.name} has no section '## {args.version.removeprefix('v')}'.", file=sys.stderr)
        return 1
    # The changelog's links are relative to the repository, and a release's page isn't
    # in it: point them at the file as it is in the release.
    tag = args.version
    notes = re.sub(r"\]\((?![a-z][a-z0-9+.-]*:|#|/)([^)]+)\)", rf"]({args.repository}/blob/{tag}/\1)", notes)
    notes = unwrap(notes)
    if not args.no_known_issues:
        try:
            issues = known_issues(args.repository)
        except (OSError, RuntimeError) as error:
            print(f"Couldn't list the issues labelled {KNOWN_ISSUE_LABEL} with gh: {error}\n"
                  "Pass --no-known-issues to leave them out.", file=sys.stderr)
            return 1
        if issues:
            notes += "\n" + known_issues_section(issues)
    sys.stdout.buffer.write(notes.encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
