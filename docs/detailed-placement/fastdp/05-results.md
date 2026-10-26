[← 4. Single-Segment Clustering](04-single-segment-clustering.md) · [FastDP contents](README.md)

# 5. Results

*In the paper: §IV, Tables III–VI.*

### 5.1 The setup

FastDP was compared with three detailed placers of the time:

- **Fengshui 5.0** post-processing, with the default re-ordering control string of the
  Fengshui flow;
- **rowIroning** from the Capo 9.1 package, with the options of the Capo flow;
- **Domino**, a network-flow based detailed placer, considered very good but slow.

Each was run on two kinds of starting placements:

- **mPL5 + legalizer:** global placements from mPL5 (its own legalizer and detailed placer
  switched off), legalized with Capo 9.1's placement utility. These have many local
  mistakes left.
- **Capo 9.1:** Capo's legalized global placements, with its greedy swapping and
  rowIroning switched off. Capo already fixes many local errors while partitioning down to
  small regions, so there is less left to gain.

Benchmarks: the ISPD 2004 IBM standard-cell suite (ibm01–ibm18) and the ISPD 2005 suite
(adaptec1–4, bigblue1–4, with fixed and movable macros and up to 2 million cells). All runs
on one 3.0 GHz Pentium 4 with 2 GB of memory.

### 5.2 IBM benchmarks

| Starting placement | | Fengshui 5.0 | rowIroning | Domino |
| --- | --- | ---: | ---: | ---: |
| mPL5 + legalizer (Table III) | FastDP's extra HPWL reduction | 19.05% | 13.22% | 0.54% |
| | FastDP faster by | 3.62× | 2.79× | ~15× |
| Capo 9.1 (Table IV) | FastDP's extra HPWL reduction | 1.17% | 1.91% | 0.55% |
| | FastDP faster by | 4.48× | 5.66× | 13.45× |

On the mPL5 placements FastDP shortened the legalized wirelength by **30.84% on average**
(ibm01: from 2.423 to 1.732 × 10⁶): the legalizer had disturbed mPL5's placements a lot,
and there was much to repair. On Capo's placements the reduction was 3.02%.

Fengshui failed on ibm08 and Domino on ibm17; their averages leave those circuits out.

### 5.3 ISPD 2005 benchmarks

Domino could not produce feasible solutions on this suite, and Capo ran out of memory on
bigblue4, so the comparison is with Fengshui and rowIroning.

| Starting placement | | Fengshui 5.0 | rowIroning |
| --- | --- | ---: | ---: |
| mPL5 + legalizer (Table V) | FastDP's extra HPWL reduction | 8.06% | 9.12% |
| | FastDP faster by | 3.05× | 2.29× |
| Capo 9.1 (Table VI) | FastDP's extra HPWL reduction | 2.04% | 0.89% |
| | FastDP faster by | 2.81× | 2.18× |

FastDP's own reduction of the legalized wirelength: 14.46% on average from mPL5 placements
(adaptec1: 0.925 → 0.864 × 10⁸, 96 s; bigblue4: 10.535 → 9.230 × 10⁸, 2273 s), 1.86% from
Capo placements.

The speed-up is smaller here than on the IBM suite because FastDP runs as many iterations
as its stopping rule asks for, while Fengshui and rowIroning run a fixed number of passes.

### 5.4 What the numbers say

- **Better and faster than all three.** On average, on both suites and from both starting
  placements, FastDP gives the shortest wirelength, in less time.
- **Domino's quality at a fraction of its time.** Domino comes within about half a percent,
  but takes 13 to 15 times longer.
- **A bad start can be repaired.** mPL5's legalized placements start much worse than
  Capo's, yet after FastDP they end up better: the errors legalization introduced are
  exactly the local mistakes FastDP is built to fix.

> [!IMPORTANT]
> **Key takeaways**
>
> - On IBM placements from mPL5, FastDP cuts wirelength by about 31% and beats Fengshui, rowIroning and Domino by 19%, 13% and 0.5%, being 3.6×, 2.8× and 15× faster.
> - On ISPD 2005 it beats Fengshui and rowIroning by about 8–9% (mPL5 starts) and 1–2% (Capo starts), 2–3× faster.
> - The gain is largest when legalization has disturbed a good global placement.

### Exercises

**1.** Why does FastDP gain much less on Capo's placements than on mPL5's?

<details class="success" markdown>
<summary>Answer</summary>

Capo already fixes many local errors while partitioning down to small regions, and its
placements were legalized inside Capo. mPL5's were legalized by a separate legalizer that
disturbed them a lot, leaving many local mistakes for detailed placement to fix.

</details>

---

[← 4. Single-Segment Clustering](04-single-segment-clustering.md) · [FastDP contents](README.md)
