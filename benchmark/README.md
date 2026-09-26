# KTPlace benchmarks

The benchmarks are **not** stored in this repository (they are several GB).
`fetch_benchmarks.py` downloads them from their original publishers and unpacks
them into the layout the ktplace adapters expect.

```sh
python3 benchmark/fetch_benchmarks.py            # fetch everything available
python3 benchmark/fetch_benchmarks.py --list     # show the sources
python3 benchmark/fetch_benchmarks.py --suite ICCAD04 --design ibm01
```

The script only needs the Python standard library. It skips designs that are
already present, so it is safe to re-run.

## Sources

| suite | format | designs | source |
|-------|--------|---------|--------|
| `ISPD_2015` | LEF/DEF | `mgc_*` (16) | [ispd.cc contest site](https://www.ispd.cc/contests/15/web/benchmarks/ispd_2015_contest_benchmark.tgz) |
| `ICCAD04` | Bookshelf | `ibm01`–`ibm18` | [UMich ICCAD04bench](https://vlsicad.eecs.umich.edu/BK/ICCAD04bench/ibmMSWpinsICCAD04Bench_BOOKSHELF.tar.gz) (IBM-MSwPins) |
| `ICCAD04` | Bookshelf | `dma`, `dsp1`, `dsp2`, `risc1`, `risc2` | [UMich ICCAD04bench](https://vlsicad.eecs.umich.edu/BK/ICCAD04bench/FARADAY_ICCAD04Bench.tar.gz) (Faraday) |
| `ISPD02` | Bookshelf | `ibm01`–`ibm18` | [UMich ISPD02bench](https://vlsicad.eecs.umich.edu/BK/ISPD02bench/ibmISPD02Bench_Bookshelf.tar.gz) (IBM-MS) |
## Resulting layout

One directory per design, holding plain text files named after the design:

```
benchmark/ISPD_2015/mgc_des_perf_a/{floorplan.def, cells.lef, tech.lef, design.v, ...}
benchmark/ICCAD04/ibm01/{ibm01.nodes, ibm01.nets, ibm01.pl, ibm01.scl, ibm01.wts, ibm01.aux}
benchmark/ICCAD04/dma/{dma.nodes, dma.nets, dma.pl, dma.scl, dma.wts, dma.aux}
benchmark/ISPD02/ibm01/{ibm01.nodes, ibm01.nets, ibm01.pl, ibm01.scl, ibm01.wts, ibm01.aux}
```

The upstream archives are inconsistent — the Faraday archive nests its files in
`DMA/BOOKSHELF/dma_BS.*` with a `_BS` suffix, and some members are gzipped — so
the script normalises all of that away. You do not need to do it by hand.

## Running

```sh
./build/bin/ktplace mgc_des_perf_a ./benchmark/ISPD_2015/mgc_des_perf_a \
    ./output/mgc_des_perf_a.pl
./build/bin/ktplace ibm01 ./benchmark/ICCAD04/ibm01 ./output/ibm01.pl
```

The input format is auto-detected: a directory containing a `.def`/`.def.gz`
is read as LEF/DEF, anything else as Bookshelf.

Add `-p <dir>` to write SVG frames of the solve plus an HTML gallery:

```sh
./build/bin/ktplace mgc_des_perf_a ./benchmark/ISPD_2015/mgc_des_perf_a \
    ./output/mgc_des_perf_a.pl -p ./output/plots
```

## File formats

Bookshelf circuits ship as `.nodes` (cells), `.nets` (connectivity), `.pl`
(initial placement), `.scl` (row structure), `.wts` (net weights) and `.aux`
(manifest). Terminals in `.nodes` are the die I/O pads; they are fixed and are
marked `/FIXED` in `.pl`. Gzipped (`.gz`) inputs are read transparently.
