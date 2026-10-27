[← 1. Three forces and one equation](01-forces.md) · [Kraftwerk2 overview](README.md) · [3. Quality control and convergence →](03-quality-control.md)

# 2. Density, potential and target points

*In the paper: §V-A, §VIII and §IX, equations (19)–(26) and (33)–(34).
Background: [Foundations chapter 6](../../foundations/06-density-and-potentials.md).*

### 2.1 The supply and demand system (19)–(24)

$$
D(x, y) = D^\text{dem}(x, y) - D^\text{sup}(x, y) \qquad \text{(19)}
$$

**Demand** (22)–(23). Every module $i$, movable *and* fixed, is a rectangle of height
$d_i$ over its area: $D^\text{dem}_i = d_i \cdot R(x, y)$, where $R$ is 1 inside the
module and 0 outside. Adding all modules gives the demand. With $d_i = 1$ its value
at a point is the number of modules covering that point.

**Supply** (24). The chip offers room at a constant density $d_\text{sup}$ over its
whole area. The two must balance (20), which fixes the supply density:

$$
d_\text{sup} = \frac{\sum_i d_i A_i}{A_\text{chip}}
$$

**Example.** A chip of area 1000 holds movable cells of total area 400 and one fixed
macro of area 200 ($d = 1$ everywhere). Then $d_\text{sup} = 600/1000 = 0.6$. Over
the macro, demand is 1 and supply 0.6, so $D = +0.4$: the macro region counts as
crowded and pushes cells away from it, and there is no special case for fixed
modules.

### 2.2 The potential and the target points (25)–(26)

$D$ is treated as electric charge, and Poisson's equation gives its potential:

$$
\left(\frac{\partial^2}{\partial x^2} + \frac{\partial^2}{\partial y^2}\right)\Phi(x, y) = -D(x, y) \qquad \text{(25)}
$$

The target point of module $i$ is its position minus the slope of $\Phi$ there:

$$
\mathring x_i = x'_i - \frac{\partial\Phi}{\partial x}\bigg|_{(x'_i, y'_i)} \qquad \text{(26)}
$$

and the same for $y$. [Foundations §6.3](../../foundations/06-density-and-potentials.md#63-poissons-equation-how-the-landscape-is-made)
works through why that sends cells out of crowds and into free space. The paper
solves (25) numerically on a grid with a multigrid solver (DiMEPACK); the cosine-wave
method of [Foundations §6.4](../../foundations/06-density-and-potentials.md#64-solving-it-on-a-grid-with-cosine-waves)
solves the same equation.

Two properties make this a good spreading force:

- **It is global.** A crowd far away still tilts the landscape here, so cells start
  moving toward distant free space early, not only after their neighbours have
  moved.
- **It is smooth.** The potential is a blurred version of the density (paper
  Figure 6(e)), so nearby cells get similar targets and the placement deforms
  smoothly instead of shuffling.

### 2.3 Large modules and halos (33)–(34)

With $d_i = 1$ for every module, the method converges to a placement where each
module sits in its own region $R_i$ that holds exactly as much supply as the module
has demand:

$$
A_{R,i} = \frac{d_i}{d_\text{sup}}\,A_i \qquad \text{(33)}
$$

With $d_\text{sup} = 0.5$ and $d_i = 1$, every module claims twice its own area. For
small cells that slack is useful, it is how they spread. For a big macro it leaves a
ring of empty space, a **halo**, that pushes the small cells away from it and makes
their wires longer (paper Figure 10). So big modules get a lower density:

$$
d_i = \begin{cases} 1 & A_i < A_\text{large} \\[4pt]
\sqrt{\dfrac{A_\text{large}}{A_i}}\,(1 - d_\text{sup}) + d_\text{sup} & \text{otherwise} \end{cases} \qquad \text{(34)}
$$

with $A_\text{large} = 50\,A_\text{avg}$. The density falls with the module's size
but never below $d_\text{sup}$; below that the method would not converge.

**Example.** $A_\text{avg} = 1$, so $A_\text{large} = 50$, and $d_\text{sup} = 0.5$.
A macro of area 200 gets $d = \sqrt{50/200}\,(0.5) + 0.5 = 0.75$, so by (33) it
claims $1.5\times$ its area instead of $2\times$. A macro of area 5000 gets
$d = 0.1 \cdot 0.5 + 0.5 = 0.55$, nearly no halo at all.

### 2.4 Controlling the density (§IX)

Spreading the supply over the whole chip spreads the cells over the whole chip, even
when the design only fills half of it, and that costs wire. The paper's alternative
packs the cells to a user-given **target density** $t_d$:

1. *Initial supply*: density $t_d$ at every point where there is demand now.
2. *Additional supply*: a frame of density $t_d$ and constant width around the
   initial supply, as wide as needed to balance the total demand.

The supply then follows the cells as they spread, and the cells end up packed at
about $t_d$ in a compact region instead of thinly over the whole chip (paper Figure
11). Because the potential is computed on a grid anyway, the paper builds both
supplies on that grid.

> [!IMPORTANT]
> **Key takeaways**
>
> - $D$ = module demand (movable and fixed) minus supply; the supply is balanced against the demand.
> - Poisson's equation turns $D$ into a potential $\Phi$; each module's target is its position minus $\nabla\Phi$.
> - Large modules get a lower density (34) so they leave no halo around them.
> - A target-density supply packs the cells to about $t_d$ instead of spreading them over the whole chip.

### Exercises

**1.** A chip of area 400 has movable cells of total area 100 and no fixed modules. What is $d_\text{sup}$, and how much area does each cell claim at convergence?

<details class="success" markdown>
<summary>Answer</summary>

$d_\text{sup} = 100/400 = 0.25$, so by (33) each cell claims $1/0.25 = 4$ times its area: the cells end up evenly spread with three quarters of the chip empty around them.

</details>

**2.** With $d_\text{sup} = 0.6$ and $A_\text{large} = 50$, what density does a movable macro of area 450 get?

<details class="success" markdown>
<summary>Answer</summary>

$\sqrt{50/450} = 1/3$, so $d = \tfrac13 \cdot 0.4 + 0.6 \approx 0.733$.

</details>

---

[← 1. Three forces and one equation](01-forces.md) · [Kraftwerk2 overview](README.md) · [3. Quality control and convergence →](03-quality-control.md)
