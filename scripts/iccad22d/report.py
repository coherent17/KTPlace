#!/usr/bin/env python3
"""Tabulate ICCAD 2022 Problem D results from output/.

Two flows per case:
  ntuplace3  the contest's score: output/<case>_ntuplace3.txt (macros as given) and
             output/<case>_<variant>_ntuplace3.txt (our .dmp), each ending in "HPWL <value>"
  ktplace    ktplace's own full flow: the last committed HPWL in output/<case>/ktplace.log
             (macros as given) and output/<case>_<variant>/ktplace.log

Usage: report.py [variant ...]      (default: every variant found)
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2] / "output"
CASES = [f"case{i:02d}" for i in range(1, 16)]
BETA = {
    "case01": [213794890717, 215749108445, 217954996726, 218533752016, 218917300285],
    "case02": [276481870207, 278817928995, 279725532869, 280024269511, 281158147865],
    "case03": [318641210756, 329541798158, 331600215555, 339682902343, 341107307192],
    "case04": [361464041923, 398715277574, 400480770435, 403093400725, 405123034293],
    "case05": [294306245762, 294909063710, 295063117701, 295492942783, 298622678447],
}


def ntuplace3(path):
    try:
        text = path.read_text()
    except OSError:
        return None
    found = re.findall(r"^HPWL (\d+)", text, re.M)
    return int(found[-1]) if found else None


def ktplace(path):
    try:
        text = path.read_text()
    except OSError:
        return None
    found = re.findall(r"committed to ktDM: HPWL ([0-9.e+]+)", text)
    if not found or "Run finished" not in text:
        return None
    return float(found[-1])


def variants():
    names = set()
    for path in ROOT.glob("case??_*_ntuplace3.txt"):
        names.add(path.name[len("case01_"):-len("_ntuplace3.txt")])
    return sorted(names)


def change(value, base):
    if value is None or base is None:
        return ""
    return f"{100.0 * (value / base - 1.0):+.2f}%"


def rank(value, case):
    if value is None or case not in BETA:
        return ""
    better = sum(1 for other in BETA[case] if other < value)
    return f"#{better + 1}"


def table(headers, rows):
    widths = [max(len(str(r[i])) for r in [headers, *rows]) for i in range(len(headers))]
    line = lambda r: "| " + " | ".join(str(c).rjust(w) for c, w in zip(r, widths)) + " |"
    out = [line(headers), "|" + "|".join("-" * (w + 2) for w in widths) + "|"]
    out += [line(r) for r in rows]
    return "\n".join(out)


def main():
    chosen = sys.argv[1:] or variants()
    rows = []
    for case in CASES:
        base = ntuplace3(ROOT / f"{case}_ntuplace3.txt")
        cells = [case, f"{base / 1e9:.3f}" if base else ""]
        for v in chosen:
            value = ntuplace3(ROOT / f"{case}_{v}_ntuplace3.txt")
            cells += [f"{value / 1e9:.3f}" if value else "", change(value, base), rank(value, case)]
        beta = BETA.get(case)
        cells += [f"{beta[0] / 1e9:.3f}" if beta else "", change(beta[0], base) if beta else ""]
        if any(cells[2:-2]) or base:
            rows.append(cells)
    headers = ["case", "baseline"]
    for v in chosen:
        headers += [v, "vs base", "beta rank"]
    headers += ["beta #1", "#1 vs base"]
    print("ntuplace3 flow (contest score, x 1e9)")
    print(table(headers, rows))

    rows = []
    for case in CASES:
        base = ntuplace3(ROOT / f"{case}_ntuplace3.txt")
        scored = [(ntuplace3(ROOT / f"{case}_{v}_ntuplace3.txt"), v) for v in chosen]
        scored = [(value, v) for value, v in scored if value]
        if not scored:
            continue
        value, v = min(scored)
        beta = BETA.get(case)
        rows.append([case, f"{base / 1e9:.3f}" if base else "", v, f"{value / 1e9:.3f}",
                     change(value, base), rank(value, case),
                     f"{beta[0] / 1e9:.3f}" if beta else "",
                     change(value, beta[0]) if beta else ""])
    print()
    print("best macro placement per case (ntuplace3, x 1e9)")
    print(table(["case", "baseline", "variant", "HPWL", "vs base", "beta rank", "beta #1",
                 "vs #1"], rows))

    rows = []
    for case in CASES:
        base = ktplace(ROOT / case / "ktplace.log")
        cells = [case, f"{base / 1e9:.3f}" if base else ""]
        for v in chosen:
            value = ktplace(ROOT / f"{case}_{v}" / "ktplace.log")
            cells += [f"{value / 1e9:.3f}" if value else "", change(value, base)]
        if any(cells[1:]):
            rows.append(cells)
    headers = ["case", "no move"]
    for v in chosen:
        headers += [v, "vs no move"]
    print()
    print("ktplace full flow (x 1e9)")
    print(table(headers, rows))


if __name__ == "__main__":
    main()
