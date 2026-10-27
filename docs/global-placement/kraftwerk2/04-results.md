[← 3. Quality control and convergence](03-quality-control.md) · [Kraftwerk2 overview](README.md)

# 4. Final placement, results and complexity

*In the paper: §III-A, §VI, §XI, Tables I–IV and Figure 13.*

### 4.1 After global placement (§VI)

Global placement stops with about 20% overlap left. Two more steps finish the job:

1. **Legalization.** Standard cells are legalized with a method similar to
   [Tetris](../../references.md): cells are taken in order of x and each is put in
   the nearest free place, never to move again. Macros use a separate legalizer.
   Legalizing a placement with 20% overlap takes about 5% of the global placement's
   time and lengthens the wires by about 1%.
2. **Detailed placement.** A simple greedy pass rotates single modules and swaps
   pairs of neighbours when that helps, which shortens the wires by about 2%.

Most of Kraftwerk2's time goes into global placement, and within it into solving the
linear systems (30).

### 4.2 Bound2Bound against clique and star (§III-A, Table I)

| | two-pin connections | extra variables | HPWL | CPU time |
| --- | --- | --- | --- | --- |
| clique | $p(p-1)/2$ per net | none | | |
| star | $p$ per net | one per net | | |
| hybrid clique/star, GordianL weights | | | +7% | +17% |
| hybrid clique/star, Eisenmann weights | | | +8% | +10% |
| **Bound2Bound** | $2p - 3$ per net | none | best | best |

On a typical circuit, where most nets have two or three pins, Bound2Bound uses about
75% fewer two-pin connections than the clique model. The percentages are relative to
Bound2Bound on legal placements of the ISPD 2005 suite, all placed by Kraftwerk2:
Bound2Bound wins on wirelength because it models HPWL exactly, and on time because it
adds no star variables.

### 4.3 Benchmark results (§XI)

All runs on one core of a 2.2 GHz AMD Opteron, using under 4 GB. Other placers' times
are scaled to that machine by their SPEC CPU2000 ratio.

| suite | what it is | Kraftwerk2's standing |
| --- | --- | --- |
| ISPD 2005 (Table II) | 8 circuits, up to 2.2 M movable modules; HPWL | as good as FastPlace3 and 2× faster; 2.2% behind NTUPlace3 and 5.4% behind RQL, but more than 3× faster than either; 3.5% behind APlace2 and about 40× faster; 2.7–30% better than the remaining placers |
| ISPD 2006 (Table III) | 8 circuits, up to 2.5 M modules; HPWL, density overflow and CPU combined | best on the full score (HPWL + overflow + CPU; NTUPlace3 second, 3.9% higher); overflow factor 1.87%; CPU factor −9.35%, more than 4× faster than the median placer; fourth on HPWL + overflow alone |
| ICCAD 2004 mixed-size (Table IV a) | 18 circuits, up to 200 k modules, about 400 big movable modules each | the fastest, from 3.5× faster than NTUPlace3 to 24× faster than APlace2; 1.0% better than mPL5 and 5.3% better than FDP; 0.5% and 1.8% behind APlace2 and NTUPlace3 |
| IBM-HB+ floorplacement (Table IV b) | 17 circuits of about a thousand modules of very different sizes | 12% shorter than SCAMPI (Capo) and about 8× faster |

The results are taken at a "good trade-off" setting of the target movement $\mu_T$
([§3.3](03-quality-control.md#33-steering-the-movement-32)). Moving the setting one
way improves the wirelength by about 0.5% for twice the time; the other way halves
the time for about 2% more wire.

### 4.4 How the runtime grows (Figure 13)

Plotting CPU time against the number of movable modules $M$ over the ISPD 2005 and
2006 circuits gives an average-case growth of about $\Theta(M^{1.18})$: close to linear.
That fits the pieces: every iteration builds $O(\text{pins})$ springs, runs
conjugate gradient on a sparse matrix, and solves Poisson's equation on a grid;
the number of iterations, about 25 for adaptec1 (Figure 12), grows only slowly with
the circuit.

> [!IMPORTANT]
> **Key takeaways**
>
> - After global placement: a Tetris-like legalizer (+1% wire) and a greedy detailed placer (−2%).
> - Bound2Bound beats hybrid clique/star models on both wirelength and runtime.
> - Kraftwerk2 trades a few percent of wirelength against the best placers of its time for 2–40× less runtime.
> - Runtime grows about as $M^{1.18}$.

### Exercises

**1.** A net has 10 pins. How many two-pin connections do the clique, the star and Bound2Bound use?

<details class="success" markdown>
<summary>Answer</summary>

Clique: $10 \cdot 9/2 = 45$. Star: 10, plus one extra variable for the star node. Bound2Bound: $2 \cdot 10 - 3 = 17$, and no extra variable.

</details>

**2.** If one circuit takes 100 s, about how long does a circuit with 4× as many modules take at $\Theta(M^{1.18})$?

<details class="success" markdown>
<summary>Answer</summary>

$4^{1.18} \approx 5.1$, so about 510 s: a little worse than linear (400 s).

</details>

---

[← 3. Quality control and convergence](03-quality-control.md) · [Kraftwerk2 overview](README.md)
