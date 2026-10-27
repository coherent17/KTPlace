[← 5. Net models](05-net-models.md) · [Foundations contents](README.md)

# 6. Density, potentials and Poisson's equation

*Background for [Kraftwerk2](../global-placement/kraftwerk2/README.md), §V-A of its paper.*

Quadratic placement pulls every cell toward its neighbours and so piles them up
([§3.8](03-quadratic-placement.md#38-the-catch-everything-collapses)). SimPL undoes
the pile with a legalizer-like spreading step. Force-directed placers such as
Kraftwerk2 do it with physics instead: they measure *where it is too crowded*, turn
that into a smooth landscape, and push every cell downhill. This chapter builds that
machinery from scratch: density on a grid, the idea of a potential, Poisson's
equation, and how a computer solves it.

### 6.1 Density: demand minus supply

Cut the chip into a grid of equal **bins**. In each bin, count how much cell area
lies there; divide by the bin's area. That is the **demand** density: 0 means empty,
1 means exactly full, 3 means three layers of cells on top of each other.

The chip also offers room. Spread the total cell area evenly over the chip and you
get the **supply** density $d_\text{sup}$: how full every bin would be if the cells
were perfectly spread. Kraftwerk2 works with the difference

$$
D = \text{demand} - \text{supply}
$$

- $D > 0$: too crowded, cells should leave.
- $D < 0$: room to spare, cells may come.
- $D = 0$ everywhere: perfectly spread, nothing to do.

Because the supply was made from the same total area as the demand, the positive
and negative parts of $D$ always cancel: adding $D$ over the whole chip gives 0.
The paper calls this a **balanced** supply and demand system.

**Example (1-D).** A line of length 6, cut into 6 bins of length 1. Four cells of
length 1 are piled into bins 2 and 3 (two layers each); the other bins are empty.

| bin | 0 | 1 | 2 | 3 | 4 | 5 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| demand | 0 | 0 | 2 | 2 | 0 | 0 |
| supply $= 4/6$ | 0.67 | 0.67 | 0.67 | 0.67 | 0.67 | 0.67 |
| $D$ | −0.67 | −0.67 | 1.33 | 1.33 | −0.67 | −0.67 |

The row $D$ adds up to 0, as promised.

### 6.2 A potential: turning crowding into a landscape

Knowing *where* it is crowded is not yet knowing *which way to push*. A cell in the
middle of a crowded bin should move outward, but which way is out? The trick is a
**potential** $\Phi(x, y)$: a smooth landscape that is high where it is crowded and
low where there is room. Then every cell simply rolls downhill.

Downhill is the negative slope ([§2.5](02-math-toolkit.md#25-derivatives-with-many-variables)).
On a line, a cell at $x$ is sent to

$$
\text{target} = x - \Phi'(x)
$$

If the ground rises to the right ($\Phi' > 0$), the target is to the left, and the
steeper the slope, the farther. In two dimensions the slope is the gradient
$\nabla\Phi = (\partial\Phi/\partial x,\ \partial\Phi/\partial y)$ and the target is
$(x, y) - \nabla\Phi$.

### 6.3 Poisson's equation: how the landscape is made

Which landscape? Physics has an answer that is two centuries old. Treat $D$ as
electric charge spread over the chip. The charge creates an electric potential
$\Phi$, and the two are linked by **Poisson's equation**:

$$
\frac{\partial^2\Phi}{\partial x^2} + \frac{\partial^2\Phi}{\partial y^2} = -D
\qquad\text{(in 1-D: } \Phi''(x) = -D(x)\text{)}
$$

The left side is the **curvature** of the landscape: how fast the slope changes.
So the equation reads: *where there is excess demand, the landscape bends downward
(a hilltop); where there is spare supply, it bends upward (a valley).* That is
exactly a landscape whose hills sit on the crowds.

**Solving the 1-D example by hand.** Take the line $[0, 6]$ with $D = +1$ on
$[2, 4]$ (a pile) and $D = -\tfrac12$ on $[0, 2]$ and $[4, 6]$ (room). It is
balanced: $2 \cdot 1 = 4 \cdot \tfrac12$. One more rule: at the two ends of the chip
the slope is 0, so nothing is pushed off the chip (a *boundary condition*). Since
$\Phi'' = -D$, the slope $\Phi'$ is $-D$ added up from the left end:

| $x$ | 0 | 1 | 2 | 2.5 | 3 | 3.5 | 4 | 5 | 6 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| $\Phi'(x)$ | 0 | 0.5 | 1 | 0.5 | 0 | −0.5 | −1 | −0.5 | 0 |
| target $x - \Phi'(x)$ | 0 | 0.5 | 1 | 2 | 3 | 4 | 5 | 5.5 | 6 |

On $[0, 2]$ the slope grows by $\tfrac12$ per unit (adding up $-D = +\tfrac12$), on
$[2, 4]$ it falls by 1 per unit, on $[4, 6]$ it grows again and returns to 0 at the
right end, exactly because the system is balanced. The targets show the effect: a
cell at 2.5 is sent to 2, a cell at 3.5 to 4, the centre of the pile stays, and the
cells outside move a little outward to make room. **The pile expands into the free
space**, which is what spreading means.

Try it: the cells below start in a pile. *Step* moves every cell part of the way to
its target, then rebuilds the density and the slope. The pile spreads, the slope
flattens, and the overlap falls.

<div class="kt-widget" data-widget="poisson"></div>

**In two dimensions** there are two curvatures, one per axis, and Poisson's
equation adds them. A single crowded spot then produces a round hill, and the
gradient points straight away from the spot in every direction, like the electric
field around a charge.

### 6.4 Solving it on a grid with cosine waves

A computer stores $D$ as one number per bin and needs $\Phi$ (really its gradient)
in each bin. The cleanest way uses one fact about cosine waves.

**One wave.** Let $D(x) = \cos(\omega x)$. Try $\Phi(x) = \cos(\omega x)/\omega^2$.
Differentiate twice: $\Phi' = -\sin(\omega x)/\omega$, $\Phi'' = -\cos(\omega x)$.
So $\Phi'' = -D$: solved. **A cosine wave of charge produces the same wave of
potential, divided by $\omega^2$.** Fast waves (large $\omega$, fine detail) give a
small potential; slow waves (large-scale crowding) give a large one. In two
dimensions, $\cos(\omega_x x)\cos(\omega_y y)$ is divided by
$\omega_x^2 + \omega_y^2$.

**Any pattern.** Every pattern of numbers on an $n \times n$ grid can be written as a
sum of $n^2$ such cosine waves, with frequencies $\omega_u = \pi u / L$ for
$u = 0, 1, \dots, n-1$ ($L$ the chip's width). Finding the amounts of each wave is
the **discrete cosine transform** (DCT). So the recipe is:

1. DCT: write $D$ as a sum of cosine waves (find each wave's amount $a_{uv}$).
2. Divide each amount by $\omega_u^2 + \omega_v^2$. The wave with $u = v = 0$ is the
   average of $D$, which is 0 for a balanced system, so it is skipped.
3. Add the waves back up, but differentiated: $\partial\Phi/\partial x$ turns each
   $\cos(\omega_u x)$ into $-\omega_u \sin(\omega_u x)$.

Every cosine has zero slope at the ends of its interval, so the result automatically
has zero slope at the chip boundary, the boundary condition from §6.3.

**Cost.** A DCT on an $n \times n$ grid can be done one row at a time, then one column
at a time: $2n$ one-dimensional transforms of $n$ numbers. Done directly, each costs
$n^2$, so a full 2-D transform costs $2n^3$. For $n = 256$ that is about 34 million
multiply-adds, a few milliseconds, and every row is independent, so all cores can
work at once.

> [!IMPORTANT]
> **Key takeaways**
>
> - Density $D$ = demand − supply, per bin; a balanced system adds up to zero.
> - A potential $\Phi$ is a landscape that is high where it is crowded; a cell's target is $x - \nabla\Phi$, downhill.
> - Poisson's equation $\nabla^2\Phi = -D$ makes that landscape: crowds become hilltops, free space valleys.
> - On a grid it is solved wave by wave: a cosine of charge gives the same cosine of potential divided by $\omega^2$.

### Exercises

**1.** A 1-D line $[0, 4]$ has $D = +1$ on $[0, 1]$ and $D = -\tfrac13$ on $[1, 4]$, with zero slope at both ends. Where does a cell at $x = 0.5$ go?

<details class="success" markdown>
<summary>Answer</summary>

$\Phi'(0.5) = -\int_0^{0.5} D = -0.5$. The target is $0.5 - (-0.5) = 1$: the cell moves right, out of the crowded first unit. (Check the right end: $\Phi'(4) = -1 + 3 \cdot \tfrac13 = 0$.)

</details>

**2.** Why must the supply be balanced against the demand?

<details class="success" markdown>
<summary>Answer</summary>

With zero slope at both ends, $\Phi'(L) - \Phi'(0) = -\int_0^L D$ must be 0. If $D$ did not add up to zero, no landscape could satisfy both the equation and the boundary condition. In the cosine recipe, the $u = v = 0$ wave would have to be divided by $0$.

</details>

**3.** $D(x) = \cos(\pi x / 10)$ on $[0, 10]$. What is the slope $\Phi'$ at $x = 5$?

<details class="success" markdown>
<summary>Answer</summary>

$\omega = \pi/10$, so $\Phi = \cos(\omega x)/\omega^2$ and $\Phi'(x) = -\sin(\omega x)/\omega$. At $x = 5$: $\Phi'(5) = -1/(\pi/10) = -10/\pi \approx -3.18$, so a cell there is sent 3.18 to the right, away from the crowded left half.

</details>

---

[← 5. Net models](05-net-models.md) · [Foundations contents](README.md)
