[← 2. The math toolkit, gently](02-math-toolkit.md) · [Foundations contents](README.md) · [4. Solving a million equations →](04-solving-linear-systems.md)

# 3. Quadratic placement

*In the SimPL paper: §2.*

### 3.1 The spring picture

Imagine every wire as a **coil spring**. A spring stretched to length $d$ stores
energy proportional to $d^2$ (Hooke's law). Let the springs pull the cells and wait
until everything stops moving: that resting state is the one with minimal total
spring energy, and every cell sits where the pulls on it cancel (**force
equilibrium**). Fixed cells are nails in the board.

### 3.2 The quadratic objective (Equation 2)

Turn the netlist into an ordinary graph $G = (E_G, V)$: each edge $e_{ij}$ joins
two cells $i, j$ and has a weight $w_{ij} > 0$ (spring stiffness). How a net with
many pins becomes edges is the topic of [Chapter 5](05-net-models.md). The total spring energy is

$$
\Phi_G(\vec x, \vec y) = \sum_{e_{ij} \in E_G} w_{ij}\Big[(x_i - x_j)^2 + (y_i - y_j)^2\Big] \qquad \text{(Eq. 2)}
$$

Why squares rather than plain distances? Squares are smooth (no corners), their
derivatives are straight lines, and so "set the derivative to zero" gives
**linear** equations, the easiest kind of equations to solve.

### 3.3 x and y separate

Expand Eq. 2:

$$
\Phi_G = \underbrace{\sum w_{ij}(x_i - x_j)^2}_{\Phi_x \text{ (only x's)}} + \underbrace{\sum w_{ij}(y_i - y_j)^2}_{\Phi_y \text{ (only y's)}}
$$

No term mixes an $x$ with a $y$. So minimizing $\Phi_G$ is two independent
problems: minimize $\Phi_x$ over the x's, and minimize $\Phi_y$ over the y's. The
paper calls this *separable*; from now on we only treat $x$, and $y$ is identical.
(It also means the two can be solved at the same time on two CPU cores, [SimPL §4.3](../global-placement/simpl/04-speed-results-parallelism.md#43-parallelism).)

### 3.4 Matrix form (Equation 3)

The paper writes the x part as

$$
\Phi_x(\vec x) = \tfrac12\,\vec x^T Q_x \vec x + \vec c_x^{\,T}\vec x + \text{const} \qquad \text{(Eq. 3)}
$$

(and the same for y). Read it as: "a sum of squares and products of the
movable coordinates (the $Q_x$ part), plus terms linear in each coordinate that
come from springs to fixed cells (the $\vec c_x$ part), plus a constant that does
not depend on where we put anything".

- $Q_x$ records **movable–movable** connections.
- $\vec c_x$ records **movable–fixed** connections.

Taking all partial derivatives of Eq. 3 gives the gradient $Q_x\vec x + \vec c_x$
(you can check this on the example below). Setting it to zero:

$$
Q_x \vec x = -\vec c_x
$$

To avoid the minus sign, this guide writes $\vec b = -\vec c_x$, so the system is
$Q\vec x = \vec b$. The rules for filling in the matrix (we derive them next) are:

| Entry | Value |
| --- | --- |
| $Q_{ii}$ (diagonal) | sum of the weights of **all** edges touching movable cell $i$ |
| $Q_{ij}$, $i \ne j$ (off-diagonal) | $-$(sum of the weights of edges between movable $i$ and movable $j$) |
| $b_i$ | sum over fixed cells $f$ connected to $i$ of $w_{if}\cdot x_f$ |

(The ½ in Eq. 3 is a convention: with it, the gradient comes out as exactly
$Q\vec x + \vec c$ with these entries. Multiplying a cost by any positive
constant never moves its minimum, so conventions like this do not change the
answer.)

### 3.5 The worked example (Figure 1 and Equation 4)

Figure 1 of the paper has four movable cells $x_1..x_4$ and two fixed cells at
$f_1 = 1.0$, $f_2 = 3.5$, with six edges whose weights are 1, 1, 1, 2, 3 and 4.
The figure is a drawing and its exact wiring is hard to read from the text; the
graph below uses exactly those weights and reproduces the paper's stated answer
exactly, so it is the right system to learn from:

```mermaid
flowchart LR
    f1(["f1 = 1.0 (fixed)"]):::fixed ---|3| x1((x1)):::mov
    x1 ---|1| x2((x2)):::mov
    x1 ---|1| x3((x3)):::mov
    x2 ---|1| x3
    x3 ---|2| x4((x4)):::mov
    x4 ---|4| f2(["f2 = 3.5 (fixed)"]):::fixed
    classDef io fill:#fef3c7,stroke:#d97706,color:#451a03
    classDef stage fill:#ccfbf1,stroke:#0f766e,color:#042f2e
    classDef found fill:#e0e7ff,stroke:#4f46e5,color:#1e1b4b
    classDef simpl fill:#fce7f3,stroke:#db2777,color:#500724
    classDef mov fill:#dbeafe,stroke:#2563eb,color:#172554
    classDef fixed fill:#e5e7eb,stroke:#374151,color:#111827
```

Edges: $f_1$–$x_1$ (3), $x_1$–$x_2$ (1), $x_1$–$x_3$ (1), $x_2$–$x_3$ (1),
$x_3$–$x_4$ (2), $x_4$–$f_2$ (4).

**Step 1: write the cost.** One term per edge (Eq. 2, x part):

$$
\Phi = 3(x_1 - 1)^2 + (x_1 - x_2)^2 + (x_1 - x_3)^2 + (x_2 - x_3)^2 + 2(x_3 - x_4)^2 + 4(x_4 - 3.5)^2
$$

**Step 2: take each partial derivative and set it to zero.** Use the rule
$\frac{d}{dx}w(x-c)^2 = 2w(x-c)$. For $x_1$, only terms containing $x_1$ matter:

$$
\frac{\partial\Phi}{\partial x_1} = 2\cdot3(x_1 - 1) + 2(x_1 - x_2) + 2(x_1 - x_3) = 0
$$

Divide by 2 and collect: $3x_1 - 3 + x_1 - x_2 + x_1 - x_3 = 0$, i.e.

$$
5x_1 - x_2 - x_3 = 3
$$

For $x_2$ (it appears as the *second* variable in $(x_1-x_2)^2$, whose derivative
with respect to $x_2$ is $-2(x_1 - x_2)$):

$$
\frac{\partial\Phi}{\partial x_2} = -2(x_1 - x_2) + 2(x_2 - x_3) = 0 \;\Rightarrow\; -x_1 + 2x_2 - x_3 = 0
$$

For $x_3$:

$$
\frac{\partial\Phi}{\partial x_3} = -2(x_1 - x_3) - 2(x_2 - x_3) + 2\cdot2(x_3 - x_4) = 0 \;\Rightarrow\; -x_1 - x_2 + 4x_3 - 2x_4 = 0
$$

For $x_4$:

$$
\frac{\partial\Phi}{\partial x_4} = -2\cdot2(x_3 - x_4) + 2\cdot4(x_4 - 3.5) = 0 \;\Rightarrow\; -2x_3 + 6x_4 = 14
$$

**Step 3: these are Eq. 4, the system from [§2.3](02-math-toolkit.md#23-matrices-a-table-that-turns-a-list-into-another-list):**

$$
\begin{bmatrix} 5 & -1 & -1 & 0\\ -1 & 2 & -1 & 0\\ -1 & -1 & 4 & -2\\ 0 & 0 & -2 & 6 \end{bmatrix}
\begin{bmatrix} x_1\\ x_2\\ x_3\\ x_4 \end{bmatrix}
=
\begin{bmatrix} 3\\ 0\\ 0\\ 14 \end{bmatrix}
\qquad \text{(Eq. 4)}
$$

Check the rules of [§3.4](#34-matrix-form-equation-3) against it:

- $Q_{11} = 5 = 3 + 1 + 1$, the sum of the weights at $x_1$. $Q_{44} = 6 = 2 + 4$.
- $Q_{34} = Q_{43} = -2$, minus the $x_3$–$x_4$ weight. The matrix is symmetric
  because the spring between 3 and 4 is the same spring seen from either end.
- $b_1 = 3 \cdot f_1 = 3\cdot1.0 = 3$; $b_4 = 4 \cdot f_2 = 4\cdot 3.5 = 14$;
  $x_2, x_3$ touch no fixed cell, so $b_2 = b_3 = 0$.

**Step 4: solve by hand** (substitution, which is fine for 4 unknowns):

- Row 2: $x_2 = (x_1 + x_3)/2$.
- Row 4: $x_4 = (14 + 2x_3)/6 = (7 + x_3)/3$.
- Row 1 with $x_2$ substituted: $5x_1 - \frac{x_1 + x_3}{2} - x_3 = 3$. Multiply by
  2: $9x_1 - 3x_3 = 6$, so $x_1 = (2 + x_3)/3$.
- Row 3 with $x_2$ and $x_4$ substituted, multiplied by 6:
  $-9x_1 + 17x_3 = 28$. Put in $x_1$: $-3(2 + x_3) + 17x_3 = 28$, so $14x_3 = 34$.

$$
x_3 = \tfrac{17}{7} \approx 2.4286,\quad x_1 = \tfrac{31}{21} \approx 1.4762,\quad x_2 = \tfrac{41}{21} \approx 1.9524,\quad x_4 = \tfrac{22}{7} \approx 3.1429
$$

which is the paper's $\vec x = [1.4762\ \ 1.9524\ \ 2.4286\ \ 3.1429]^T$.

### 3.6 Every cell sits at the weighted average of its neighbours

Rearrange row 2: $x_2 = \frac{1\cdot x_1 + 1\cdot x_3}{1 + 1}$. Rearrange row 1:
$x_1 = \frac{3\cdot f_1 + 1\cdot x_2 + 1\cdot x_3}{3 + 1 + 1}$. In general, setting
$\partial\Phi/\partial x_i = 0$ gives

$$
x_i = \frac{\sum_j w_{ij}\,x_j}{\sum_j w_{ij}}
$$

> [!IMPORTANT]
> **Each cell sits at the weighted average of the cells it connects to**, over all
> its neighbours $j$, movable or fixed. This is force equilibrium: a stiffer spring pulls
> the average harder toward its end. Keep this picture; the Jacobi method ([§4.2](04-solving-linear-systems.md#42-the-simplest-idea-jacobi-iteration)) and
> the anchors ([SimPL chapter 3](../global-placement/simpl/03-anchors-and-convergence.md)) are both easy to understand with it.

### 3.7 Why $Q$ is a strictly convex bowl (SPD), and diagonal dominance

Here is a useful identity. Expanding $\vec x^T Q \vec x$ for a placement matrix
regroups exactly into

$$
\vec x^T Q \vec x = \sum_{\text{movable–movable edges}} w_{ij}(x_i - x_j)^2 \;+\; \sum_{\text{movable–fixed edges}} w_{if}\,x_i^2
$$

(Try it on the example. The $x_1$–$x_2$ edge (weight 1) puts $+1$ into $Q_{11}$ and
$Q_{22}$ and $-1$ into $Q_{12}$ and $Q_{21}$. In $\sum_i\sum_j x_iQ_{ij}x_j$ those four
entries give $x_1^2 + x_2^2 - x_1x_2 - x_2x_1 = (x_1 - x_2)^2$. The $f_1$–$x_1$ edge
(weight 3) only puts $+3$ into $Q_{11}$, which gives $3x_1^2$.)

Every term is $\ge 0$, so $\vec x^T Q\vec x \ge 0$. It is $0$ only if every
connected pair has equal coordinates *and* every cell attached to a fixed cell is
at 0, which, as long as every connected group of cells touches at least one fixed
cell, forces $\vec x = 0$. So $Q$ is SPD and the solution is unique.

If some group of cells had **no** connection to anything fixed, the whole group
could slide left or right without changing any spring length: the bowl would have a
flat direction, $Q$ would be *degenerate* (singular), and there would be no unique
answer. That is the paper's "when $Q_x$ is nondegenerate".

**Diagonal dominance.** A matrix is *diagonally dominant* if in every row the
diagonal entry is at least the sum of the sizes of the other entries:
$Q_{ii} \ge \sum_{j\ne i}|Q_{ij}|$. In the example: $5 \ge 1+1$, $2 \ge 1+1$,
$4 \ge 1+1+2$, $6 \ge 2$. It always holds for placement matrices, because the
diagonal is the sum of *all* weights at a cell, while the off-diagonal entries only
count the weights to *movable* cells. The surplus is exactly the weight to fixed
cells. The more surplus, the "rounder" the bowl and the faster the solvers of
[Chapter 4](04-solving-linear-systems.md) converge. SimPL deliberately adds surplus ([SimPL chapter 3](../global-placement/simpl/03-anchors-and-convergence.md)).

### 3.8 The catch: everything collapses

> [!WARNING]
> Because every cell goes to the average of its neighbours, and the only things
> holding the netlist open are the fixed pads around the border, the solution of
> $Q\vec x = \vec b$ pulls almost all cells into a dense clump near the middle of the
> chip. Wirelength is wonderful; overlap is enormous. Every quadratic placer must add
> something that **spreads** cells out.

The two classic questions (paper §3) are:

1. **Direction:** which way should each cell be pushed?
2. **Force modulation:** how hard? Too weak and nothing spreads, too strong and
   the wirelength is destroyed or the iterations oscillate.

SimPL's answer to both is look-ahead legalization plus anchors; the [SimPL guide](../global-placement/simpl/README.md) explains both.

> [!IMPORTANT]
> **Key takeaways**
>
> - Wires are springs: the cost is $\sum w_{ij}(x_i - x_j)^2$, solved separately in x and y.
> - Setting the derivatives to zero gives $Q\vec x = \vec b$: the diagonal holds each cell's total weight, off-diagonals the negative weights, $\vec b$ the pull of fixed cells.
> - Every cell ends up at the weighted average of its neighbours.
> - The answer is unique when every group of cells touches something fixed, but the cells collapse into a clump, so something must spread them.

---

[← 2. The math toolkit, gently](02-math-toolkit.md) · [Foundations contents](README.md) · [4. Solving a million equations →](04-solving-linear-systems.md)
