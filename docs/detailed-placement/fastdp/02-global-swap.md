[← 1. Detailed placement and the FastDP flow](01-the-flow.md) · [FastDP contents](README.md) · [3. Vertical Swap and Local Re-ordering →](03-vertical-swap-and-reordering.md)

# 2. The optimal region and Global Swap

*In the paper: §III-A, Figures 2–3, Table I, equations (1)–(2).*

**Goal:** for each cell, find where its wires would be shortest, and swap it with a cell
or a gap in that region, as long as the swap pays off.

### 2.1 The best position of one cell

Hold every other cell still and move only cell $i$. Which position gives the shortest
total HPWL of the nets connected to $i$? The x- and y-directions can be treated
separately, because HPWL is width plus height. Take the x-direction.

For each net $e$ connected to $i$, take the bounding box of its **other** pins, with
cell $i$ left out: its left edge $x_l[e]$ and right edge $x_r[e]$. Where cell $i$ goes
changes this net's width as follows:

- between $x_l[e]$ and $x_r[e]$: the width stays $x_r[e] - x_l[e]$, so the slope is 0;
- left of $x_l[e]$: the box grows as $i$ moves left, slope $-1$;
- right of $x_r[e]$: the box grows as $i$ moves right, slope $+1$.

So each net contributes a "bathtub" shape. Add them up and the total slope at a point
$x$ is

$$
\text{slope}(x) = \#\{\text{bounds smaller than } x\} - k,
$$

where $k$ is the number of nets and the **bounds** are all $2k$ values
$x_l[e], x_r[e]$ together. (For one net: below both of its bounds, it contributes
$0 - 1 = -1$; between them, $1 - 1 = 0$; above both, $2 - 1 = +1$.) The total is lowest
where the slope turns from negative to positive, which is where exactly $k$ of the $2k$
bounds lie to the left: **between the two middle values, the medians, of the sorted
list of bounds.** This median rule goes back to Goto (1981).

The same holds for y with the bottom and top edges $y_l[e]$, $y_u[e]$. Because there is an
even number of bounds, the two medians usually differ, and the best position is a whole
rectangle, not a point: the cell's **optimal region**. It shrinks to a line or a point
when the two medians are equal.

> [!TIP]
> **Example (x only).** Cell $i$ is on three nets. Without $i$, their boxes span
> $[2, 5]$, $[4, 9]$ and $[6, 7]$ in x. The bounds sorted: 2, 4, 5, 6, 7, 9. The two
> middle values are 5 and 6, so anywhere with $5 \le x \le 6$ is optimal in x. Check at
> $x = 5.5$: it is 0.5 right of the first box and 0.5 left of the third, and inside the
> second, so the nets grow by $0.5 + 0 + 0.5 = 1$ in total. At $x = 3$ they grow by
> $0 + 1 + 3 = 4$.

Paper Figure 2 shows the two-dimensional picture: cell 1 is on three nets; the bounding
boxes of those nets, without cell 1, draw a grid of x- and y-lines, and the optimal region
is the cell of that grid between the middle lines in both directions.

### 2.2 Why the optimal region alone is not enough

Detailed placement moves from one legal placement to another, so cell $i$ cannot simply
be dropped into its optimal region: something is already there. Global Swap exchanges $i$
with something in the region: either another cell $j$, which then takes $i$'s old place,
or a **space** (a gap between cells in a segment).

If the two cells have different widths, or the gap is narrower than $i$, the swap creates
overlap, and the cells next to the target have to be shifted aside to make room. Shifting
cells changes wirelength too, and the more cells have to shift, the more the placement is
disturbed. FastDP charges a **penalty** for that. And if the cells of a segment would no
longer fit in it at all, the swap is not considered.

### 2.3 The two penalties (Figure 3, equation (1))

Swap cell $i$ (width $w_i$) into the place of cell $j$ (width $w_j$) in segment
$\text{Seg}_j$, with $w_i > w_j$. The extra width that has to be found is $w_i - w_j$.
Around $j$:

- $j_1$ and $j_2$: the cells right next to $j$, left and right;
- $s_2$ and $s_3$: the free spaces between $j$ and $j_1$, and between $j$ and $j_2$;
- $s_1$ and $s_4$: the next free spaces further out, beyond $j_1$ and $j_2$;
- $S_1 = s_1 + s_2 + s_3 + s_4$.

If the two spaces next to $j$ are wide enough ($w_i - w_j \le s_2 + s_3$), the swap creates
no overlap and costs no penalty. Otherwise the neighbours $j_1$ and $j_2$ have to shift
outwards, and

$$
P_1 = \big((w_i - w_j) - (s_2 + s_3)\big)\cdot wt_1 .
$$

If even the four nearest spaces are not enough ($S_1 < w_i - w_j$), cells beyond $j_1$ and
$j_2$ have to move as well, and

$$
P_2 = \big((w_i - w_j) - S_1\big)\cdot wt_2 .
$$

Each penalty is proportional to how far cells must shift. Swapping $i$ with a **space** $s$
works the same way: the width to find is $w_i$ minus the width of the space, $P_1$ uses the
space's own width, and $S_1$ is the width of the space plus the closest space on each side
of it.

The weights $wt_1$ and $wt_2$ have no unit (the shifts are lengths, like wirelength). Since
moving many cells disturbs the placement much more, **$wt_2$ is set much larger than
$wt_1$**: large overlaps are strongly discouraged. The paper does not give their values.

### 2.4 The benefit of a swap (equation (2))

If the total wirelength of the affected nets is $W_1$ before the swap and $W_2$ after:

$$
b = (W_1 - W_2) - P_1 - P_2 .
$$

$b > 0$ means the swap pays off: the wire it saves outweighs the estimated cost of the
shifting. The penalties are only an estimate; the true wirelength change of resolving the
overlap is hard to measure, and this simple model is good enough.

**Global Swap**, then, is: for each cell $i$, find its optimal region; for every cell and
every space in that region, compute the benefit of swapping with it; take the one with the
best benefit, and swap if that benefit is positive.

### 2.5 Two shortcuts that make it fast

- **First good candidate.** Instead of evaluating every cell and space in the region and
  taking the best, the implementation takes the **first** candidate that gives a certain
  benefit.
- **Legalize once per pass.** After a swap of two cells of different widths the
  placement has overlap around the bigger cell and gaps around the smaller one. Legalizing
  after every swap would be slow, so the whole placement is legalized once after all
  segments have been traversed. Positions are slightly inaccurate in between, and the
  experiments show this does not hurt the final wirelength significantly.

### 2.6 Does it work? (Table I)

Distance of the cells of ibm01 from their optimal regions, in row heights, before and after
one pass of Global Swap:

| distance | 0 | (0, 1] | (1, 2] | (2, 3] | (3, 4] | > 4 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| before | 30.0% | 36.8% | 18.0% | 6.4% | 3.4% | 5.4% |
| after | 33.0% | 39.3% | 17.1% | 5.3% | 2.2% | 3.1% |

More cells are in or near their optimal region after one pass, and the far tail shrinks.
Because the target comes from the optimal region and not from a fixed window around the
cell, Global Swap has a global view: it can move a cell next door or across the chip.

> [!IMPORTANT]
> **Key takeaways**
>
> - A cell's optimal region lies between the medians of the bounds of its nets' bounding boxes, computed without the cell, separately in x and y.
> - Global Swap exchanges a cell with a cell or a gap in its optimal region.
> - Overlap from unequal widths costs $P_1$ (shift the two neighbours) and $P_2$ (shift more cells), with $wt_2 \gg wt_1$.
> - A swap is made when its benefit $b = (W_1 - W_2) - P_1 - P_2$ is positive; the placement is re-legalized once per pass.

### Exercises

**1.** A cell is on two nets whose boxes, without it, span $[0, 10]$ and $[20, 30]$ in x.
What is its optimal region in x, and what does that say?

<details class="success" markdown>
<summary>Answer</summary>

Bounds sorted: 0, 10, 20, 30; medians 10 and 20, so anywhere in $[10, 20]$. Between the
two boxes, moving right lengthens the first net exactly as much as it shortens the second,
so every position in between is equally good.

</details>

**2.** Cell $i$ (width 6) is swapped into the place of cell $j$ (width 2). The spaces next
to $j$ are 1 and 1, and the next ones out are 0.5 and 0.5. Which penalties apply?

<details class="success" markdown>
<summary>Answer</summary>

The extra width is 4. $s_2 + s_3 = 2 < 4$, so $P_1 = (4 - 2)\,wt_1 = 2\,wt_1$.
$S_1 = 0.5 + 1 + 1 + 0.5 = 3 < 4$, so more cells must move too: $P_2 = (4 - 3)\,wt_2 = wt_2$.

</details>

**3.** Why is $wt_2$ much larger than $wt_1$?

<details class="success" markdown>
<summary>Answer</summary>

$P_2$ counts overlap that can only be resolved by shifting cells beyond the two direct
neighbours, which disturbs much more of the placement and is poorly predicted by the
estimate. A large $wt_2$ makes such swaps rare.

</details>

---

[← 1. Detailed placement and the FastDP flow](01-the-flow.md) · [FastDP contents](README.md) · [3. Vertical Swap and Local Re-ordering →](03-vertical-swap-and-reordering.md)
