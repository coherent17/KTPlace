#!/usr/bin/env python3
"""Fetch the KTPlace benchmark suites from their original sources.

Every archive is downloaded from the site that originally published the suite
(the University of Michigan VLSI CAD lab "GSRC Bookshelf" archive, or the ISPD
contest site) -- no third-party mirror or personal repository is involved.
Each suite is unpacked into the layout the ktplace adapters expect: one
directory per design. The ISPD 2015 suite stays in its contest LEF/DEF form,
while the Bookshelf archives are normalized so every design directory holds
plain, uncompressed files named after the design:

    benchmark/ISPD_2015/mgc_des_perf_a/{floorplan.def,cells.lef,tech.lef,...}
    benchmark/ICCAD04/ibm01/{ibm01.nodes,ibm01.nets,ibm01.pl,ibm01.scl,...}
    benchmark/ICCAD04/dma/{dma.nodes,dma.nets,dma.pl,dma.scl,...}
    benchmark/ISPD02/ibm01/{ibm01.nodes,ibm01.nets,ibm01.pl,ibm01.scl,...}

Only the standard library is used; archives are unpacked with `tarfile` and any
`.gz` members are inflated so the readers see plain text. Design directories
that already contain a `.nodes` file are left alone, so re-running is cheap.

Usage:
    python3 benchmark/fetch_benchmarks.py            # everything available
    python3 benchmark/fetch_benchmarks.py --list     # show sources and status
    python3 benchmark/fetch_benchmarks.py --suite ICCAD04 --design ibm01
    python3 benchmark/fetch_benchmarks.py --dest /tmp/ktplace_bm
"""

import argparse
import gzip
import pathlib
import shutil
import sys
import tarfile
import tempfile
import urllib.request

# Repository root is the parent of the benchmark/ directory holding this file.
ROOT = pathlib.Path(__file__).resolve().parent.parent / "benchmark"
CACHE = ROOT / ".cache"

# File extensions the Bookshelf reader consumes.
BOOKSHELF_EXTS = (".nodes", ".nets", ".pl", ".scl", ".wts", ".aux")

UMICH = "https://vlsicad.eecs.umich.edu/BK"

# --- ISPD 2015 placement contest (LEF/DEF), published by the contest site -----
ISPD15_URL = "https://www.ispd.cc/contests/15/web/benchmarks/ispd_2015_contest_benchmark.tgz"
ISPD15_DIR = "ISPD_2015"
ISPD15_DESIGNS = [
    "mgc_des_perf_1", "mgc_des_perf_a", "mgc_des_perf_b",
    "mgc_edit_dist_a",
    "mgc_fft_1", "mgc_fft_2", "mgc_fft_a", "mgc_fft_b",
    "mgc_matrix_mult_1", "mgc_matrix_mult_a", "mgc_matrix_mult_b",
    "mgc_pci_bridge32_a", "mgc_pci_bridge32_b",
    "mgc_superblue11_a", "mgc_superblue12", "mgc_superblue16_a",
]

# --- Bookshelf suites published by the UMich VLSI CAD lab ----------------------
# layout "flat"   : <design>/<name>.{nodes,nets,...}                (IBM archives)
# layout "faraday": <DESIGN>/BOOKSHELF/<name>_BS.{nodes,nets,...}   (Faraday archive)
BOOKSHELF = {
    "ICCAD04": [
        {
            "label": "IBM-MSwPins (ibm01-ibm18)",
            "url": f"{UMICH}/ICCAD04bench/ibmMSWpinsICCAD04Bench_BOOKSHELF.tar.gz",
            "layout": "flat",
            "designs": [f"ibm{i:02d}" for i in range(1, 19)],
        },
        {
            "label": "Faraday (dma, dsp1, dsp2, risc1, risc2)",
            "url": f"{UMICH}/ICCAD04bench/FARADAY_ICCAD04Bench.tar.gz",
            "layout": "faraday",
            "designs": ["dma", "dsp1", "dsp2", "risc1", "risc2"],
        },
    ],
    "ISPD02": [
        {
            "label": "IBM-MS (ibm01-ibm18)",
            "url": f"{UMICH}/ISPD02bench/ibmISPD02Bench_Bookshelf.tar.gz",
            "layout": "flat",
            "designs": [f"ibm{i:02d}" for i in range(1, 19)],
        },
    ],
}

USER_AGENT = "KTPlace-benchmark-fetcher/1.0"
CHUNK = 1 << 20


def download(url: str, dst: pathlib.Path) -> None:
    """Stream @p url into @p dst, reporting progress for large archives."""
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=120) as response, open(dst, "wb") as out:
        total = int(response.headers.get("Content-Length") or 0)
        done = 0
        next_report = 10
        while True:
            chunk = response.read(CHUNK)
            if not chunk:
                break
            out.write(chunk)
            done += len(chunk)
            if total:
                pct = 100 * done // total
                if pct >= next_report:
                    print(f"    {pct:3d}%  ({done >> 20} / {total >> 20} MiB)", flush=True)
                    next_report += 10


def safe_extract(archive: pathlib.Path, dest: pathlib.Path) -> None:
    with tarfile.open(archive, "r:*") as tar:
        try:
            tar.extractall(dest, filter="data")
        except TypeError:  # Python < 3.12 has no extraction filters
            tar.extractall(dest)


def inflate(path: pathlib.Path) -> pathlib.Path:
    """Return @p path with any .gz suffix removed, inflating when needed."""
    if path.suffix != ".gz":
        return path
    plain = path.with_suffix("")
    if plain.exists():
        path.unlink()
        return plain
    with gzip.open(path, "rb") as src, open(plain, "wb") as dst:
        shutil.copyfileobj(src, dst)
    path.unlink()
    return plain


def bookshelf_files(design: pathlib.Path) -> list[pathlib.Path]:
    """The Bookshelf input files inside @p design, with .gz inflated."""
    found = []
    for entry in sorted(design.iterdir()):
        if not entry.is_file():
            continue
        name = entry.name
        if name.endswith(".gz") and not name[:-3].endswith(BOOKSHELF_EXTS):
            continue
        if not name.endswith(BOOKSHELF_EXTS):
            continue
        found.append(inflate(entry))
    return found


def design_is_installed(target: pathlib.Path) -> bool:
    return target.is_dir() and any(target.glob("*.nodes"))


def install_flat(staging: pathlib.Path, dest: pathlib.Path, spec: dict,
                 only: str | None, force: bool) -> int:
    """Copy <design>/<name>.{ext} trees straight across."""
    count = 0
    for source in sorted(p for p in staging.iterdir() if p.is_dir()):
        design = source.name
        if only and design != only:
            continue
        target = dest / design
        if design_is_installed(target) and not force:
            continue
        shutil.rmtree(target, ignore_errors=True)
        target.mkdir(parents=True)
        for book in bookshelf_files(source):
            shutil.copy2(book, target / book.name)
        if design_is_installed(target):
            count += 1
    return count


def install_faraday(staging: pathlib.Path, dest: pathlib.Path, spec: dict,
                    only: str | None, force: bool) -> int:
    """Flatten <DESIGN>/BOOKSHELF/<name>_BS.{ext} into <design>/<name>.{ext}."""
    count = 0
    for source in sorted(p for p in staging.iterdir() if p.is_dir()):
        design = source.name.lower()
        if only and design != only:
            continue
        book_dir = source / "BOOKSHELF"
        if not book_dir.is_dir():
            continue
        target = dest / design
        if design_is_installed(target) and not force:
            continue
        shutil.rmtree(target, ignore_errors=True)
        target.mkdir(parents=True)
        for entry in sorted(book_dir.iterdir()):
            if not entry.is_file():
                continue
            path = inflate(entry)
            if not path.name.endswith(BOOKSHELF_EXTS):
                continue
            stem = path.stem
            if stem.upper().endswith("_BS"):
                stem = stem[:-3]
            shutil.copy2(path, target / f"{stem.lower()}{path.suffix}")
        if design_is_installed(target):
            count += 1
    return count


LAYOUTS = {"flat": install_flat, "faraday": install_faraday}


def fetch_bookshelf(suite: str, dest_root: pathlib.Path, only: str | None,
                    force: bool) -> None:
    dest = dest_root / suite
    dest.mkdir(parents=True, exist_ok=True)
    for spec in BOOKSHELF[suite]:
        wanted = [d for d in spec["designs"] if not only or d == only]
        if not force and all(design_is_installed(dest / d) for d in wanted):
            print(f"{suite}: {spec['label']} already present, skipping.")
            continue
        CACHE.mkdir(parents=True, exist_ok=True)
        archive = CACHE / spec["url"].rsplit("/", 1)[-1]
        print(f"Downloading {suite} / {spec['label']} ...\n  {spec['url']}")
        download(spec["url"], archive)
        with tempfile.TemporaryDirectory(prefix=suite.lower() + "_") as tmp:
            staging = pathlib.Path(tmp)
            print("  unpacking ...")
            safe_extract(archive, staging)
            count = LAYOUTS[spec["layout"]](staging, dest, spec, only, force)
            print(f"  installed {count} design(s) into {suite}/")
        archive.unlink(missing_ok=True)


def fetch_ispd15(dest_root: pathlib.Path, force: bool, only: str | None = None) -> None:
    """Fetch the ISPD 2015 suite, or just one design from it.

    The suite ships as a single contest tarball, so the download covers every
    design either way; @p only limits what gets unpacked into the tree, which
    matters because the full suite extracts to roughly 900 MiB.
    """
    dest = dest_root / ISPD15_DIR
    wanted = [only] if only else ISPD15_DESIGNS
    if only and only not in ISPD15_DESIGNS:
        print(f"{ISPD15_DIR}: unknown design '{only}'; known designs: "
              f"{', '.join(ISPD15_DESIGNS)}")
        return
    if not force and all((dest / d).is_dir() for d in wanted):
        scope = only if only else f"all {len(ISPD15_DESIGNS)} designs"
        print(f"{ISPD15_DIR}: {scope} present, nothing to do.")
        return
    dest.mkdir(parents=True, exist_ok=True)
    CACHE.mkdir(parents=True, exist_ok=True)
    archive = CACHE / "ispd_2015_contest_benchmark.tgz"
    print(f"Downloading {ISPD15_DIR} / ISPD 2015 placement contest (LEF/DEF) ...\n  {ISPD15_URL}")
    download(ISPD15_URL, archive)
    with tempfile.TemporaryDirectory(prefix="ispd15_") as tmp:
        staging = pathlib.Path(tmp)
        print("  unpacking ...")
        safe_extract(archive, staging)
        count = 0
        for child in sorted(staging.iterdir()):
            if not (child.is_dir() and child.name.startswith("mgc_")):
                continue
            if only and child.name != only:
                continue  # leave the rest in the temporary directory
            target = dest / child.name
            if target.is_dir() and not force:
                continue
            shutil.rmtree(target, ignore_errors=True)
            shutil.move(str(child), str(target))
            count += 1
        print(f"  installed {count} design(s) into {ISPD15_DIR}/")
    archive.unlink(missing_ok=True)


def show_sources() -> None:
    print("KTPlace benchmark sources (original publishers)\n")
    print(f"{ISPD15_DIR}  (LEF/DEF, {len(ISPD15_DESIGNS)} designs)")
    print(f"  {ISPD15_URL}")
    for suite, specs in BOOKSHELF.items():
        print(f"\n{suite}  (Bookshelf)")
        for spec in specs:
            print(f"  {spec['label']}\n    {spec['url']}")


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true", help="print the source URLs and exit")
    ap.add_argument("--suite", choices=[ISPD15_DIR, *BOOKSHELF], help="fetch only this suite")
    ap.add_argument("--design", help="fetch only this design (used with --suite)")
    ap.add_argument("--dest", type=pathlib.Path, default=ROOT,
                    help="output root (default: <repo>/benchmark)")
    ap.add_argument("--force", action="store_true", help="re-fetch existing designs")
    args = ap.parse_args()

    if args.list:
        show_sources()
        return 0

    dest_root = args.dest
    dest_root.mkdir(parents=True, exist_ok=True)

    if args.suite in (None, ISPD15_DIR):
        fetch_ispd15(dest_root, args.force, args.design)
    for suite in BOOKSHELF:
        if args.suite in (None, suite):
            fetch_bookshelf(suite, dest_root, args.design, args.force)
    return 0


if __name__ == "__main__":
    sys.exit(main())
