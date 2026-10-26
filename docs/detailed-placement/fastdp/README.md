[Documentation home](../../README.md)

# FastDP

A guided reading of **"An Efficient and Effective Detailed Placement Algorithm"**, Min
Pan, Natarajan Viswanathan and Chris Chu, *ICCAD* 2005, pages 48–55. The placer it
describes is known as **FastDP** (FastPlace-DP). See [references](../../references.md)
for links.

**In one paragraph.** FastDP is a detailed placer: it takes a *legal* placement and
moves cells to shorten the wires, keeping the placement legal. It combines four
techniques. **Global Swap** computes, for each cell, the region where its wires would
be shortest (from the medians of its nets' bounding boxes) and swaps the cell with a
cell or a gap there. **Vertical Swap** moves a cell one row towards that region.
**Local Re-ordering** tries every order of three neighbouring cells. **Single-Segment
Clustering** slides the cells of a row segment, without changing their order, to their
exact best positions. The first three run in a loop until the wirelength stops
improving, and clustering runs before and after the loop.

**Where it sits in the flow.** FastDP is the last placement stage in KTPlace. It runs
after legalization ([Abacus](../../legalization/abacus/README.md) or multi-row
legalization) and its result is the placement that gets written out.

## Read first

[Foundations](../../foundations/README.md) chapter 1 (the placement problem and HPWL)
and chapter 2 (slopes and the minimum of a function). The [Abacus](../../legalization/abacus/README.md)
guide is not required, but its [chapter 3](../../legalization/abacus/03-clusters-and-dynamic-programming.md)
uses the same clustering idea with a different objective, and reading both makes each
one clearer.

## Chapters

| # | Chapter | What you learn | Paper |
| --- | --- | --- | --- |
| 1 | [Detailed placement and the FastDP flow](01-the-flow.md) | what detailed placement may change, segments, the four techniques, Figure 1 | §I, §II |
| 2 | [The optimal region and Global Swap](02-global-swap.md) | the median rule, the overlap penalties, the benefit of a swap | §III-A, Figures 2–3, Table I |
| 3 | [Vertical Swap and Local Re-ordering](03-vertical-swap-and-reordering.md) | moving one row at a time, trying all orders of three cells | §III-B, §III-C |
| 4 | [Single-Segment Clustering](04-single-segment-clustering.md) | the fixed-order segment problem, bounds, the clustering algorithm and its proof | §III-D, Figures 4–5, Table II |
| 5 | [Results](05-results.md) | the IBM and ISPD 2005 comparisons with Fengshui, rowIroning and Domino | §IV, Tables III–VI |

```mermaid
flowchart LR
    f["Foundations 1, 2"]:::found --> d1["1 · The flow"]:::simpl
    d1 --> d2["2 · Global Swap"]:::simpl
    d2 --> d3["3 · Vertical Swap, Re-ordering"]:::simpl
    d3 --> d4["4 · Single-Segment Clustering"]:::simpl
    d4 --> d5["5 · Results"]:::simpl
    classDef io fill:#fef3c7,stroke:#d97706,color:#451a03
    classDef stage fill:#ccfbf1,stroke:#0f766e,color:#042f2e
    classDef found fill:#e0e7ff,stroke:#4f46e5,color:#1e1b4b
    classDef simpl fill:#fce7f3,stroke:#db2777,color:#500724
    classDef mov fill:#dbeafe,stroke:#2563eb,color:#172554
    classDef fixed fill:#e5e7eb,stroke:#374151,color:#111827
```

The [glossary](../../glossary.md) lists every symbol used here and has the whole
algorithm in a few lines.
