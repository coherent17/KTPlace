[← Kraftwerk2 overview](README.md) · [2. Density, potential and target points →](02-density-and-potential.md)

# 1. Three forces and one equation

*In the paper: §IV and §V-B to §V-D, equations (15)–(30).*

### 1.1 The net force

Start from [quadratic placement](../../foundations/03-quadratic-placement.md). The
wire cost in x is

$$
\Gamma_x = \tfrac12\,\vec x^TC_x\vec x + \vec x^T\vec d_x + \text{const} \qquad \text{(15)}
$$

$C_x$ is the paper's name for the spring table, built from Bound2Bound weights
([Foundations §5.3](../../foundations/05-net-models.md#53-many-pin-nets-connect-to-the-bounds)),
and $\vec d_x$ holds the springs to fixed cells. The derivative of an energy is a
force, so the springs pull on the cells with

$$
F^\text{net}_x = C_x\vec x + \vec d_x \qquad \text{(17)}
$$

Setting it to zero is quadratic placement: every cell where its wire pulls cancel,
everyone in one clump ([Foundations §3.8](../../foundations/03-quadratic-placement.md#38-the-catch-everything-collapses)).
Kraftwerk2 adds two more forces.

### 1.2 The hold force: stay where you are

An iteration starts with the cells at $\vec x'$. If only the wires acted, the cells
would snap back into the clump. So Kraftwerk2 adds a **hold force** that is exactly
the opposite of the wire force *at the starting positions*:

$$
F^\text{hold}_x = -(C_x\vec x' + \vec d_x) \qquad \text{(29)}
$$

It is a constant: it does not change as the cells move. With the wires and the hold
force alone, the total force at the start is zero, and it stays zero only there:
$F^\text{net} + F^\text{hold} = C_x(\vec x - \vec x') = 0$ means $\vec x = \vec x'$.
**The two together keep the placement where it is.** Anything that moves the cells
now has to come from the third force.

The paper stresses a consequence: nothing has to be remembered from earlier
iterations. Older force-directed placers (Eisenmann's Kraftwerk, FDP) *added up*
spreading forces over the iterations; Kraftwerk2 starts each iteration fresh.

### 1.3 The move force: a spring to a target point

Each cell $i$ gets a **target point** $\mathring x_i$ (chapter 2 computes it from the
density) and a spring of stiffness $\mathring w_i$ toward it:

$$
F^\text{move}_{x,i} = \mathring w_i\,(x_i - \mathring x_i) \qquad \text{(27)}
$$

The targets are $\mathring{\vec x} = \vec x' - \vec\Phi_x$, the starting positions
minus the slope of the potential at each cell. Collect the stiffnesses into the
diagonal table $\mathring C_x = \text{diag}(\mathring w_i)$:

$$
F^\text{move}_x = \mathring C_x(\vec x - \mathring{\vec x}) \qquad \text{(28)}
$$

Because the target is a fixed point, a cell can at most reach its target in one
iteration, never overshoot it. That limits how far anything moves per iteration,
which is what makes the method converge (chapter 3).

### 1.4 The core equation (30)

The cells settle where the three forces add up to zero. Write $\Delta\vec x = \vec x - \vec x'$:

$$
\underbrace{C_x\vec x + \vec d_x}_{\text{net}}
\underbrace{- C_x\vec x' - \vec d_x}_{\text{hold}}
+ \underbrace{\mathring C_x(\vec x - \vec x' + \vec\Phi_x)}_{\text{move}} = 0
$$

The $\vec d_x$ terms cancel, and $C_x\vec x - C_x\vec x' = C_x\Delta\vec x$:

$$
\boxed{(C_x + \mathring C_x)\,\Delta\vec x = -\mathring C_x\,\vec\Phi_x} \qquad \text{(30)}
$$

This is the whole engine of Kraftwerk2: one sparse linear system per axis per
iteration. The matrix is the wire springs plus a diagonal, so it is symmetric
positive definite and conjugate gradient solves it
([Foundations chapter 4](../../foundations/04-solving-linear-systems.md)). The fixed
cells have dropped out of the right-hand side: only the potential's slope drives the
motion.

### 1.5 One cell, by hand

Take one movable cell whose springs to fixed pins add up to a stiffness $c = 3$, so
$C_x = [3]$. The potential's slope at the cell is $\Phi_x = 8$ (the ground rises to
the right), so its target is 8 to the left. Equation (30) gives

$$
(3 + \mathring w)\,\Delta x = -8\,\mathring w \qquad\Longrightarrow\qquad \Delta x = -\frac{\mathring w}{3 + \mathring w}\cdot 8
$$

| move weight $\mathring w$ | 0 | 1 | 3 | 9 | very large |
| --- | ---: | ---: | ---: | ---: | ---: |
| $\Delta x$ | 0 | −2 | −4 | −6 | −8 |

With no move force the cell stays put (the hold force at work). A stiff move spring
takes it all the way to the target; a soft one only part of the way, because its
wires hold it back. The fraction $\beta = \mathring w / (c + \mathring w)$ is how
the paper explains convergence in chapter 3, and $\mathring w$ is the knob that
quality control turns.

### 1.6 One iteration, step by step (paper Figure 6)

1. Start from the current placement $\vec x'$.
2. Compute the hold force (implicitly: it is what makes the fixed cells drop out of (30)).
3. Build the density $D$ and solve Poisson's equation for $\Phi$ (chapter 2).
4. Place each target point at $\vec x' - \nabla\Phi$.
5. Rebuild the Bound2Bound springs at $\vec x'$; solve (30) for x and for y.
6. Move every cell by $\Delta$: the result is less crowded and still wire-aware.

> [!IMPORTANT]
> **Key takeaways**
>
> - Net force: the wires' pull, $C\vec x + \vec d$. Hold force: its opposite at the start, a constant.
> - Move force: a spring of stiffness $\mathring w_i$ from each cell to a target point $x'_i - \partial\Phi/\partial x$.
> - Their sum is zero at $(C + \mathring C)\Delta\vec x = -\mathring C\,\vec\Phi_x$: one SPD system per axis per iteration.
> - A cell moves a fraction $\mathring w/(c + \mathring w)$ of the way to its target.

### Exercises

**1.** In §1.5, which $\mathring w$ moves the cell exactly halfway to its target?

<details class="success" markdown>
<summary>Answer</summary>

$\mathring w/(3 + \mathring w) = \tfrac12$ gives $\mathring w = 3$: the move spring as stiff as the wires. $\Delta x = -4$.

</details>

**2.** Why do the springs to fixed cells ($\vec d_x$) not appear in equation (30)?

<details class="success" markdown>
<summary>Answer</summary>

They appear in the net force and, with the opposite sign, in the hold force, because the hold force is minus the net force at the start. They cancel. Fixed cells still matter through $C_x$: each spring to a fixed cell adds to the diagonal, and so to how strongly the cell resists moving.

</details>

---

[← Kraftwerk2 overview](README.md) · [2. Density, potential and target points →](02-density-and-potential.md)
