#!/usr/bin/env python3
"""Baseline sweep through the KTPlace web console.

Runs every design the console knows about, once per algorithm, and writes a
table the repository can carry as a reference. The console runs one job at a
time and records each result in its own history, so this only has to start a
run, wait for it to stop, and read the headline numbers back.

Re-running is safe: a pair already in the output table is skipped, so an
interrupted sweep resumes where it left off. A run that outlives --timeout is
stopped (the console has no stop-without-delete), and recorded as `timeout`.

    python3 scripts/bench_baseline.py                      # http://127.0.0.1:8080
    python3 scripts/bench_baseline.py --algorithms simpl
    python3 scripts/bench_baseline.py --designs ibm0,adaptec
    python3 scripts/bench_baseline.py --dry-run

Standard library only, like the rest of the tooling under webui/.
"""
import argparse
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

COLUMNS = ("design", "algorithm", "status", "hpwl", "verdict", "overlaps",
           "unplaced", "offRow", "elapsed_s", "run_id")
POLL_S = 5  # matches the console page's own tick


def api(base, path, method="GET", body=None, timeout=60):
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Content-Type": "application/json"} if data else {}
    req = urllib.request.Request(base + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read().decode()
    return json.loads(raw) if raw.strip() else {}


def read_done(path):
    """(design, algorithm) -> row, for the pairs a previous sweep recorded."""
    done = {}
    if not os.path.exists(path):
        return done
    with open(path) as fh:
        for line in fh:
            if line.startswith("#") or line.startswith(COLUMNS[0] + "\t"):
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= len(COLUMNS):
                done[(parts[0], parts[1])] = dict(zip(COLUMNS, parts))
    return done


def write_table(path, rows, meta):
    tmp = path + ".tmp"
    with open(tmp, "w") as fh:
        for k, v in meta.items():
            fh.write("# %s: %s\n" % (k, v))
        fh.write("\t".join(COLUMNS) + "\n")
        for key in sorted(rows):
            fh.write("\t".join(str(rows[key].get(c, "")) for c in COLUMNS) + "\n")
    os.replace(tmp, path)


def git_rev():
    try:
        rev = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=os.path.dirname(__file__),
                             capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "status", "--porcelain"], cwd=os.path.dirname(__file__),
                               capture_output=True, text=True).stdout.strip()
        return rev + ("-dirty" if dirty else "")
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def run_one(base, design, algo, timeout_s, log):
    # The console answers 409 while another run holds its single slot; wait it out
    # rather than treating it as a failure.
    while True:
        try:
            run = api(base, "/api/runs", "POST", {"benchmark": design, "algorithm": algo})
            break
        except urllib.error.HTTPError as e:
            if e.code == 409:
                time.sleep(POLL_S)
                continue
            if e.code == 503:
                sys.exit("engine is not built: " + e.read().decode().strip())
            raise
    rid = run["id"]
    deadline = time.time() + timeout_s
    view = run
    while view.get("status") == "running":
        time.sleep(POLL_S)
        if time.time() > deadline:
            api(base, "/api/runs/" + rid, "DELETE")
            log("    stopped: exceeded %ds" % timeout_s)
            return {"design": design, "algorithm": algo, "status": "timeout",
                    "run_id": rid}
        try:
            view = api(base, "/api/runs/" + rid)
        except urllib.error.HTTPError as e:
            if e.code == 404:  # deleted or gone; nothing more to wait for
                return {"design": design, "algorithm": algo, "status": "missing",
                        "run_id": rid}
    s = view.get("summary") or {}
    return {"design": design, "algorithm": algo, "status": view.get("status", "?"),
            "hpwl": s.get("hpwl", ""), "verdict": s.get("verdict", ""),
            "overlaps": s.get("overlaps", ""), "unplaced": s.get("unplaced", ""),
            "offRow": s.get("offRow", ""), "elapsed_s": view.get("elapsed", ""),
            "run_id": rid}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", default="http://127.0.0.1:8080")
    ap.add_argument("--out", default="benchmark/baseline.tsv")
    ap.add_argument("--algorithms", default="simpl,ntuplace1")
    ap.add_argument("--designs", default="", help="comma-separated substrings to keep")
    ap.add_argument("--timeout", type=int, default=1500, help="seconds per run")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    base = args.base.rstrip("/")
    algos = [a.strip() for a in args.algorithms.split(",") if a.strip()]

    designs = api(base, "/api/benchmarks")["designs"]
    if args.designs:
        subs = [s.strip() for s in args.designs.split(",") if s.strip()]
        designs = [d for d in designs if any(s in d for s in subs)]

    rows = read_done(args.out)
    todo = [(d, a) for d in designs for a in algos if (d, a) not in rows]
    print("%d designs x %d algorithms: %d runs to do, %d already recorded"
          % (len(designs), len(algos), len(todo), len(rows)), flush=True)
    if args.dry_run:
        for d, a in todo:
            print("  would run", d, a)
        return

    meta = {"ktplace baseline": "one row per design and algorithm",
            "generated": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "git": git_rev(),
            "console": base,
            "algorithms": ",".join(algos),
            "settings": "engine defaults (animation on), console default environment"}

    def log(msg):
        print(msg, flush=True)

    for i, (design, algo) in enumerate(todo, 1):
        log("[%d/%d] %s %s ..." % (i, len(todo), design, algo))
        row = run_one(base, design, algo, args.timeout, log)
        rows[(design, algo)] = row
        write_table(args.out, rows, meta)
        log("    -> %s hpwl=%s %ss" % (row["status"], row.get("hpwl", ""), row.get("elapsed_s", "")))
    log("wrote %s: %d rows" % (args.out, len(rows)))


if __name__ == "__main__":
    main()
