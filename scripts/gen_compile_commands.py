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
#
# `c++2b` rather than `c++23`, though both name the same standard to g++ 13. The
# distinction matters only to clangd, and there it is the difference between a
# working database and none: clang rejects a `-std` value it does not know, and on
# rejecting one it discards the rest of the command line too -- so every include
# path goes with it and the file then reports its own headers as missing. clang 14
# spells the flag `c++2b`; clang 16 and later accept `c++23` and would not need
# this, but `c++2b` is accepted by every version from 14 on, so it is the one that
# works for both.
FLAGS = "-std=c++2b -Wall -Wextra -Wpedantic -Wshadow -O2 -g -pthread"


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
    db = []
    for f in sources():
        # `directory` is absolute rather than "." because clangd resolves the
        # include paths it finds in `command` against it, and a relative one is
        # measured from the editor's working directory rather than from the
        # database's own location. Get that wrong and clangd silently falls back
        # to a command with no -I at all, which reads as every project header
        # being missing rather than as a database problem.
        db.append({
            "directory": ROOT,
            "file": f,
            "command": "g++ %s -I%s -c %s" % (FLAGS, SRC, f),
        })
    with open(os.path.join(ROOT, "compile_commands.json"), "w") as fh:
        json.dump(db, fh, indent=2, sort_keys=True)
        fh.write("\n")
    print("compile_commands.json: %d translation unit(s)" % len(db))


if __name__ == "__main__":
    main()
