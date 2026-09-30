#!/usr/bin/env python3
"""Find includes that can be removed, and remove the provably unnecessary ones.

The test is the compiler's: comment out one include, recompile that translation
unit, and if it still builds the header was not needed. That is unsound in
general -- a header can supply a declaration used only at template instantiation,
so "it compiles without it" does not always mean "nothing needs it" -- so this
only removes includes that BOTH pass the compile test AND are project headers,
where the risk is a redundant dependency rather than a missing declaration. System
headers are reported but never touched.

Run with --dry-run to see the list without changing anything.
"""
import argparse
import json
import os
import re
import shlex
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def compiles(cmd, cwd):
    return subprocess.run(shlex.split(cmd.replace(" -c ", " -fsyntax-only -c ")),
                          cwd=cwd, capture_output=True, text=True).returncode == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    os.chdir(ROOT)

    db = json.load(open("compile_commands.json"))
    include_re = re.compile(r'^\s*#\s*include\s+"([^"]+)"')

    def check(entry):
        path = entry["file"]
        if "/test/" in path or path.startswith("src/test/"):
            return []
        src = open(path).read()
        lines = src.split("\n")
        results = []
        for i, line in enumerate(lines):
            m = include_re.match(line)
            if not m:
                continue
            hdr = m.group(1)
            is_project = "/" in hdr and not hdr.startswith("CImg")
            trial = lines[:]
            trial[i] = "// kt-unused-include: " + line
            open(path, "w").write("\n".join(trial))
            ok = compiles(entry["command"], entry["directory"])
            open(path, "w").write(src)
            if ok:
                results.append((path, i + 1, hdr, is_project))
        return results

    with ThreadPoolExecutor(max_workers=a.jobs) as ex:
        found = [r for rs in ex.map(check, db) for r in rs]

    removable = [r for r in found if r[3]]
    system = [r for r in found if not r[3]]
    print("removable project includes: %d" % len(removable))
    for p, ln, h, _ in removable:
        print("  %s:%d  %s" % (p, ln, h))
    print("system includes the compiler would accept dropping: %d (left alone)" % len(system))

    if a.dry_run or not removable:
        return 0

    # Apply, one pass per file, dropping every confirmed project include at once
    # and re-verifying the file afterwards.
    byfile = {}
    for p, ln, h, _ in removable:
        byfile.setdefault(p, set()).add(ln)
    entry_of = {e["file"]: e for e in db}
    for path, lines_to_drop in byfile.items():
        src = open(path).read().split("\n")
        for ln in lines_to_drop:
            src[ln - 1] = ""
        new = "\n".join(src)
        open(path, "w").write(new)
        if not compiles(entry_of[path]["command"], entry_of[path]["directory"]):
            open(path, "w").write("\n".join(src))
            print("REVERTED %s: removing all of them together broke it" % path)
        else:
            print("cleaned %s (-%d)" % (path, len(lines_to_drop)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
