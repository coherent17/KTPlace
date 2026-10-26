[FastDP contents](README.md) · [2. The optimal region and Global Swap →](02-global-swap.md)

# 1. Detailed placement and the FastDP flow

*In the paper: §I, §II, Figure 1.*

**Goal:** shorten the wires of a legal placement by moving cells, without ever ending
with an illegal one.

### 1.1 What detailed placement is for

Global placement looks at the whole chip at once. To stay fast it uses approximations
(a smooth wirelength model, density bins) and pays attention to where cells are relative
to each other globally, not to the exact neighbourhood of each cell. Legalization then
moves cells onto rows with no regard for wirelength. What comes out is legal, but full of
small local mistakes: a cell that would be better two sites to the left, two cells that
should swap, a cell one row too high.

**Detailed placement** repairs those mistakes. It starts from a legal placement and
transforms it into another legal placement with shorter wires. Because it works on exact
positions, it can use the exact wirelength measure, **half-perimeter wirelength** (HPWL,
[Foundations 1](../../foundations/01-placement-problem.md)): for each net, the width plus
the height of the smallest rectangle around its pins.

The paper's starting observation is that detailed placers of the time were either weak
(window-based branch and bound with a small window only fixes very local problems) or
slow: Domino gained 5.9% wirelength over FastPlace's own result, but the FastPlace +
Domino flow was 7.6 times slower than FastPlace alone. Good global placers had become
fast, so a detailed placer needed to be both effective and fast.

### 1.2 Segments

A **segment** is a stretch of a row where standard cells may go: a whole row, or, when
fixed macros and placement blockages cut through the rows, each unblocked piece of one.
Detailed placement only moves standard cells inside segments; macros stay where they
are.

### 1.3 Four techniques

| Technique | What it does | Reach |
| --- | --- | --- |
| **Global Swap** | for a cell, finds the region where its wires are shortest and swaps it with a cell or a gap there | anywhere on the chip |
| **Vertical Swap** | swaps a cell with a nearby cell in the row above or below, towards that region | one row |
| **Local Re-ordering** | tries every left-to-right order of 3 neighbouring cells in a segment | 3 cells |
| **Single-Segment Clustering** | slides all cells of a segment, keeping their order, to their best positions | one segment |

Global Swap does most of the work, because it can move a cell far in one step. The other
three fix what it leaves: Vertical Swap fixes small vertical errors, Local Re-ordering
small horizontal errors, and clustering the exact x-positions inside each segment.

### 1.4 The flow (Figure 1)

```
Detailed placement:
    Single-Segment Clustering
    repeat
        Global Swap
        Vertical Swap
        Local Re-ordering
    until no significant improvement in wirelength
    repeat
        Single-Segment Clustering
    until no significant improvement in wirelength
```

```mermaid
flowchart LR
    in["legal placement"]:::io --> c1["Single-Segment<br/>Clustering"]:::stage
    c1 --> gs["Global Swap"]:::stage
    gs --> vs["Vertical Swap"]:::stage
    vs --> lr["Local Re-ordering"]:::stage
    lr -->|"still improving"| gs
    lr -->|"no significant gain"| c2["Single-Segment<br/>Clustering, repeated"]:::stage
    c2 --> out["better legal placement"]:::io
    classDef io fill:#fef3c7,stroke:#d97706,color:#451a03
    classDef stage fill:#ccfbf1,stroke:#0f766e,color:#042f2e
```

- **Clustering first** gives the main loop a good starting point: every segment's cells
  already sit at their best positions for their current order.
- **The main loop** changes which segment each cell is in and the order of cells, until
  an iteration no longer improves the wirelength significantly.
- **Clustering last** fixes the positions again for the final orders. It is repeated
  because it is only exact for one segment while the others are held still
  ([chapter 4](04-single-segment-clustering.md)), so placing segment after segment needs
  a few rounds to settle.

> [!IMPORTANT]
> **Key takeaways**
>
> - Detailed placement turns one legal placement into another with shorter HPWL.
> - Cells move only inside segments: rows, or the pieces of rows between macros and blockages.
> - Global Swap moves cells far; Vertical Swap, Local Re-ordering and clustering fix small vertical, horizontal and positional errors.
> - Clustering runs first, the three swaps loop until the gain is small, and clustering repeats at the end.

### Exercises

**1.** Why can detailed placement use HPWL directly, when global placement uses a
smoother model?

<details class="success" markdown>
<summary>Answer</summary>

Detailed placement evaluates concrete moves of a few cells in a legal placement: it only
needs the change in HPWL of the nets those cells touch, which is cheap to compute
exactly. Global placement solves for all positions at once with linear algebra, which
needs a smooth, differentiable model.

</details>

**2.** Why is clustering run again, several times, at the end?

<details class="success" markdown>
<summary>Answer</summary>

The main loop changed segments and orders, so the cells' positions inside each segment
are no longer the best ones. Clustering is only exact for one segment with all others
fixed; after one segment moves, its neighbours' best positions change, so a few rounds
are needed.

</details>

---

[FastDP contents](README.md) · [2. The optimal region and Global Swap →](02-global-swap.md)
