[← 3. Vertical Swap and Local Re-ordering](03-vertical-swap-and-reordering.md) · [FastDP contents](README.md) · [5. Results →](05-results.md)

# 4. Single-Segment Clustering

*In the paper: §III-D, Figures 4–5, Table II, Theorem 1.*

**Goal:** with each cell's segment and the order inside every segment fixed, slide the
cells of one segment to the positions with the shortest wirelength.

### 4.1 The problem

> **Fixed-order single segment placement.** A segment $S$ holds $n$ standard cells
> $C_1, C_2, \dots, C_n$, in a fixed left-to-right order ($C_i$ is left of $C_j$ when
> $i < j$). Every cell outside $S$ is fixed. Find non-overlapping positions for the cells
> of $S$ that minimize the total HPWL.

This is the **single-row problem** of Kahng, Tucker and Zelikovsky (1999), who solved it
optimally by dynamic programming. FastDP solves it optimally too, with a simpler and
faster clustering algorithm.

It is the same shape as Abacus's PlaceRow
([Abacus chapter 3](../../legalization/abacus/03-clusters-and-dynamic-programming.md)):
cells in a fixed order, no overlap, merge neighbours into rigid clusters. Only the
objective differs: Abacus minimizes squared *movement*, FastDP minimizes *wirelength*, and
that changes how a cluster finds its best position.

### 4.2 A cluster's wirelength is a bathtub with steps

A **cluster** is one cell, or several consecutive cells abutted together, in their order;
its width is the sum of their widths. **Clustering** two clusters abuts them into one.

Hold everything outside a cluster still and slide the cluster along x. Its wirelength
$W(x)$ is a **convex piecewise-linear function**, the same bathtub sum as in
[chapter 2](02-global-swap.md#21-the-best-position-of-one-cell): every net touching the
cluster adds slope −1 to the left of its box and +1 to the right of it. So the slopes of the
pieces are

$$
\dots,\; -3,\; -2,\; -1,\; 0,\; 1,\; 2,\; 3,\; \dots
$$

- The points where the slope changes are the **bounds**: the left and right edges of the
  bounding boxes of the cluster's nets (without the cluster's own cells).
- The piece with slope 0 is the cluster's **optimal region** in x.
- Its midpoint is the **Optimal Region Center** $X_c$. That is where the algorithm puts the
  cluster.

For a cluster of several cells, each bound is taken relative to the cluster's left edge: a
cell sitting $d$ to the right of the left edge reaches a position $b$ when the cluster is at
$b - d$, so its bounds are shifted left by $d$.

### 4.3 Neighbours in the same segment

To compute the bounds, everything except the cluster must be fixed. But the other cells of
segment $S$ are moving too. If a net connects the cluster to another cell $C_j$ of the same
segment, where is $C_j$?

The order answers it. If $C_j$ is left of the cluster, it will stay left of it whatever
happens, so it is treated as sitting at the **left end** of segment $S$; if it is right of
the cluster, at the **right end**. This pins those pins at the extreme where the cluster can
never pass them, and it does not change where the cluster's optimum is: with $C_j$
anywhere to its left, the cluster's best position does not depend on exactly where.

### 4.4 The algorithm (Figure 4)

The idea: put every cluster at its Optimal Region Center; where two neighbours overlap,
merge them for good and put the merged cluster at *its* center; repeat until nothing
overlaps.

```
Single-Segment Clustering:
    every cell C_i starts as its own cluster K_i
    repeat
        for each cluster: find its bounds and its Optimal Region Center X_c,
                          and put it at X_c
        walk the clusters from left to right:
            if the current cluster overlaps the next one:
                merge them into one cluster (it is never broken again)
                merge their bounds lists
                put the merged cluster at the center of its new optimal region
            else:
                move on to the next cluster
    until no two clusters overlap
    give each cell the position its cluster assigns it
```

If a cluster's optimal region reaches past an end of the segment, the cluster is placed
against that end instead, so no cell leaves the segment.

> [!TIP]
> **Example.** A segment $[0, 20]$ holds cells A then B, both width 4, each on one net.
> A's net, without A, spans $[8, 10]$ in x; B's net, without B, spans $[9, 11]$.
>
> - Alone, A's optimal region is $[8, 10]$ (center 9) and B's is $[9, 11]$ (center 10).
>   A at 9 covers $[9, 13]$, B at 10 starts inside it: they overlap.
> - Merge them. B sits 4 to the right of the cluster's left edge, so its bounds 9, 11 become
>   5, 7. The merged bounds are 5, 7, 8, 10; the medians are 7 and 8, so the optimal region
>   is $[7, 8]$ and its center is 7.5.
> - The cluster goes to 7.5: A at $[7.5, 11.5]$, B at $[11.5, 15.5]$. Each net is 0.5 too
>   long, total 1. At cluster position 8 the total is also 1 (A on target, B 1 too far): the
>   whole region $[7, 8]$ is optimal, and the center is one point of it.

### 4.5 Why it is optimal (Theorem 1, Figure 5)

**Theorem.** The algorithm finds an optimal solution of the fixed-order single segment
problem.

The paper gives a sketch. If the clusters are right, putting each at the center of its
optimal region is clearly optimal, so suppose a merge was wrong: in a truly optimal
solution, two cells $a$ and $b$ have a gap between them, but the algorithm put them in the
same cluster when it merged clusters A (containing $a$) and B (containing $b$). It merged them
because, each at its own optimum, A and B overlapped; so with the order kept, they cannot both
be at their optimal regions. In the supposedly optimal solution, then, either A wants to move
right or B wants to move left, towards each other, to shorten the wires. There is a gap between
them, so one of them can move into it without creating overlap, and the wirelength drops: the
"optimal" solution was not optimal. So the algorithm never merges wrongly.

### 4.6 Cost, and keeping only a few bounds (Table II)

There are $n$ cells, so at most $n - 1$ merges. Each step is constant time except merging two
bounds lists, which takes time proportional to the number of bounds. With every bound kept,
the cost grows with $n$ times the length of the lists.

In practice FastDP keeps only a **constant number of bounds** per cluster: the ones around
the middle, where the slope-0 piece is. Merging then takes constant time and the whole
algorithm is $O(n)$. That can lose optimality when the optimum lies outside the kept window,
but very little. The paper keeps **16 bounds**. On the 8th segment of ibm01, a segment with
a lot of room to improve:

| bounds kept | 4 | 8 | 12 | 16 | 20 | all (optimal) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| wirelength decrease | 13600 | 14060 | 14210 | 14377 | 14425 | 14425 |

On the 1st to 7th segments of ibm01, 8 bounds already reach the optimum.

### 4.7 Why it runs more than once

Clustering is optimal for **one segment, with every other segment fixed**. Placing segment
after segment changes the pins that the next segments see, so the result of a single sweep
is not optimal for the whole chip. That is why the flow repeats clustering until the
improvement is no longer significant ([chapter 1](01-the-flow.md#14-the-flow-figure-1)).

> [!IMPORTANT]
> **Key takeaways**
>
> - With the order fixed and the other segments still, placing one segment for minimum HPWL is solved exactly.
> - A cluster's wirelength is convex and piecewise linear; its best position is the center of the slope-0 piece, found from the bounds of its nets.
> - Pins on other cells of the same segment are placed at the segment's end on their side, which the fixed order makes safe.
> - Clusters at their centers merge when they overlap and never split; keeping 16 bounds per cluster makes it $O(n)$ with almost no loss.

### Exercises

**1.** Why can the slope of $W(x)$ only take integer values?

<details class="success" markdown>
<summary>Answer</summary>

Each net touching the cluster contributes −1, 0 or +1 to the slope (left of its box,
inside, right of it). The total is a sum of such numbers, so it is an integer, and it
changes by one at every bound.

</details>

**2.** A cluster has bounds 3, 5, 6, 12 (relative to its left edge). The segment leaves
it room only for left-edge positions $0 \le x \le 4$. Where does it go?

<details class="success" markdown>
<summary>Answer</summary>

The medians are 5 and 6, so the optimal region is $[5, 6]$, center 5.5. That is beyond the
segment's allowed range, so the cluster goes to the nearest allowed position, 4.

</details>

**3.** How is this algorithm related to Abacus's PlaceRow?

<details class="success" markdown>
<summary>Answer</summary>

Both keep the cells' order, place clusters of abutting cells at their best position, and
merge overlapping neighbours. PlaceRow minimizes squared movement, so a cluster's best
position is one division, $q_c / e_c$. Here the objective is linear wirelength, so the best
position comes from the median of the cluster's bounds.

</details>

---

[← 3. Vertical Swap and Local Re-ordering](03-vertical-swap-and-reordering.md) · [FastDP contents](README.md) · [5. Results →](05-results.md)
