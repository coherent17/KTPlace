[Documentation home](README.md)

# Glossary and cheat sheet

| Symbol / term | Meaning |
| --- | --- |
| $N = (E, V)$ | netlist: nets $E$, cells $V$ |
| HPWL | half the perimeter of a net's bounding box, summed over nets (Eq. 1) |
| $w_{ij}$ | weight (spring stiffness) of the edge between cells $i$ and $j$ |
| $\Phi_G$ | quadratic wirelength, $\sum w_{ij}[(x_i-x_j)^2 + (y_i-y_j)^2]$ (Eq. 2) |
| $Q_x$, $\vec c_x$ | matrix of movable–movable springs; vector of movable–fixed springs (Eq. 3) |
| $Q\vec x = \vec b$ | "set every partial derivative to zero": the equations solved each iteration |
| SPD | symmetric positive definite: the cost is a bowl with exactly one bottom |
| diagonally dominant | each diagonal entry ≥ sum of the others in its row; surplus = fixed connections |
| residual $\vec r = \vec b - Q\vec x$ | how far from solved; the net force on each cell |
| CG | conjugate gradient: downhill steps that don't undo each other |
| κ | condition number: how stretched the bowl is; CG needs about $\sqrt\kappa$ steps |
| Jacobi preconditioner | divide each row by its diagonal entry before solving |
| B2B | Bound2Bound net model: edges to the two extreme pins, weight $\frac{2}{p-1}\frac{1}{\ell}$ (Eq. 5) |
| lower bound | linear-system solution; ignores overlap, optimistic wirelength |
| upper bound | look-ahead legalized placement; nearly legal, pessimistic wirelength |
| γ | target density; a bin is overfilled if cell area / free area > γ |
| $C_c$, $C_B$ | cutline halving the cell area; cutline halving the free space |
| nonlinear scaling | pack cells, in order, into stripes; scale linearly inside each stripe |
| anchor, pseudonet | a cell's upper-bound location and the imaginary spring to it |
| α | anchor strength, $0.01(1 + \text{iteration})$; a Lagrange multiplier |
| gap | HPWL(upper) − HPWL(lower); convergence is judged on it |

**SimPL in ten lines**

```
place cells uniformly
repeat (5-7 times, until HPWL stops improving):          # initial placement
    build B2B graphs at the current placement
    solve Qx = b and Qy = b_y with Jacobi-preconditioned CG
for iteration = 0, 1, 2, ...:                             # global placement
    upper = look_ahead_legalize(lower)                    # Algorithm 1
    if gap small enough relative to iteration 10: stop
    alpha = 0.01 * (1 + iteration)
    build B2B graphs at lower, plus anchor springs alpha/|x - upper| to upper
    lower = CG solve
return upper  → final legalization and detailed placement
```

## Abacus

| Symbol / term | Meaning |
| --- | --- |
| legalization | put every cell in a row, without overlap, moving cells as little as possible |
| movement | distance between a cell's global position $(x'_i, y'_i)$ and its legal position $(x_i, y_i)$ |
| Tetris | greedy legalizer: sort by x, put each cell in the nearest free site, never move it again |
| subrow | piece of a row not blocked by a macro |
| PlaceRow | re-place all cells of one row, in order, for minimum $\sum e_i (x_i - x'_i)^2$ |
| $e_i$ | cell weight (1, area or number of pins) |
| abut | two cells touch, with no gap: $x_i = x_{i-1} + w_{i-1}$ |
| cluster | run of abutting cells; rigid, placed at $q_c / e_c$ clamped to the row |
| $e_c, q_c, w_c$ | a cluster's total weight, weighted wishes $\sum e_i(x'_i - \text{width to its left})$, total width |
| AddCell, AddCluster, Collapse | append a cell / a cluster to a cluster; place a cluster and merge it leftwards while it overlaps |

**Abacus in a few lines**

```
sort cells by x
for each cell i:
    for rows r outwards from the nearest, while vertical distance alone < best cost:
        append i to r; PlaceRow(r) as a trial; cost = movement of i
    append i to the cheapest row; PlaceRow it for real
PlaceRow: for each cell left to right:
    if it overlaps the last cluster: AddCell, then Collapse (merge leftwards while overlapping)
    else: start a new cluster at x'_i
```

## FastDP

| Symbol / term | Meaning |
| --- | --- |
| detailed placement | turn a legal placement into another legal one with shorter wires |
| segment | a row, or a piece of a row between macros and blockages, where standard cells go |
| optimal region | where one cell's nets are shortest with all else fixed: between the medians of its nets' box edges, in x and in y |
| Global Swap | swap a cell with a cell or a gap in its optimal region |
| $P_1$, $P_2$ | overlap penalties: shift the two neighbours ($wt_1$), shift more cells ($wt_2 \gg wt_1$) |
| benefit $b$ | $(W_1 - W_2) - P_1 - P_2$; swap if positive |
| Vertical Swap | swap a cell with a nearby cell one row towards its optimal region |
| Local Re-ordering | try all $3! = 6$ orders of 3 consecutive cells |
| Single-Segment Clustering | place a segment's cells, order fixed, for minimum HPWL |
| bounds | box edges of a cluster's nets: where its piecewise-linear wirelength changes slope |
| Optimal Region Center | midpoint of the slope-0 piece; where a cluster is placed |

**FastDP in a few lines**

```
Single-Segment Clustering
repeat:
    Global Swap      # each cell to a cell or gap in its optimal region, if b > 0
    Vertical Swap    # one row towards the optimal region
    Local Re-ordering  # best of 6 orders of every 3 neighbours
until the gain is small
repeat Single-Segment Clustering until the gain is small
```
