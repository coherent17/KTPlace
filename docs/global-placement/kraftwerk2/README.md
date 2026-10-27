[Documentation home](../../README.md)

# Kraftwerk2

A guided reading of **"Kraftwerk2 – A Fast Force-Directed Quadratic Placement
Approach Using an Accurate Net Model"**, Peter Spindler, Ulf Schlichtmann and Frank
M. Johannes, *IEEE Transactions on Computer-Aided Design* 27(8), August 2008. See
[references](../../references.md) for the link.

**In one paragraph.** Kraftwerk2 is a force-directed quadratic placer. Like SimPL it
measures wire with the Bound2Bound net model and solves one sparse linear system per
axis per iteration. Unlike SimPL it never legalizes along the way. Instead it adds
two forces to the wire springs. A **hold force** cancels the wires' pull at the
start of each iteration, so the cells would stay exactly where they are. A **move
force** pulls each cell toward a *target point* downhill on a potential landscape
computed from the cell density by Poisson's equation. One linear system then moves
every cell a controlled distance out of the crowded regions, and the iterations stop
when less than 20% of the cell area overlaps.

**Where it sits in the flow.** Kraftwerk2 is a global placer. The paper finishes
its output with a Tetris-like legalizer and a greedy detailed placer
([chapter 4](04-results.md)).

## Read first

[Foundations](../../foundations/README.md) chapters 3, 4 and 5 (quadratic
placement, conjugate gradient, Bound2Bound), and chapter 6, which introduces the
density, the potential and Poisson's equation that this guide relies on.

## Chapters

| # | Chapter | What you learn | Paper |
| --- | --- | --- | --- |
| 1 | [Three forces and one equation](01-forces.md) | net, hold and move force; the core system $(C + \mathring C)\Delta x = -\mathring C\,\Phi_x$ | §IV, §V-B to V-D, eq. (15)–(30) |
| 2 | [Density, potential and target points](02-density-and-potential.md) | supply and demand, Poisson's equation, large modules, the target density | §V-A, §VIII, §IX, eq. (19)–(26), (33)–(34) |
| 3 | [Quality control and convergence](03-quality-control.md) | the move-force weights, the movement target $\mu_T$, the overlap $\Omega$, why it converges, the whole loop | §VI, §VII, §X, eq. (31)–(40) |
| 4 | [Final placement, results and complexity](04-results.md) | legalization afterwards, Bound2Bound against clique and star, the benchmark results, runtime growth | §III-A, §VI, §XI, Tables I–IV |

```mermaid
flowchart LR
    f["Foundations 3, 4, 5, 6"]:::found --> k1["1 · Three forces"]:::simpl
    k1 --> k2["2 · Density and potential"]:::simpl
    k2 --> k3["3 · Quality control"]:::simpl
    k3 --> k4["4 · Results"]:::simpl
    classDef io fill:#fef3c7,stroke:#d97706,color:#451a03
    classDef stage fill:#ccfbf1,stroke:#0f766e,color:#042f2e
    classDef found fill:#e0e7ff,stroke:#4f46e5,color:#1e1b4b
    classDef simpl fill:#fce7f3,stroke:#db2777,color:#500724
    classDef mov fill:#dbeafe,stroke:#2563eb,color:#172554
    classDef fixed fill:#e5e7eb,stroke:#374151,color:#111827
```

**Kraftwerk2 in ten lines.**

1. Place every cell by minimizing the Bound2Bound wire cost alone, a few times over (initial placement).
2. Repeat until the overlap $\Omega$ is below 20%:
3. Rasterize the cells (demand) and the room (supply) into a density grid $D$.
4. Solve Poisson's equation $\nabla^2\Phi = -D$; each cell's target is its position minus $\nabla\Phi$.
5. Rebuild the Bound2Bound springs $C$ at the current positions.
6. Solve $(C + \mathring C)\Delta x = -\mathring C\,\partial_x\Phi$ and the same for $y$, by conjugate gradient.
7. Move every cell by $\Delta$.
8. Measure the average movement $\mu$ and scale the move weights by $\kappa = 1 + \tanh(\ln(\mu_T/\mu))$.
9. Measure the overlap $\Omega$.
10. Hand the result to legalization.
