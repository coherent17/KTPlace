[Documentation home](../../README.md)

# SimPL

A guided reading of **"SimPL: An Algorithm for Placing VLSI Circuits"**, Myung-Chul
Kim, Dong-Jin Lee and Igor L. Markov, *Communications of the ACM* 56(6), June 2013
(the original version is in IEEE TCAD, January 2012). See
[references](../../references.md) for links.

**In one paragraph.** SimPL is a global placer. It keeps two placements at once: a
*lower bound*, which minimizes wirelength by solving a linear system and ignores
overlap, and an *upper bound*, which spreads that solution into an almost legal
placement by top-down partitioning and nonlinear scaling (*look-ahead
legalization*). Every iteration, each cell is tied to its upper-bound location by an
imaginary spring that gets stiffer over time, which pulls the two placements
together. When they are close, the upper bound is the result.

**Where it sits in the flow.** SimPL is KTPlace's default global placer. Its output
goes to legalization (Abacus) and detailed placement (FastDP).

## Read first

All five [foundations](../../foundations/README.md) chapters. Chapters 3 to 5 are
part of the paper's own background section (§2) and are used throughout this guide.

## Chapters

| # | Chapter | What you learn | Paper |
| --- | --- | --- | --- |
| 1 | [The SimPL idea and initial placement](01-the-simpl-idea.md) | the lower-bound and upper-bound placements, the overall flow | §3, §4.1, Figure 2 |
| 2 | [Look-ahead legalization](02-look-ahead-legalization.md) | Algorithm 1: over-filled bins, cutlines, nonlinear scaling | §4.2, Figures 3–5 |
| 3 | [Anchors and convergence](03-anchors-and-convergence.md) | pseudonets, the α schedule, the main loop, when it stops | §4.3, Figures 6–8 |
| 4 | [Speed, results and parallelism](04-speed-results-parallelism.md) | complexity, benchmark results, multi-threading | §4.4, §5, §6, Table 1 |

```mermaid
flowchart LR
    f["Foundations 3, 4, 5"]:::found --> s1["1 · SimPL idea"]:::simpl
    s1 --> s2["2 · Look-ahead legalization"]:::simpl
    s2 --> s3["3 · Anchors and convergence"]:::simpl
    s3 --> s4["4 · Speed, results, parallelism"]:::simpl
    classDef io fill:#fef3c7,stroke:#d97706,color:#451a03
    classDef stage fill:#ccfbf1,stroke:#0f766e,color:#042f2e
    classDef found fill:#e0e7ff,stroke:#4f46e5,color:#1e1b4b
    classDef simpl fill:#fce7f3,stroke:#db2777,color:#500724
    classDef mov fill:#dbeafe,stroke:#2563eb,color:#172554
    classDef fixed fill:#e5e7eb,stroke:#374151,color:#111827
```

The [glossary](../../glossary.md) lists every symbol used here and has the whole
algorithm in ten lines.
