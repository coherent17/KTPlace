[← 1. The SimPL idea and initial placement](01-the-simpl-idea.md) · [SimPL contents](README.md) · [3. Anchors and convergence →](03-anchors-and-convergence.md)

# 2. Look-ahead legalization

*In the paper: §4.2, Algorithm 1, Figures 3–5.*

**Goal:** take the clumped lower-bound placement and move cells so that no bin is
over-full, while keeping the cells' **relative order** (who is left of whom, who is
above whom) as much as possible. Keeping the order keeps most of the wirelength
quality the linear solver found. The paper calls this a *projection of the
lower-bound placement onto the set of feasible placements*: find a nearby
placement that satisfies the constraints. (Footnote *b*: this is related to
optimal transport, the mathematics of moving mass at minimum cost; SimPL does not
need an optimal answer, only a fast good one.)

### 2.1 Density, γ, and over-filled bins

For every bin of the grid compute

- $A_c$: total area of the cells in the bin;
- $A_a$: total area of free cell sites in the bin (bin area minus fixed obstacles).

The bin's density is $A_c / A_a$. Given a **target density** $0 < \gamma \le 1$
(the ISPD 2005 runs use $\gamma = 1.0$; a smaller γ leaves whitespace), a bin is
**γ-overfilled** if $A_c / A_a > \gamma$.

### 2.2 Clusters and their regions (Algorithm 1, lines 1–5)

1. Over-filled bins that touch each other are grouped into **clusters** with
   **breadth-first search** (BFS: start at one over-filled bin, add its over-filled
   neighbours, then their neighbours, and so on).
2. For each cluster, find a **minimal rectangle $R$ around it whose density is at
   most γ**: grow the box around the cluster until the free space inside is enough
   for the cells inside. Now it is at least possible to fit those cells in $R$.
3. Put $R$ in a queue with `level = 1`.

### 2.3 Top-down geometric partitioning (lines 6–19)

Take a region $B$ from the queue. Stop splitting it if it is small enough or
`level` ≥ 10. Otherwise, with $M$ the movable cells in $B$:

- **$C_c$, the cell-area cutline**: sort the cells in $M$ by x (on odd levels;
  y on even levels: the cut direction alternates, vertical first) and find the
  position that splits the *cell area* in half. Cells on the left of $C_c$ form
  $M_0$, cells on the right $M_1$.
- **$C_B$, the whitespace cutline**: the position that splits the *free space* of
  $B$ in half. It creates two sub-regions $B_0$ (left) and $B_1$ (right).

The clump is lopsided: $C_c$ sits where the cells are, $C_B$ where the room is
(paper Figure 3). The fix is to move $M_0$ into $B_0$ and $M_1$ into $B_1$: half the
cells into half the room on each side, which **equalizes the density of the two
halves**. The move is done by **nonlinear scaling** (next). Then $B_0$ and $B_1$ go
back into the queue with `level + 1`, and the next cut runs in the other direction.

> [!TIP]
> The paper's footnote *c* explains why this order-preserving packing is reasonable:
> a region filled to capacity leaves the placer little freedom anyway, so sorting and
> packing loses little wirelength there.

### 2.4 Nonlinear scaling (Figure 4)

Take sub-region $B_0$ and its cells $M_0$ (the case of $B_1$ is the mirror image).

1. **Stripes.** Cut $B_0$ into stripes parallel to $C_B$. First cut along the edges
   of every fixed obstacle inside, so no stripe is half obstacle. Then any stripe
   whose free area is more than 1/10 of the region's free area is cut further.
2. **Sort** the cells of $M_0$ by their distance from $C_B$.
3. **Greedy packing.** The cell farthest from $C_B$ goes into the stripe farthest
   from $C_B$. Each next cell goes into the farthest stripe that is not yet full. A
   stripe with free area $A_a$ is full when the area assigned to it reaches
   $\gamma A_a$.
4. **Linear scaling inside each stripe.** In each stripe, spread its cells
   evenly in proportion to their old positions: if the stripe's cells used to
   span $[u, v]$ and the stripe is $[s_0, s_1]$, a cell at $x$ goes to

$$
x' = s_0 + \frac{x - u}{v - u}\,(s_1 - s_0)
$$

(keeping the cell inside the stripe). Each stripe uses its own stretch factor:
the overall map is a different straight line in each stripe, which makes the whole
transformation **nonlinear**, hence the name. Order is preserved, because the
cells are assigned in sorted order and each stripe's map is increasing.

**Numerical example (1-D).** $B_0 = [0, 50]$, $C_B = 50$, $\gamma = 1$, no obstacles,
five stripes of width 10, each with free area 10. Eight cells of width 5 have been
clumped at centres $22, 23, \dots, 29$ (all left of $C_c = 30$).

| Step | Result |
| --- | --- |
| Sort by distance from $C_B = 50$, farthest first | 22, 23, 24, 25, 26, 27, 28, 29 |
| Stripe $[0,10]$ (farthest) fills with area 10 | 22, 23 |
| Stripe $[10,20]$ | 24, 25 |
| Stripe $[20,30]$ | 26, 27 |
| Stripe $[30,40]$ | 28, 29 |
| Stripe $[40,50]$ | empty (only 40 of 50 units of area to place) |
| Scale inside each stripe | centres 2.5, 7.5, 12.5, 17.5, 22.5, 27.5, 32.5, 37.5 |

No overlap, the left-to-right order is unchanged, and the cells moved toward where
the room is. Cells that were far from the cut moved far; cells near it moved little.

Paper Figure 5 shows this on the real benchmark adaptec1: one vertical cut and
scaling, then each half cut horizontally, and so on; four rounds of scaling spread
the clump over the region.

### 2.5 What comes out

After all regions are processed, every cell has a new location: that is the
**upper-bound placement**. It is "roughly legal": no bin is (much) over-full, but
cells still overlap a little inside bins and are not snapped to rows. It is
not refined further; it is only used as anchors ([Chapter 3](03-anchors-and-convergence.md)), and the last one is the
output of global placement.

> [!IMPORTANT]
> **Key takeaways**
>
> - Over-full bins are grouped into clusters, and each cluster grows into a region with room for its cells.
> - Each region is cut where the cell area splits in half ($C_c$) and where the free space splits in half ($C_B$); the cells move to the matching side.
> - Nonlinear scaling packs cells in order into stripes and stretches each stripe linearly, so the relative order is kept.

---

[← 1. The SimPL idea and initial placement](01-the-simpl-idea.md) · [SimPL contents](README.md) · [3. Anchors and convergence →](03-anchors-and-convergence.md)
