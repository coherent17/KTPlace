[← 3. Anchors and convergence](03-anchors-and-convergence.md) · [SimPL contents](README.md)

# 4. Speed, results and parallelism

*In the paper: §4.4, §5, §6, Table 1.*

### 4.1 How fast is it

Notation: $n$ cells, $m$ nonzeros in $Q$, κ its condition number.

**CG.** One CG iteration costs $O(m)$ (one sparse matrix-vector product), and the
number of iterations grows like $\sqrt\kappa$, so one CG solve is
$O(m\sqrt\kappa)$. Practical netlists have a bounded average number of nets per cell,
$d \approx 5$, so each row has at most about $d+1$ nonzeros (counting the diagonal):
$m \le (d+1)n$, and CG is $O(n\sqrt\kappa)$. Thanks to preconditioning the authors
observe the number of CG iterations growing no faster than $\log n$, so in practice
CG costs about $O(n \log n)$.

**Look-ahead legalization.** The scaling itself is linear, $O(n)$; the dominant
cost is **sorting** cells by coordinate. Each level of partitioning sorts all
cells at most once in total, which is $O(n\log n)$. Region sizes halve each level, so
there are $O(\log n)$ levels. Total: $O(n\log^2 n)$.

**One iteration** $= O(n\log n)$ (CG) $+ O(n\log^2 n)$ (LAL) $= O(n\log^2 n)$, and
fewer than 50 iterations are needed even for million-cell designs.

### 4.2 Results

The whole SimPL program is under 5000 lines of C++ with only the standard library.
It was compared on the ISPD 2005 contest benchmarks (adaptec1–4, bigblue1–4: 211K to
2.18M cells) with target density $\gamma = 1.0$, all placers single-threaded, final
legalization and detailed placement by FastPlace-DP.

| | Capo 10.5 | NTUPlace3 | FastPlace 3.0 | mPL6 | SimPL |
| --- | ---: | ---: | ---: | ---: | ---: |
| HPWL (geometric mean, relative to SimPL) | 1.19× | 1.04× | 1.04× | 1.03× | 1.00× |
| Runtime (relative to SimPL) | 11.55× | 3.32× | 1.12× | 7.28× | 1.00× |

SimPL had the shortest wirelength *and* the shortest runtime: on average
16.26%, 4.12%, 4.23% and 2.57% less wire than Capo, NTUPlace3, FastPlace3 and mPL6.
A few percent is a lot in this field: the paper compares it with one generation of
chip-manufacturing scaling (~30% less wire, at a cost of billions).
The full suite took 1 hour 3 minutes; bigblue4 (2.18M cells) took 33 minutes and
2.1 GB.

**Where the time goes (bigblue4):** initial placement 5.0% (3.7% CG, 1.3% building
B2B graphs and matrices); global placement iterations 47.4% (19% CG, 9.9% B2B and
matrix construction, 17.7% look-ahead legalization); final legalization and detailed
placement 46.2%; I/O 1.4%.

### 4.3 Parallelism

- **B2B construction**: x and y are independent ([Foundations §3.3](../../foundations/03-quadratic-placement.md#33-x-and-y-separate)), so they are built on two
  threads; with more cores the nets are split into equal groups.
- **CG**: the matrix rows are split into blocks, one block per thread (OpenMP). The
  core operation is the sparse matrix-vector product (**SpMxV**), whose speed is
  limited by **memory bandwidth** (how fast numbers come from RAM), not arithmetic;
  the CSR layout reduces the data that must be read.
- **SIMD (SSE2)**: one instruction operates on 4 floats at once. Listing 1 is an inner
  product: each thread accumulates 4-wide partial sums of its part of the vectors,
  the per-thread partial sums are added together at the end, and the leftover
  elements are added one by one. SIMD helped the inner products but barely helped
  SpMxV, because SpMxV reads memory at irregular addresses, which cannot be loaded
  4-at-a-time.
- **Look-ahead legalization**: after CG was parallel, LAL became the bottleneck.
  Top-down partitioning naturally creates independent sub-regions. Each thread has
  a private queue $Q_i$ and there is a global queue $Q_g$. After splitting a region,
  a thread keeps one half in $Q_i$ and puts the other half in $Q_g$; when $Q_i$ is
  empty it takes $N = \max(Q_g.\text{size}/\text{threads}, 1)$ regions from $Q_g$. This
  keeps all threads busy (load balancing).

**Speed-ups** on 8 cores: global placement 2.4× faster (multi-threading plus SSE was
1.6× faster than multi-threading alone); solution quality unchanged; peak memory up
1.91×. Gains saturate above 4 threads because LAL scales poorly. With 8 threads, CG
was still the biggest part (36% of global placement) and LAL a close second (>31%).

> [!IMPORTANT]
> **Key takeaways**
>
> - One iteration costs $O(n \log^2 n)$, and fewer than 50 iterations are needed.
> - On ISPD 2005, SimPL had shorter wires and shorter runtimes than Capo, NTUplace3, FastPlace3 and mPL6.
> - CG and Bound2Bound construction parallelize well; look-ahead legalization limits the speed-up beyond four threads.

---

[← 3. Anchors and convergence](03-anchors-and-convergence.md) · [SimPL contents](README.md)
