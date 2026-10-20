# KTPlace benchmarks

The benchmark suites are committed here, gzipped (about 245 MB), grouped by
input format: Bookshelf designs under `bookshelf/`, LEF/DEF designs under
`lefdef/`. ktplace reads `.gz` files directly. The ISPD 2006 designs are the
exception: only their evaluator is committed, and `fetch_benchmarks.py`
downloads the designs. The script fetches any suite from its original publisher,
into the same layout and also gzipped, and the contest suites' own evaluators
into `<suite>/evaluator/`. The committed files are compressed with `pigz -11`
(zopfli), which reads exactly as fast as ordinary gzip; the fetcher, which uses
only the Python standard library, compresses at level 9.

```sh
python3 benchmark/fetch_benchmarks.py            # fetch everything missing
python3 benchmark/fetch_benchmarks.py --list     # show the sources
python3 benchmark/fetch_benchmarks.py --suite ICCAD04 --design ibm01
```

The script only needs the Python standard library. It skips designs and
evaluator files that are already present, so it is safe to re-run.

## The CI designs: `adaptec1` and `ibm01`

`bookshelf/ISPD_2005/adaptec1` is the CI smoke design.
`bookshelf/ICCAD04/ibm01` is the CI multi-row design:

* It is the only benchmark with **multi-row cells** — 120 of its 12,380 movable
  cells are taller than a row — so it is the only design that reaches
  `MultiRowLegalizer` at all. Every other benchmark goes through Abacus.
* It is small and deterministic enough to assert legality on. Adaptec1's verdict
  moves with the machine, so CI deliberately does not gate on it; `ibm01`
  legalizes in well under a second and lands on zero overlapping pairs every
  run, so the multi-row path can be held to its actual contract.

## Sources

| suite | format | designs | source |
|-------|--------|---------|--------|
| `lefdef/ISPD_2015` | LEF/DEF | `mgc_*` (16) | [ispd.cc contest site](https://www.ispd.cc/contests/15/web/benchmarks/ispd_2015_contest_benchmark.tgz) |
| `bookshelf/ICCAD04` | Bookshelf | `ibm01`–`ibm18` | [UMich ICCAD04bench](https://vlsicad.eecs.umich.edu/BK/ICCAD04bench/ibmMSWpinsICCAD04Bench_BOOKSHELF.tar.gz) (IBM-MSwPins) |
| `bookshelf/ICCAD04` | Bookshelf | `dma`, `dsp1`, `dsp2`, `risc1`, `risc2` | [UMich ICCAD04bench](https://vlsicad.eecs.umich.edu/BK/ICCAD04bench/FARADAY_ICCAD04Bench.tar.gz) (Faraday) |
| `bookshelf/ISPD_2005` | Bookshelf | `adaptec1`–`adaptec4`, `bigblue1`–`bigblue4` | [ISPD 2005 contest site](https://www.ispd.cc/contests/05/contest.htm), one archive per design |
| `bookshelf/ISPD06` | Bookshelf | `adaptec5`, `newblue1`–`newblue7` | [ISPD 2006 contest site](https://www.ispd.cc/contests/06/contest.html), one archive per design; fetched, not committed |
| `bookshelf/ICCAD22_D`, `lefdef/ICCAD22_D` | Bookshelf, LEF/DEF | `case01`–`case15` | ICCAD 2022 CAD Contest Problem D (Maxeda), `ProblemD_HiddenCase.tar.gz` on the contest's Google Drive; not committed (1.9 GB) |

## ICCAD 2022 Problem D: detailed macro placement

The 15 cases are three ICCAD 2015 netlists (superblue1: 01, 06, 07, 12, 13; superblue7: 02, 05,
08, 11, 14; superblue10: 03, 04, 09, 10, 15) with different starting macro placements and maximum
displacements (0 to 1700 µm). Each case is kept twice: `lefdef/ICCAD22_D/caseNN/` is the contest
input (`.v`, `.lef`, `.def`, `.mlist`, `.txt`; ktplace cannot read it yet, as the DEF has no `NETS`
and the netlist is only in the `.v`), and `bookshelf/ICCAD22_D/caseNN/` is the evaluator's copy,
with the `.mlist` and `.txt` copied in so `read_macro_constraints` finds them.

```sh
ktplace benchmark/bookshelf/ICCAD22_D/case07 -w output/case07 -tcl scripts/iccad22d/dmp.tcl
scripts/iccad22d/score.sh benchmark/bookshelf/ICCAD22_D/case07 output/case07/case07.dmp output/case07_ntuplace3
```

The contest score is ntuplace3's final HPWL with the macros fixed. `score.sh` runs it; the 2012
static binary needs `scripts/iccad22d/patch_ntuplace3.py` first on kernels without the vsyscall page
(WSL2 among them), and the patched copy goes to `bookshelf/ICCAD22_D/ntuplace3`.

## Layout

One directory per design, holding gzipped files named after the design:

```
benchmark/lefdef/ISPD_2015/mgc_des_perf_a/{floorplan.def.gz, cells.lef.gz, tech.lef.gz, ...}
benchmark/bookshelf/ICCAD04/ibm01/{ibm01.nodes.gz, ibm01.nets.gz, ibm01.pl.gz, ...}
benchmark/bookshelf/ISPD_2005/adaptec1/{adaptec1.nodes.gz, adaptec1.nets.gz, ...}
benchmark/bookshelf/ISPD06/newblue1/{newblue1.nodes.gz, newblue1.nets.gz, ...}
```

The upstream archives are inconsistent — the Faraday archive nests its files in
`DMA/BOOKSHELF/dma_BS.*` with a `_BS` suffix, and some members are gzipped — so
the script normalises all of that away. You do not need to do it by hand.

## Running

```sh
./build/bin/ktplace ./benchmark/lefdef/ISPD_2015/mgc_des_perf_a -w ./output/mgc_des_perf_a
./build/bin/ktplace ./benchmark/bookshelf/ICCAD04/ibm01 -w ./output/ibm01
```

The input format is auto-detected: a directory containing a `.def`/`.def.gz`
is read as LEF/DEF, anything else as Bookshelf. The placement is written to
`<work-dir>/placed.pl`, and SVG frames of the solve plus an HTML gallery to
`<work-dir>/plots/` (open `plots/index.html`).

## File formats

Bookshelf circuits ship as `.nodes` (cells), `.nets` (connectivity), `.pl`
(initial placement), `.scl` (row structure), `.wts` (net weights) and `.aux`
(manifest). Terminals in `.nodes` are the die I/O pads; they are fixed and are
marked `/FIXED` in `.pl`. Gzipped (`.gz`) inputs are read transparently.

## Benchmark status

[`bm_status.md`](bm_status.md) lists our HPWL, legality verdict and runtime per
design next to published results: SimPL (IEEE TCAD 2012) on ISPD 2005, and
Eh?Placer, NTUplace4dr and DREAMPlace 3.0 (ICCAD 2020) on the fenced ISPD 2015
designs.

## ISPD 2005 contest evaluator

`bookshelf/ISPD_2005/evaluator/` holds the contest's own scripts, unchanged, from the
[ISPD 2005 contest page](https://www.ispd.cc/contests/05/contest.htm), written by
the organizer, Gi-Joon Nam (IBM):

- `hpwl.pl`: HPWL with pin offsets, the contest's metric.
- `legal2.pl`: the contest's final legality check (March 2005). It checks that
  terminals did not move, that every movable cell is inside the rows it covers
  and aligned to them, and that nothing overlaps.
- `legal.pl`: the earlier legality check (January 2005), kept for reference.
  `legal2.pl` adds the check against each row's own x-range and reports overlaps
  as `ERROR Type3` rather than `ErrorType3`; `evaluate.sh` uses `legal2.pl`.

They read plain Bookshelf files, so `evaluate.sh` unpacks a design and runs both:

```sh
benchmark/bookshelf/ISPD_2005/evaluator/evaluate.sh benchmark/bookshelf/ISPD_2005/adaptec1 output/adaptec1/placed.pl
#          Total HPWL: 79443114
# Legality: 0 error(s)
```

It exits non-zero if the placement has any legality error. The contest did not
allow cells to be rotated or mirrored (organizer's announcement of 21 January
2005), and `hpwl.pl` ignores the orientation column, scoring every cell as `N`.

## ISPD 2006 contest scripts

ISPD 2006 is not committed. `fetch_benchmarks.py --suite ISPD06` downloads its
designs and the contest's scripts into `bookshelf/ISPD06/evaluator/`: `hpwl.pl`,
`check_legality.pl`, `check_density_target.pl`, and the variant newblue1 needs
with its `transformed.list`. The 2006 score is not plain HPWL. Each design has a
target density (adaptec5 0.5, newblue1 0.8, newblue2 0.9, newblue3 0.8,
newblue4 0.5, newblue5 0.5, newblue6 0.8, newblue7 0.8), and the score is

    scaled HPWL = HPWL * (1 + scaled overflow per bin / 100)

with the overflow measured on a 120 x 120 bin grid; the placement must also be
legal, and the contest scaled the score by run time too (up to 10%).

## ISPD 2015 placement checker

The ISPD 2015 contest scored placements by routing them with Mentor's
Olympus-SoC, which was never released. The only tool it published is
`placement_checker`, which checks that a placement DEF is well formed and legal;
it is in `lefdef/ISPD_2015/evaluator/`.

## Fetching the evaluators

`fetch_benchmarks.py` also downloads each contest's scripts into
`<suite>/evaluator/`, skipping files that are already there, so the committed
ISPD 2005 and 2015 copies are left alone. ICCAD04 is an academic release with
no contest evaluator.
