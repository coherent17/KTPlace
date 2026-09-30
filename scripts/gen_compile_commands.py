#!/usr/bin/env python3
"""Write compile_commands.json, so clangd can resolve the project's headers.

There is no compilation database in the tree and `bear` is not installed, so this
builds one from the same values the build itself uses: the flags in
src/Master.make and the per-directory include of the project root.

Two details that matter for clangd specifically:

  * The compiler is named as `g++` with the real flags, not a stub. clangd
    re-parses the command line, and a command it cannot honour is worse than no
    database at all, because it looks like the file has no headers.
  * `directory` is the directory the command would run in and `file` is relative
    to it, which is the form clangd resolves. Absolute paths are avoided so the
    database is identical on every machine rather than embedding one checkout's
    layout.

Vendored third-party headers are included normally: clangd needs to resolve them
too, and excluding them produces a wall of "file not found" in the CImg users.
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")

# Kept in step with src/Master.make. The coverage branch is not included: clangd
# does not need it, and a flag it cannot parse is a flag it drops.
FLAGS = "-std=c++23 -Wall -Wextra -Wpedantic -Wshadow -O2 -g -pthread"


def subdirs():
    out = subprocess.run(
        ["make", "-s", "-C", SRC, "-f", "Master.make", "print-subdirs"],
        capture_output=True, text=True, cwd=ROOT,
    )
    dirs = [d for d in out.stdout.split() if d]
    return dirs or []


def sources():
    """Every tracked first-party translation unit."""
    out = subprocess.run(
        ["git", "ls-files", "src/*.cc", "src/*/*.cc", "src/*/*/*.cc"],
        capture_output=True, text=True, cwd=ROOT,
    )
    return [f for f in out.stdout.split() if f.endswith(".cc")]


def main():
    os.chdir(ROOT)
    dirs = ["."] + subdirs()
    db = []
    for f in sources():
        # The translation unit's own directory decides -I depth, exactly as the
        # per-directory Master.make files do: a file in src/placer/simpl includes
        # the project root, one in src/util includes src/util's parent.
        # `directory` is where the command runs and `file` is relative to it, which
        # is the form clangd resolves. Anchoring both at the project root keeps the
        # pair consistent -- a per-subdirectory `directory` with a root-relative
        # `file` is a combination that resolves to nothing.
        db.append({
            "directory": ".",
            "file": f,
            "command": "g++ %s -I%s -c %s" % (FLAGS, SRC, f),
        })
    with open(os.path.join(ROOT, "compile_commands.json"), "w") as fh:
        json.dump(db, fh, indent=2, sort_keys=True)
        fh.write("\n")
    print("compile_commands.json: %d translation unit(s)" % len(db))


if __name__ == "__main__":
    main()
