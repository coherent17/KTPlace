# KTPlace benchmarks

The benchmarks are **not** stored in this repository, with two exceptions
(`adaptec1` and `ibm01`, below). `fetch_benchmarks.py` downloads them from their
original publishers and unpacks them into the layout the ktplace adapters expect.

```sh
python3 benchmark/fetch_benchmarks.py            # fetch everything available
python3 benchmark/fetch_benchmarks.py --list     # show the sources
python3 benchmark/fetch_benchmarks.py --suite ICCAD04 --design ibm01
```

The script only needs the Python standard library. It skips designs that are
already present, so it is safe to re-run.

## The first vendored design: `ISPD_2005/adaptec1`

`benchmark/ISPD_2005/adaptec1/` (5.4 MB, gzipped Bookshelf, dated March 2005) is
committed. It is the CI smoke design, in the tree so that a smoke test cannot
fail on someone else's uptime. `fetch_benchmarks.py` downloads the other seven
ISPD 2005 designs (`adaptec2`–`adaptec4`, `bigblue1`–`bigblue4`) from the
[contest page](https://www.ispd.cc/contests/05/contest.htm), one archive per
design, and the ISPD 2006 designs (`adaptec5`, `newblue1`–`newblue7`) from
[theirs](https://www.ispd.cc/contests/06/contest.html).

## The second vendored design: `ICCAD04/ibm01`

`benchmark/ICCAD04/ibm01/` (436 KB gzipped Bookshelf, dated June 2004) is also
committed. Unlike `adaptec1` its suite *is* fetchable, but it holds 23 designs,
and pulling the whole archive on every push to exercise one of them is a bad
trade for CI. It is here for two reasons:

* It is the only benchmark in the tree with **multi-row cells** — 120 of its
  12,380 movable cells are taller than a row — so it is the only design that
  reaches `MultiRowLegalizer` at all. Every other benchmark goes through Abacus.
* It is small and deterministic enough to assert legality on. Adaptec1's verdict
  moves with the machine, so CI deliberately does not gate on it; `ibm01`
  legalizes in well under a second and lands on zero overlapping pairs every
  run, so the multi-row path can be held to its actual contract.

## Sources

| suite | format | designs | source |
|-------|--------|---------|--------|
| `ISPD_2005` | Bookshelf | `adaptec1` | vendored here; see above |
| `ICCAD04` | Bookshelf | `ibm01` | vendored here; see above |
| `ISPD_2015` | LEF/DEF | `mgc_*` (16) | [ispd.cc contest site](https://www.ispd.cc/contests/15/web/benchmarks/ispd_2015_contest_benchmark.tgz) |
| `ICCAD04` | Bookshelf | `ibm01`–`ibm18` | [UMich ICCAD04bench](https://vlsicad.eecs.umich.edu/BK/ICCAD04bench/ibmMSWpinsICCAD04Bench_BOOKSHELF.tar.gz) (IBM-MSwPins) |
| `ICCAD04` | Bookshelf | `dma`, `dsp1`, `dsp2`, `risc1`, `risc2` | [UMich ICCAD04bench](https://vlsicad.eecs.umich.edu/BK/ICCAD04bench/FARADAY_ICCAD04Bench.tar.gz) (Faraday) |
| `ISPD02` | Bookshelf | `ibm01`–`ibm18` | [UMich ISPD02bench](https://vlsicad.eecs.umich.edu/BK/ISPD02bench/ibmISPD02Bench_Bookshelf.tar.gz) (IBM-MS) |

## Resulting layout

One directory per design, holding plain text files named after the design:

```
benchmark/ISPD_2015/mgc_des_perf_a/{floorplan.def, cells.lef, tech.lef, design.v, ...}
benchmark/ICCAD04/ibm01/{ibm01.nodes.gz, ibm01.nets.gz, ibm01.pl.gz, ...}
benchmark/ICCAD04/dma/{dma.nodes, dma.nets, dma.pl, dma.scl, dma.wts, dma.aux}
benchmark/ISPD02/ibm01/{ibm01.nodes, ibm01.nets, ibm01.pl, ibm01.scl, ibm01.wts, ibm01.aux}
benchmark/ISPD_2005/adaptec1/{adaptec1.nodes.gz, adaptec1.nets.gz, ...}
```

The upstream archives are inconsistent — the Faraday archive nests its files in
`DMA/BOOKSHELF/dma_BS.*` with a `_BS` suffix, and some members are gzipped — so
the script normalises all of that away. You do not need to do it by hand.
`adaptec1` and `ibm01` are the exceptions: they are committed in gzipped form,
which the Bookshelf reader accepts transparently.

## Running

```sh
./build/bin/ktplace ./benchmark/ISPD_2015/mgc_des_perf_a -w ./output/mgc_des_perf_a
./build/bin/ktplace ./benchmark/ICCAD04/ibm01 -w ./output/ibm01
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

## ISPD 2005 contest evaluator

`ISPD_2005/evaluator/` holds the contest's own scripts, unchanged, from the
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
benchmark/ISPD_2005/evaluator/evaluate.sh benchmark/ISPD_2005/adaptec1 output/adaptec1/placed.pl
#          Total HPWL: 79443114
# Legality: 0 error(s)
```

It exits non-zero if the placement has any legality error. The contest did not
allow cells to be rotated or mirrored (organizer's announcement of 21 January
2005), and `hpwl.pl` ignores the orientation column, scoring every cell as `N`.

## ISPD 2006 contest evaluator

`ISPD06/evaluator/` holds the 2006 contest's scripts, unchanged apart from the
`.pl` suffix: `hpwl.pl` and `check_legality.pl` (the 2005 scripts, lightly
revised), `check_density_target.pl`, and the variant newblue1 needs with its
`transformed.list`. The 2006 score is not plain HPWL. Each design has a target
density, and the score is

    scaled HPWL = HPWL * (1 + scaled overflow per bin / 100)

with the overflow measured on a 120 x 120 bin grid. The placement must also be
legal. `evaluate.sh` runs all three and prints the score; the target densities
are the contest's (adaptec5 0.5, newblue1 0.8, newblue2 0.9, newblue3 0.8,
newblue4 0.5, newblue5 0.5, newblue6 0.8, newblue7 0.8), and a third argument
overrides it:

```sh
benchmark/ISPD06/evaluator/evaluate.sh benchmark/ISPD06/newblue1 output/newblue1/placed.pl
```

The contest also scaled the score by run time (up to 10%), which the script
does not attempt.

## ISPD 2015 placement checker

The ISPD 2015 contest scored placements by routing them with Mentor's
Olympus-SoC, which was never released. The only tool it published is
`placement_checker`, which checks that a placement DEF is well formed and legal;
`fetch_benchmarks.py` downloads it into `ISPD_2015/evaluator/`.

## Fetching the evaluators

`fetch_benchmarks.py` also downloads each contest's scripts into
`<suite>/evaluator/`, skipping files that are already there, so the committed
ISPD 2005 and 2006 copies are left alone. The ICCAD04 and ISPD02 suites are
academic releases with no contest evaluator.
