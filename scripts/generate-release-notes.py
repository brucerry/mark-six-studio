#!/usr/bin/env python3
"""Render version-specific release notes with a short linked commit list."""
import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parent.parent
REPOSITORY = "https://github.com/brucerry/mark-six-studio"
TAG = re.compile(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\Z")
HEADINGS = ["## 📦 Version description", "## ✨ Changes", "## 🧾 Commits"]
MARKER = "<!-- RELEASE_COMMITS -->"


def git(root, *arguments):
    return subprocess.check_output(["git", "-C", str(root), *arguments], text=True).strip()


def render(tag, root=ROOT):
    match = TAG.fullmatch(tag)
    if not match:
        raise ValueError("Expected a stable vMAJOR.MINOR.PATCH tag")
    path = root / "installer" / "releases" / (tag + ".md")
    if not path.is_file():
        raise ValueError("Create version-specific notes at " + str(path))
    text = path.read_text(encoding="utf-8")
    if [line for line in text.splitlines() if line.startswith("## ")] != HEADINGS:
        raise ValueError("Release notes must have version description, changes and commits sections")
    if MARKER not in text:
        return text  # Retained historical notes already have an explicit commit list.
    if text.count(MARKER) != 1:
        raise ValueError("Release commit marker must appear exactly once")
    version = tuple(map(int, match.groups()))
    previous = []
    for candidate in git(root, "tag", "--merged", tag).splitlines():
        parsed = TAG.fullmatch(candidate)
        if parsed and tuple(map(int, parsed.groups())) < version:
            previous.append((tuple(map(int, parsed.groups())), candidate))
    prior = max(previous)[1] if previous else None
    commit_range = prior + ".." + tag if prior else tag
    entries = git(root, "log", "--no-merges", "--max-count=10", "--format=%H%x09%s", commit_range).splitlines()
    bullets = []
    for entry in entries:
        commit, subject = entry.split("\t", 1)
        subject = re.sub(r"([\\`*_\[\]<>])", r"\\\1", subject[:160])
        bullets.append("- [" + commit[:7] + "](" + REPOSITORY + "/commit/" + commit + ") — " + subject)
    history = "/compare/" + prior + "..." + tag if prior else "/commits/" + tag + "/"
    bullets.append("- [Full changelog](" + REPOSITORY + history + ").")
    return text.replace(MARKER, "\n".join(bullets))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        notes = render(args.tag)
    except (ValueError, subprocess.CalledProcessError) as error:
        parser.error(str(error))
    args.output.write_text(notes, encoding="utf-8")


if __name__ == "__main__":
    main()
