[← 3. Clusters and dynamic programming](03-clusters-and-dynamic-programming.md) · [Abacus contents](README.md)

# 4. Complexity and results

*In the paper: §5, §6, Figures 2–3, Table 4.*

### 4.1 PlaceRow is linear

Count the work in Algorithm 2 for a row of $N_r$ cells:

- **AddCell** runs once per cell: $N_r$ calls, constant time each.
- **AddCluster** runs once per merge. Every merge reduces the number of clusters by one,
  and there are never more than $N_r$ clusters, so there are at most $N_r - 1$ merges,
  however the cascades are spread out.
- **Assigning positions** (lines 14–17) touches each cell once.

So PlaceRow is $O(N_r)$ in the worst case. Its constant is small, a few additions and
one division per step, which matters because Algorithm 1 calls it several times for
every cell of the circuit.

### 4.2 Abacus is quadratic in the worst case

With $N$ cells in the circuit, $R$ rows and at most $\hat N_r$ cells in a row, Algorithm 1
tries each of the $N$ cells in up to $R$ rows, and each try is a PlaceRow of cost
$O(\hat N_r)$:

$$
O(N \cdot R \cdot \hat N_r).
$$

For a square chip with roughly square cells, $R \approx \sqrt N$ rows and
$\hat N_r \approx \sqrt N$ cells per row, giving $O(N^2)$.

That bound assumes every cell is tried in every row, which the row search of
[chapter 1](01-legalization-and-tetris.md#15-three-practical-details) avoids.
Measured on circuits from $10^4$ to beyond $10^6$ cells (IBM-PLACE 2.0, ISPD 2005 and
ISPD 2006 benchmarks), the CPU time grows like $\Theta(N^{1.19})$, close to linear
(paper Figure 3). The worst case is never reached.

### 4.3 Movement

The paper reports movement as Euclidean distance, $\sqrt{(x_i - x'_i)^2 + (y_i - y'_i)^2}$
per cell, even though PlaceRow minimizes the squared x-distance. Average movement is the
total divided by the number of cells.

Paper Figure 2 is a histogram of the movement for ibm12e, normalized to the average cell
dimension. Abacus's histogram peaks closer to zero than Tetris's: more cells move a
little, fewer move far. **Average movement is about 30% lower with Abacus.**

### 4.4 Routability-driven placement

The main experiment legalizes routability-driven global placements from Kraftwerk on the
IBM-PLACE 2.0 suite (16 circuits, ibm01e/h to ibm12e/h without ibm03–06). Routability was
optimized during global placement, so legalization should disturb it as little as
possible. After legalization the same detailed placement is applied in both cases
(optimal single-row placement for HPWL, then greedy cell flipping and pair exchange), and
the result is routed with Cadence WarpRoute.

Averages relative to Abacus (paper Table 4):

| | Abacus | Tetris |
| --- | ---: | ---: |
| average movement | 1.000 | 1.456 |
| routed wirelength | 1.000 | 1.012 |
| number of vias | 1.000 | 1.010 |
| routing CPU time | 1.00 | 0.995 |
| legalization share of total placement CPU time | 6.6% | 0.5% |

So Tetris moves cells about 46% further on average (Abacus moves about 30% less), and its
placements route with about 1% more wire and 1% more vias. Abacus spends more time in
legalization, but Kraftwerk is a fast global placer and the whole placement process takes
only about 7% longer.

### 4.5 What Abacus does not do

- **Macros.** They are assumed placed and fixed; Abacus only legalizes standard cells
  around them, through the subrows of [chapter 1](01-legalization-and-tetris.md).
- **Cells taller than a row.** All cells are assumed to be one row high.
- **Wirelength.** Abacus minimizes movement, not wirelength. Improving wirelength is left
  to detailed placement (for example [FastDP](../../detailed-placement/fastdp/README.md)).

> [!IMPORTANT]
> **Key takeaways**
>
> - PlaceRow is $O(N_r)$: one AddCell per cell and at most $N_r - 1$ merges.
> - Abacus is $O(N^2)$ in the worst case, but $\Theta(N^{1.19})$ in practice because only a few rows are tried per cell.
> - Average movement is about 30% lower than Tetris; routed wirelength and vias improve by about 1%; total placement time grows by about 7%.

### Exercises

**1.** A row receives 1000 cells and ends up as a single cluster. How many AddCell and
AddCluster calls were made, at most?

<details class="success" markdown>
<summary>Answer</summary>

1000 AddCell calls, one per cell, and at most 999 AddCluster calls: each merge removes
one cluster, and 1000 one-cell clusters become one after 999 merges.

</details>

**2.** Why is the measured complexity so much better than $O(N^2)$?

<details class="success" markdown>
<summary>Answer</summary>

The $O(N^2)$ bound assumes each cell is tried in all $\sqrt N$ rows. The row search
stops as soon as the vertical distance to the next row exceeds the best cost found, so
each cell is tried in only a few rows.

</details>

---

[← 3. Clusters and dynamic programming](03-clusters-and-dynamic-programming.md) · [Abacus contents](README.md)
