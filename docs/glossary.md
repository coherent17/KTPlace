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

**The algorithm in ten lines**

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
