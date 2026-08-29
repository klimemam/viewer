#!/usr/bin/env python3
"""Check the documentation graph, migration map, and numbered references.

The checker deliberately uses only the Python standard library so the same
rules run locally and in GitHub Actions.  The machine-readable policy lives in
``docs/README.md``; this file implements it rather than keeping a second list.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import io
import os
import re
import subprocess
import sys
import tempfile
import urllib.parse
from dataclasses import dataclass
from pathlib import Path, PurePosixPath


MOVE_START = "<!-- DOCS-MOVE-MAP:START -->"
MOVE_END = "<!-- DOCS-MOVE-MAP:END -->"
SPLIT_START = "<!-- DOCS-SPLIT-HISTORY:START -->"
SPLIT_END = "<!-- DOCS-SPLIT-HISTORY:END -->"
EXC_START = "<!-- DOCS-PATH-EXCEPTIONS:START -->"
EXC_END = "<!-- DOCS-PATH-EXCEPTIONS:END -->"

MD_LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
REPO_DOC_RE = re.compile(r"(?<![A-Za-z0-9_.-])(docs/[A-Za-z0-9_.\-/]+\.(?:md|csv|html))")
BARE_DOC_RE = re.compile(
    r"(?<![A-Za-z0-9_./-])([A-Za-z0-9_.-]+\.(?:md|csv|html))(?![A-Za-z0-9_./-])"
)
SECTION_RE = re.compile(
    r"(?P<path>docs/[A-Za-z0-9_.\-/]+\.md)\s+(?:§|#)(?P<section>[0-9]+(?:\.[0-9]+)*)"
)
FENCE_RE = re.compile(r"^\s*(```|~~~)")
INLINE_CODE_RE = re.compile(r"(?<!`)`[^`\n]*`(?!`)")
TABLE_LINK_RE = re.compile(r"^\[([^\]]+)\]\(([^)]+)\)$")
TEXT_SUFFIXES = {
    ".c", ".cc", ".cmake", ".cpp", ".csv", ".h", ".hpp", ".html",
    ".inc", ".json", ".jsonc", ".md", ".ps1", ".py", ".sh", ".txt",
    ".yaml", ".yml",
}


@dataclass(frozen=True)
class Problem:
    code: str
    path: str
    detail: str

    def __str__(self) -> str:
        return f"{self.code}: {self.path}: {self.detail}"


@dataclass(frozen=True)
class Move:
    old: str
    current: str
    kind: str
    stub: str


def _posix(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def _tracked_and_untracked(root: Path) -> list[Path]:
    """Return repository files, including not-yet-staged files during a move."""
    try:
        cp = subprocess.run(
            ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
            cwd=root,
            check=True,
            stdout=subprocess.PIPE,
        )
        names = [n for n in cp.stdout.decode("utf-8", "surrogateescape").split("\0") if n]
        return [root / PurePosixPath(n) for n in names if (root / PurePosixPath(n)).is_file()]
    except (OSError, subprocess.CalledProcessError):
        return [p for p in root.rglob("*") if p.is_file() and ".git" not in p.parts]


def _text(path: Path) -> str | None:
    try:
        return path.read_text(encoding="utf-8-sig")
    except (OSError, UnicodeDecodeError):
        return None


def _without_fences(text: str) -> str:
    out: list[str] = []
    in_fence = False
    fence = ""
    for line in text.splitlines():
        m = FENCE_RE.match(line)
        if m:
            mark = m.group(1)
            if not in_fence:
                in_fence, fence = True, mark[0]
            elif mark[0] == fence:
                in_fence = False
            continue
        if not in_fence:
            out.append(line)
    return "\n".join(out)


def _without_markdown_code(text: str) -> str:
    """Remove fenced and one-backtick code before parsing Markdown links.

    A string such as ``[label](target)`` inside a code span is an example, not
    a navigable link. Bare ``docs/...`` references are handled separately so
    that source comments and prose still participate in the repository-wide
    path check.
    """
    return INLINE_CODE_RE.sub("", _without_fences(text))


def _between(text: str, start: str, end: str) -> str | None:
    if start not in text or end not in text:
        return None
    body = text.split(start, 1)[1].split(end, 1)[0]
    return body


def _table_cell_path(cell: str) -> str:
    cell = cell.strip().strip("`")
    m = TABLE_LINK_RE.match(cell)
    if m:
        label, target = m.groups()
        return label.strip() if label.strip().startswith("docs/") else target.strip()
    return cell


def _table_rows(block: str) -> list[list[str]]:
    rows: list[list[str]] = []
    for line in block.splitlines():
        if not line.lstrip().startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if not cells or all(re.fullmatch(r":?-{3,}:?", c or "---") for c in cells):
            continue
        rows.append(cells)
    return rows


def _read_policy(portal: Path) -> tuple[list[Move], set[str], list[Problem]]:
    problems: list[Problem] = []
    text = _text(portal) or ""
    move_block = _between(text, MOVE_START, MOVE_END)
    exc_block = _between(text, EXC_START, EXC_END)
    if move_block is None:
        problems.append(Problem("POLICY_MARKER", "docs/README.md", "move-map markers are missing"))
        move_block = ""
    if exc_block is None:
        problems.append(Problem("POLICY_MARKER", "docs/README.md", "exception markers are missing"))
        exc_block = ""

    moves: list[Move] = []
    for cells in _table_rows(move_block):
        if cells[0] in {"旧パス", "旧"}:
            continue
        if len(cells) < 4:
            problems.append(Problem("MOVE_ROW", "docs/README.md", f"expected 4 columns: {cells!r}"))
            continue
        old, current = _table_cell_path(cells[0]), _table_cell_path(cells[1])
        stub = cells[3].strip().lower().strip("`")
        if not old.startswith("docs/") or not current.startswith("docs/"):
            problems.append(Problem("MOVE_PATH", "docs/README.md", f"repo-relative docs paths required: {old!r}, {current!r}"))
        if stub not in {"required", "none"}:
            problems.append(Problem("MOVE_STUB", "docs/README.md", f"stub must be required/none for {old}"))
        moves.append(Move(old, current, cells[2].strip(), stub))

    seen_old: set[str] = set()
    seen_current: set[str] = set()
    for move in moves:
        if move.old in seen_old:
            problems.append(Problem("MOVE_DUPLICATE", "docs/README.md", f"old path appears twice: {move.old}"))
        if move.current in seen_current:
            problems.append(Problem("MOVE_DUPLICATE", "docs/README.md", f"current path appears twice: {move.current}"))
        seen_old.add(move.old)
        seen_current.add(move.current)

    exceptions: set[str] = set()
    for cells in _table_rows(exc_block):
        if not cells or cells[0] in {"パス", "path"}:
            continue
        path = _table_cell_path(cells[0])
        if path:
            exceptions.add(path)
    return moves, exceptions, problems


def _resolve_link(source: Path, raw: str, root: Path) -> tuple[Path | None, str]:
    raw = raw.strip()
    if raw.startswith("<") and ">" in raw:
        raw = raw[1:raw.index(">")]
    else:
        raw = raw.split(maxsplit=1)[0]
    if re.match(r"^[A-Za-z][A-Za-z0-9+.-]*:", raw) or raw.startswith("//"):
        return None, ""
    path_part, _, fragment = raw.partition("#")
    path_part = urllib.parse.unquote(path_part).split("?", 1)[0]
    if not path_part:
        return source, fragment
    if path_part.startswith("/"):
        target = root / path_part.lstrip("/")
    else:
        target = source.parent / PurePosixPath(path_part)
    try:
        target = target.resolve()
        target.relative_to(root.resolve())
    except (OSError, ValueError):
        return root / "__outside_repository__", fragment
    return target, fragment


def _heading_sections(text: str) -> set[str]:
    sections: set[str] = set()
    for line in text.splitlines():
        m = re.match(r"^#{1,6}\s+([0-9]+(?:\.[0-9]+)*)\b", line)
        if m:
            sections.add(m.group(1))
    return sections


def _github_heading_slug(heading: str) -> str:
    """Return the GitHub-style fragment used by the headings in this repo."""
    # A link inside a heading contributes its label, not its destination.
    heading = re.sub(r"!?\[([^\]]*)\]\([^)]+\)", r"\1", heading)
    heading = re.sub(r"<[^>]+>", "", heading)
    heading = heading.replace("`", "").replace("*", "").replace("_", "")
    # GitHub removes punctuation and turns each whitespace character into one
    # hyphen.  Keeping the replacement uncollapsed matters for headings such
    # as "series (系列) —— ...", whose fragment contains adjacent hyphens.
    kept = "".join(ch for ch in heading if ch.isalnum() or ch in "-_" or ch.isspace())
    return re.sub(r"\s", "-", kept.lower()).strip("-")


def _heading_fragments(text: str) -> set[str]:
    fragments: set[str] = set()
    counts: dict[str, int] = {}
    for line in text.splitlines():
        match = re.match(r"^#{1,6}\s+(.+?)\s*#*\s*$", line)
        if not match:
            continue
        base = _github_heading_slug(match.group(1))
        if not base:
            continue
        count = counts.get(base, 0)
        counts[base] = count + 1
        fragments.add(base if count == 0 else f"{base}-{count}")
    return fragments


def _stub_target(path: Path, root: Path, move: Move) -> tuple[str | None, str | None]:
    text = _text(path)
    if text is None:
        return None, "not UTF-8"
    # Permit the conventional one final newline, but do not hide a fourth,
    # blank line by stripping every trailing newline.
    if text.endswith("\n"):
        text = text[:-1]
    lines = text.split("\n")
    if len(lines) != 3:
        return None, "stub must be exactly three lines in the documented format"

    target_path = root / PurePosixPath(move.current)
    relative = os.path.relpath(target_path, path.parent).replace("\\", "/")
    expected_head = f"# (moved) {path.name} → {relative}"
    if lines[0] != expected_head:
        return None, f"stub line 1 must be {expected_head!r}"

    date_text = lines[1].removeprefix("移動日: ")
    if lines[1] != f"移動日: {date_text}" or not re.fullmatch(r"\d{4}-\d{2}-\d{2}", date_text):
        return None, "stub line 2 must contain an ISO date"
    try:
        dt.date.fromisoformat(date_text)
    except ValueError:
        return None, "stub line 2 contains an invalid calendar date"

    role = "記録" if move.current.startswith("docs/background/") else "正典"
    expected_body = f"本文はここにありません。{role}は [{relative}]({relative}) です。"
    if lines[2] != expected_body:
        return None, f"stub line 3 must be {expected_body!r}"

    target, _ = _resolve_link(path, relative, root)
    if target is None:
        return None, "stub target must be a local link"
    return _posix(target, root), None


def _is_historical_source(source: str) -> bool:
    """Return whether old paths are evidence rather than live navigation."""
    return (source == "docs/tasks.csv" or
            source.startswith("docs/background/") or
            source.startswith("docs/verification/results/"))


def _inside_block(text: str, offset: int, start: str, end: str) -> bool:
    """Return whether offset is inside one marked policy block."""
    first = text.find(start)
    last = text.find(end, first + len(start)) if first >= 0 else -1
    return first >= 0 and last >= 0 and first <= offset < last + len(end)


def _inside_portal_history(text: str, offset: int) -> bool:
    """Limit portal old-path exceptions to the two explicit ledgers."""
    return (_inside_block(text, offset, MOVE_START, MOVE_END) or
            _inside_block(text, offset, SPLIT_START, SPLIT_END))


def _is_current_surface(source: str) -> bool:
    """Return whether a source is part of the current documentation surface."""
    return (source in {"README.md", "ARCHITECTURE.md"} or
            (source.startswith("docs/") and not _is_historical_source(source)))


def _old_path_allowed(source: str, move: Move) -> bool:
    """Apply the old-path rule for the current stub lifecycle.

    While a required stub exists, non-canonical consumers such as C++, tools,
    and CI may migrate in a follow-up PR. Current docs and repository entry
    points must already use the new path. Once the map says stub=none, every
    non-historical source must use the new path.
    """
    if _is_historical_source(source):
        return True
    return move.stub == "required" and not _is_current_surface(source)


def run_checks(root: Path, files: list[Path] | None = None) -> list[Problem]:
    root = root.resolve()
    files = files if files is not None else _tracked_and_untracked(root)
    existing = {_posix(p.resolve(), root) for p in files if p.exists()}
    portal = root / "docs" / "README.md"
    moves, exceptions, problems = _read_policy(portal)
    move_by_old = {m.old: m for m in moves}
    move_by_old_basename = {PurePosixPath(m.old).name: m for m in moves}

    # The migration table is an executable contract, including absent stubs.
    for move in moves:
        current = root / PurePosixPath(move.current)
        old = root / PurePosixPath(move.old)
        if not current.is_file():
            problems.append(Problem("MOVE_TARGET", move.current, f"target for {move.old} does not exist"))
        if move.stub == "required":
            if not old.is_file():
                problems.append(Problem("STUB_MISSING", move.old, f"required redirect to {move.current} is absent"))
            else:
                actual, err = _stub_target(old, root, move)
                if err:
                    problems.append(Problem("STUB_FORMAT", move.old, err))
                elif actual != move.current:
                    problems.append(Problem("STUB_TARGET", move.old, f"maps to {actual}, expected {move.current}"))
        elif old.exists():
            problems.append(Problem("STUB_UNEXPECTED", move.old, "map says stub=none but the old path exists"))

    # A redirect-looking document is also part of the migration contract. It
    # must not exist outside the portal map, where nobody could review its
    # lifetime or target.
    for path in files:
        if path.suffix.lower() != ".md":
            continue
        text = _text(path)
        if text is None or not text.startswith("# (moved) "):
            continue
        rel = _posix(path.resolve(), root)
        if rel not in move_by_old:
            problems.append(Problem("STUB_UNMAPPED", rel, "redirect stub is absent from the move map"))

    # Markdown links and one-hop portal coverage.
    portal_targets: set[str] = {"docs/README.md"}
    for path in files:
        rel = _posix(path.resolve(), root)
        if path.suffix.lower() != ".md":
            continue
        text = _text(path)
        if text is None:
            problems.append(Problem("UTF8", rel, "Markdown is not UTF-8"))
            continue
        body = _without_markdown_code(text)
        for match in MD_LINK_RE.finditer(body):
            target, fragment = _resolve_link(path, match.group(1), root)
            if target is None:
                continue
            try:
                target_rel = _posix(target, root)
            except ValueError:
                problems.append(Problem("LINK_OUTSIDE", rel, match.group(1)))
                continue
            old_move = move_by_old.get(target_rel)
            policy_reference = (rel == "docs/README.md" and
                                _inside_portal_history(body, match.start()))
            old_allowed = old_move is not None and (
                policy_reference or _old_path_allowed(rel, old_move))
            if not target.exists():
                if not old_allowed:
                    problems.append(Problem("LINK_MISSING", rel, f"{match.group(1)} -> {target_rel}"))
            elif (fragment and target.suffix.lower() == ".md" and
                  target_rel.startswith("docs/")):
                target_text = _text(target)
                decoded = urllib.parse.unquote(fragment)
                if target_text is not None and decoded not in _heading_fragments(target_text):
                    problems.append(Problem("FRAGMENT_MISSING", rel, f"{match.group(1)} -> {target_rel}#{decoded}"))
            if rel == "docs/README.md" and target_rel.startswith("docs/") and target_rel.endswith(".md"):
                portal_targets.add(target_rel)
            if (old_move is not None and rel != target_rel and not old_allowed):
                problems.append(Problem("OLD_LINK", rel, f"link uses migrated path {target_rel}; use {old_move.current}"))

    for rel in sorted(existing):
        if not rel.startswith("docs/") or not rel.endswith(".md") or rel == "docs/README.md":
            continue
        if rel in move_by_old and move_by_old[rel].stub == "required":
            continue
        if rel not in portal_targets:
            problems.append(Problem("PORTAL_ORPHAN", rel, "not linked directly from docs/README.md"))

    # Repo-wide docs/path and numbered-section references.
    for path in files:
        rel = _posix(path.resolve(), root)
        suffix = path.suffix.lower()
        if suffix not in TEXT_SUFFIXES and path.name not in {"CMakeLists.txt", "README"}:
            continue
        text = _text(path)
        if text is None:
            continue
        body = _without_fences(text)
        for m in REPO_DOC_RE.finditer(body):
            ref = m.group(1)
            old_move = move_by_old.get(ref)
            policy_reference = (rel == "docs/README.md" and
                                _inside_portal_history(body, m.start()))
            if (old_move is not None and rel != ref and not policy_reference and
                    not _old_path_allowed(rel, old_move)):
                problems.append(Problem("OLD_PATH", rel, f"{ref} moved to {old_move.current}"))
            if (ref not in existing and ref not in exceptions and ref not in move_by_old
                    and not _is_historical_source(rel)):
                problems.append(Problem("DOC_PATH_MISSING", rel, ref))
        for m in SECTION_RE.finditer(body):
            ref, section = m.group("path"), m.group("section")
            old_move = move_by_old.get(ref)
            if old_move is not None and _old_path_allowed(rel, old_move):
                continue
            target_ref = (old_move or Move(ref, ref, "", "none")).current
            target = root / PurePosixPath(target_ref)
            target_text = _text(target)
            if target_text is not None and section not in _heading_sections(target_text):
                problems.append(Problem("SECTION_MISSING", rel, f"{ref} §{section} (current: {target_ref})"))

    # Only live board rows must follow current paths; completed rows are history.
    board = root / "docs" / "tasks.csv"
    board_text = _text(board)
    if board_text:
        try:
            rows = csv.reader(io.StringIO(board_text))
            next(rows, None)
            previous_end = rows.line_num
            for row in rows:
                # Report the physical start line. csv.reader.line_num is the
                # physical end line of the record just consumed, including an
                # embedded newline inside a quoted field.
                row_no = previous_end + 1
                previous_end = rows.line_num
                if len(row) < 4 or row[0].strip() == "対応済み":
                    continue
                live_text = "\n".join(row[1:])
                for ref in REPO_DOC_RE.findall(live_text):
                    if ref in move_by_old:
                        problems.append(Problem("BOARD_OLD_PATH", f"docs/tasks.csv:{row_no}", f"{ref} -> {move_by_old[ref].current}"))
                    elif ref not in existing and ref not in exceptions:
                        problems.append(Problem("BOARD_PATH_MISSING", f"docs/tasks.csv:{row_no}", ref))
                for basename in BARE_DOC_RE.findall(live_text):
                    move = move_by_old_basename.get(basename)
                    if move is not None:
                        problems.append(Problem("BOARD_OLD_PATH", f"docs/tasks.csv:{row_no}", f"{basename} -> {move.current}"))
        except csv.Error as exc:
            problems.append(Problem("BOARD_CSV", "docs/tasks.csv", str(exc)))

    return sorted(set(problems), key=lambda p: (p.code, p.path, p.detail))


def selftest() -> int:
    with tempfile.TemporaryDirectory(prefix="viewer-doc-check-") as tmp:
        root = Path(tmp)
        old_path = "docs/" + "old.md"
        new_path = "docs/features/" + "new.md"
        (root / "docs" / "features").mkdir(parents=True)
        (root / "docs" / "features" / "new.md").write_text(
            "# New\n\n## Real heading\n\n"
            "`[historical example](missing-inline.md)`\n",
            encoding="utf-8",
        )
        (root / "docs" / "background").mkdir(parents=True)
        (root / "docs" / "background" / "history.md").write_text(
            "# History\n\n`[old link](old-target.md)` and docs/" "missing-history.md\n",
            encoding="utf-8",
        )
        (root / "docs" / "tasks.csv").write_text(
            "分類,項目,内容,参照,実装ブランチ,備考\n", encoding="utf-8"
        )
        portal = f"""# Portal

{MOVE_START}
| 旧パス | 現在パス | 種別 | stub |
|---|---|---|---|
| [{old_path}](old.md) | [{new_path}](features/new.md) | whole-file | required |
{MOVE_END}

{EXC_START}
| パス | 理由 |
|---|---|
{EXC_END}

[new](features/new.md#missing-heading)
[history](background/history.md)
"""
        (root / "docs" / "README.md").write_text(portal, encoding="utf-8")
        (root / "docs" / "unmapped.md").write_text(
            "# (moved) unmapped.md → features/new.md\n"
            "移動日: 2026-08-18\n"
            "本文はここにありません。正典は [features/new.md](features/new.md) です。\n",
            encoding="utf-8",
        )

        def scan() -> list[Problem]:
            return run_checks(root, [p for p in root.rglob("*") if p.is_file()])

        problems = scan()
        if not any(p.code == "STUB_MISSING" for p in problems):
            print("selftest: missing required stub was not rejected", file=sys.stderr)
            return 1
        if not any(p.code == "FRAGMENT_MISSING" for p in problems):
            print("selftest: missing heading fragment was not rejected", file=sys.stderr)
            return 1
        if not any(p.code == "STUB_UNMAPPED" for p in problems):
            print("selftest: unmapped redirect stub was not rejected", file=sys.stderr)
            return 1

        (root / "docs" / "unmapped.md").unlink()
        portal = portal.replace("#missing-heading", "#real-heading")
        (root / "docs" / "README.md").write_text(portal, encoding="utf-8")
        valid_stub = (
            "# (moved) old.md → features/new.md\n"
            "移動日: 2026-08-18\n"
            "本文はここにありません。正典は [features/new.md](features/new.md) です。\n"
        )
        malformed_stubs = [
            valid_stub.replace("old.md →", "wrong.md →", 1),
            valid_stub.replace("2026-08-18", "garbage", 1),
            valid_stub.replace("本文はここにありません。正典は", "任意の本文。リンクは", 1),
            valid_stub + "\n",
        ]
        for malformed in malformed_stubs:
            (root / "docs" / "old.md").write_text(malformed, encoding="utf-8")
            problems = scan()
            if not any(p.code == "STUB_FORMAT" and p.path == old_path for p in problems):
                print("selftest: malformed three-line stub was accepted", file=sys.stderr)
                return 1

        (root / "docs" / "old.md").write_text(valid_stub, encoding="utf-8")
        problems = scan()
        if any(p.code.startswith("STUB_") or p.code in {
                "FRAGMENT_MISSING", "LINK_MISSING", "DOC_PATH_MISSING"}
               for p in problems):
            print("selftest: valid stub or heading fragment was rejected", file=sys.stderr)
            for problem in problems:
                print(problem, file=sys.stderr)
            return 1

        # Lifecycle case 1: a required stub keeps non-canonical source
        # references valid until the separate reference-migration PR lands.
        (root / "core").mkdir()
        (root / "core" / "reader.inc").write_text(
            "// " + old_path + " §1\n", encoding="utf-8"
        )
        problems = scan()
        if any(p.code == "OLD_PATH" and p.path == "core/reader.inc" for p in problems):
            print("selftest case 1: required stub rejected transitional source", file=sys.stderr)
            return 1

        # Lifecycle case 2: current docs, root entry points, and live board rows
        # must switch immediately even while the stub remains required.
        current_source = "docs/features/" + "current.md"
        (root / "docs" / "features" / "current.md").write_text(
            f"# Current\n\n{old_path}\n", encoding="utf-8"
        )
        (root / "README.md").write_text(old_path + "\n", encoding="utf-8")
        (root / "ARCHITECTURE.md").write_text(old_path + "\n", encoding="utf-8")
        (root / "docs" / "tasks.csv").write_text(
            "分類,項目,内容,参照,実装ブランチ,備考\n"
            f'進行中,"reference\ncontinued",case,{old_path},,\n'
            f"進行中,{PurePosixPath(old_path).name},case,{new_path},,\n"
            f"進行中,content,{old_path},{new_path},,\n"
            f"進行中,branch,case,{new_path},{PurePosixPath(old_path).name},\n"
            f"進行中,note,case,{new_path},,{PurePosixPath(old_path).name}\n",
            encoding="utf-8",
        )
        bad_portal_link = "\n[bad old portal link](old.md)\n"
        (root / "docs" / "README.md").write_text(
            portal + bad_portal_link, encoding="utf-8"
        )
        problems = scan()
        rejected = {p.path for p in problems if p.code in {"OLD_PATH", "BOARD_OLD_PATH"}}
        required_rejections = {
            current_source, "README.md", "ARCHITECTURE.md",
            "docs/tasks.csv:2", "docs/tasks.csv:4", "docs/tasks.csv:5",
            "docs/tasks.csv:6", "docs/tasks.csv:7",
        }
        if not required_rejections.issubset(rejected):
            print("selftest case 2: current source escaped old-path gate", file=sys.stderr)
            print(" missing", sorted(required_rejections - rejected), file=sys.stderr)
            return 1
        if not any(p.code == "OLD_LINK" and p.path == "docs/README.md" for p in problems):
            print("selftest case 2: portal link outside move map escaped old-link gate", file=sys.stderr)
            return 1

        # Lifecycle case 3: after stub=none, transitional source is no longer
        # exempt and must use the current path as well.
        (root / "docs" / "features" / "current.md").unlink()
        (root / "README.md").unlink()
        (root / "ARCHITECTURE.md").unlink()
        (root / "docs" / "tasks.csv").write_text(
            "分類,項目,内容,参照,実装ブランチ,備考\n", encoding="utf-8"
        )
        portal = portal.replace("| whole-file | required |", "| whole-file | none |")
        (root / "docs" / "README.md").write_text(
            portal + bad_portal_link, encoding="utf-8"
        )
        (root / "docs" / "old.md").unlink()
        problems = scan()
        if not any(p.code == "OLD_PATH" and p.path == "core/reader.inc" for p in problems):
            print("selftest case 3: stub=none allowed transitional source", file=sys.stderr)
            return 1
        if not any(p.code == "OLD_LINK" and p.path == "docs/README.md" for p in problems):
            print("selftest case 3: stub=none allowed portal link outside move map", file=sys.stderr)
            return 1

        # Lifecycle case 4: background, immutable results, and completed board
        # rows remain historical evidence after the stub is removed.
        (root / "docs" / "README.md").write_text(portal, encoding="utf-8")
        (root / "core" / "reader.inc").unlink()
        background_source = "docs/background/" + "old-ref.md"
        result_source = "docs/verification/results/" + "old-ref.md"
        (root / "docs" / "background" / "old-ref.md").write_text(
            f"# Background\n\n{old_path}\n", encoding="utf-8"
        )
        (root / "docs" / "verification" / "results").mkdir(parents=True)
        (root / "docs" / "verification" / "results" / "old-ref.md").write_text(
            f"# Result\n\n{old_path}\n", encoding="utf-8"
        )
        (root / "docs" / "tasks.csv").write_text(
            "分類,項目,内容,参照,実装ブランチ,備考\n"
            f"対応済み,{PurePosixPath(old_path).name},{old_path},{old_path},"
            f"{PurePosixPath(old_path).name},{PurePosixPath(old_path).name}\n",
            encoding="utf-8",
        )
        problems = scan()
        historical_paths = {
            background_source,
            result_source,
            "docs/tasks.csv",
            "docs/tasks.csv:2",
        }
        if any(p.path in historical_paths and
               p.code in {"OLD_PATH", "OLD_LINK", "LINK_MISSING", "BOARD_OLD_PATH"}
               for p in problems):
            print("selftest case 4: historical source was rejected", file=sys.stderr)
            for problem in problems:
                if problem.path in historical_paths:
                    print(problem, file=sys.stderr)
            return 1

    print("doc-check selftest: lifecycle cases 1-4 and stub/fragment gates green")
    return 0

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--selftest", action="store_true", help="calibrate the required-stub gate")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    problems = run_checks(args.root)
    for problem in problems:
        print(problem)
    if problems:
        print(f"doc-check: {len(problems)} problem(s)", file=sys.stderr)
        return 1
    print("doc-check: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
