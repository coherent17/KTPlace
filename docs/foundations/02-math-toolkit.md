[← 1. The placement problem and wirelength](01-placement-problem.md) · [Foundations contents](README.md) · [3. Quadratic placement →](03-quadratic-placement.md)

# 2. The math toolkit, gently

This chapter covers the math the rest of the guide uses beyond basic calculus:
vectors and matrices, derivatives with several variables, what makes a problem have
exactly one answer, and how running time is measured. It assumes you know
single-variable derivatives and $\sum$ notation; no linear algebra is assumed.

Each section has three pieces:

- **The idea** in everyday words.
- **A tiny example** with small numbers you can check by hand.
- **Where placement uses it**, so you know why it is here.

Chapters 3 to 5 reuse these ideas constantly; the algorithm guides mostly don't need
new math.

### 2.1 Notation used in this guide

| You see | Meaning |
| --- | --- |
| $x_i$ | the x coordinate of cell $i$ |
| $\vec x$ | the list of all the cells' x coordinates ([§2.2](#22-vectors-a-list-of-numbers)) |
| $Q$, $Q_{ij}$ | a matrix, and its entry in row $i$, column $j$ ([§2.3](#23-matrices-a-table-that-turns-a-list-into-another-list)) |
| $\lvert x_1 - x_2 \rvert$ | the distance between $x_1$ and $x_2$ on a line |
| $e \in E$ | "$e$ is one of the nets in the set $E$" |
| $\frac{\partial f}{\partial x_1}$, $\nabla f$ | partial derivative and gradient ([§2.5](#25-derivatives-with-many-variables)) |
| $\Phi$ | the cost being minimized (Greek phi) |
| $\alpha, \gamma, \kappa$ | alpha, gamma, kappa: the anchor weight, the target density and the condition number |

### 2.2 Vectors: a list of numbers

**The idea.** A vector is a list of numbers with a single name. If you have four
cells, their four x coordinates form one vector called $\vec x$ (the little arrow
just reminds you "this is a list, not one number"). Think of one column of a
spreadsheet:

| cell | x |
| --- | --- |
| 1 | 1.5 |
| 2 | 2.0 |
| 3 | 2.4 |
| 4 | 3.1 |

$$
\vec x = \begin{bmatrix} x_1 \\ x_2 \\ x_3 \\ x_4 \end{bmatrix} = \begin{bmatrix} 1.5 \\ 2.0 \\ 2.4 \\ 3.1 \end{bmatrix}
$$

Writing $\vec x^T$ ("x transpose") only means the same list written sideways, as a
row: $[1.5\ \ 2.0\ \ 2.4\ \ 3.1]$. Nothing else changes.

**The inner product: a shopping receipt.** You buy 2 apples at 3 dollars each, 1 bread
at 4 dollars and 5 eggs at 1 dollar each. Your bill is $2\cdot3 + 1\cdot4 + 5\cdot1 = 15$. That calculation,
"multiply matching items and add everything up", is the **inner product** of the
quantity list $[2, 1, 5]$ and the price list $[3, 4, 1]$:

$$
\vec a^T \vec b = a_1 b_1 + a_2 b_2 + \dots + a_n b_n
$$

**Size of a vector.** Take the inner product of a list with itself: you get the sum
of the squares, $\vec r^T\vec r = r_1^2 + r_2^2 + \dots$. It is zero only if every
entry is zero, and it is large if any entry is large. Its square root is the
vector's **norm** $\lVert \vec r \rVert$, its "length". For $\vec r = [3, 4]$:
$\vec r^T\vec r = 9 + 16 = 25$ and $\lVert\vec r\rVert = 5$.

**Where placement uses it.** All the x coordinates of a million cells are one vector
$\vec x$. When the solver ([Chapter 4](04-solving-linear-systems.md)) wants to know "am I done yet?", it measures the
size of an error vector with a norm. Inner products are the most common operation
inside the solver.

### 2.3 Matrices: a table that turns a list into another list

**The idea.** A matrix is a table of numbers. $Q_{ij}$ means "the number in row $i$,
column $j$". On its own a table is boring; what matters is what it **does** to a
vector.

**Matrix times vector = one receipt per row.** To compute $Q\vec x$, take each row of
the table and do the shopping-receipt calculation ([§2.2](#22-vectors-a-list-of-numbers)) with $\vec x$. Each row
gives one number, so the result is a new list with one number per row:

$$
\begin{bmatrix} 2 & 1\\ 1 & 3 \end{bmatrix}
\begin{bmatrix} 4\\ 5 \end{bmatrix}
=
\begin{bmatrix} 2\cdot4 + 1\cdot5\\ 1\cdot4 + 3\cdot5 \end{bmatrix}
=
\begin{bmatrix} 13\\ 19 \end{bmatrix}
$$

In symbols, the $i$-th entry of the answer is
$(Q\vec x)_i = Q_{i1}x_1 + Q_{i2}x_2 + \dots + Q_{in}x_n$.

**Why we care: a system of equations is one line.** Turn the example around.
Suppose you did not know the vector, only the answer:

$$
\begin{aligned}
2a + 1b &= 13\\
1a + 3b &= 19
\end{aligned}
$$

That is the same as $\begin{bmatrix} 2 & 1\\ 1 & 3 \end{bmatrix}\begin{bmatrix} a\\ b \end{bmatrix} = \begin{bmatrix} 13\\ 19 \end{bmatrix}$,
and we already know the answer is $a = 4$, $b = 5$. So "$Q\vec x = \vec b$" is
nothing scary: it is **a short way to write a list of equations**, where $Q$ holds
the numbers in front of the unknowns, $\vec x$ is the list of unknowns, and $\vec b$
is the list of right-hand sides. "Solving $Q\vec x = \vec b$" means "find the
unknowns that make every equation true".

[Chapter 3](03-quadratic-placement.md) will produce this system with four unknowns, one per cell:

$$
\begin{aligned}
5x_1 - x_2 - x_3 &= 3\\
-x_1 + 2x_2 - x_3 &= 0\\
-x_1 - x_2 + 4x_3 - 2x_4 &= 0\\
-2x_3 + 6x_4 &= 14
\end{aligned}
\quad\Longleftrightarrow\quad
\underbrace{\begin{bmatrix} 5 & -1 & -1 & 0\\ -1 & 2 & -1 & 0\\ -1 & -1 & 4 & -2\\ 0 & 0 & -2 & 6 \end{bmatrix}}_{Q}
\underbrace{\begin{bmatrix} x_1\\ x_2\\ x_3\\ x_4 \end{bmatrix}}_{\vec x}
=
\underbrace{\begin{bmatrix} 3\\ 0\\ 0\\ 14 \end{bmatrix}}_{\vec b}
$$

A missing unknown in an equation is a 0 in the table (row 1 has no $x_4$, so
$Q_{14} = 0$).

**Words you will meet.** Use the 4×4 table above to see each one.

- **Diagonal**: the entries from top-left to bottom-right, $Q_{11}, Q_{22}, \dots$
  Here: 5, 2, 4, 6. In placement, each diagonal entry belongs to one cell.
- **Symmetric**: the table is a mirror image across the diagonal: $Q_{ij} = Q_{ji}$.
  Here $Q_{12} = Q_{21} = -1$ and $Q_{34} = Q_{43} = -2$. In placement this is
  automatic, because a wire between cells 3 and 4 is the same wire seen from either
  end.
- **Sparse**: almost every entry is zero. A real chip has a million cells, so the
  table has a million rows and a million columns, but each cell is wired to only 5
  to 10 others, so each row has only 5 to 10 non-zero entries. The computer stores
  only the non-zeros (one common layout is called **CSR**, compressed sparse row).
- **Inverse** $Q^{-1}$: for one number, $3x = 6$ is solved by dividing:
  $x = 6/3$. For a table, the "dividing" is the inverse matrix, so the solution is
  written $\vec x = Q^{-1}\vec b$. This is only notation. For a huge table nobody
  actually computes $Q^{-1}$; [Chapter 4](04-solving-linear-systems.md) shows what is done instead. The one easy case:
  if a table has numbers only on its diagonal, "dividing by it" just means dividing
  each equation by its own diagonal number. [Chapter 4](04-solving-linear-systems.md) uses exactly that trick.

### 2.4 The quadratic form $\vec x^T Q \vec x$: a sum of squares in disguise

**The idea.** You will see the expression $\vec x^TQ\vec x$. Read it in two steps:
first compute the list $Q\vec x$ ([§2.3](#23-matrices-a-table-that-turns-a-list-into-another-list)), then take its inner product with $\vec x$
([§2.2](#22-vectors-a-list-of-numbers)). The result is a single number.

**A tiny example.** $Q = \begin{bmatrix} 2 & 1\\ 1 & 3 \end{bmatrix}$ and
$\vec x = \begin{bmatrix} 1\\ 2 \end{bmatrix}$.

- Step 1: $Q\vec x = [2\cdot1 + 1\cdot2,\ 1\cdot1 + 3\cdot2] = [4, 7]$.
- Step 2: $\vec x^T(Q\vec x) = 1\cdot4 + 2\cdot7 = 18$.

If you do the same thing with letters for a 2×2 table
$\begin{bmatrix} a & b\\ b & c \end{bmatrix}$, it always comes out as

$$
\vec x^TQ\vec x = a x_1^2 + 2b\,x_1x_2 + c x_2^2
$$

Check: $2\cdot1^2 + 2\cdot1\cdot1\cdot2 + 3\cdot2^2 = 2 + 4 + 12 = 18$ ✓.

**Why it matters.** Every term is a square ($x_1^2$) or a product of two unknowns
($x_1x_2$). Functions built only from such terms are called **quadratic**. The
wire cost in [Chapter 3](03-quadratic-placement.md) is a sum of squares like $(x_1 - x_2)^2$, so it can always be
written as $\vec x^TQ\vec x$ plus simpler terms. When the paper writes
$\tfrac12\vec x^TQ_x\vec x$, read: "the sum of squared wire lengths, packed into a
table".

### 2.5 Derivatives with many variables

Every equation in [Chapter 3](03-quadratic-placement.md) comes from one fact you
know from calculus: **at the minimum of a smooth function, the derivative is zero.**
The only rule needed is the chain rule applied to a square,

$$
\frac{d}{dx}\,w(x - c)^2 = 2w(x - c)
$$

for constants $w$ and $c$.

**Partial derivatives.** A placement cost depends on many unknowns at once, one per
cell. With two unknowns, picture a terrain with an east–west direction ($x_1$) and
a north–south direction ($x_2$). The **partial derivative**
$\frac{\partial f}{\partial x_1}$ is the ordinary derivative in $x_1$ while $x_2$ is
held fixed as if it were a constant: the slope you feel stepping only east. The
minimum of the terrain is where the slope is zero **in every direction at once**,
so every partial derivative is zero.

**Example with a physical meaning.** Cell 1 is wired to a fixed pin at position 1,
and cell 2 is wired only to cell 1. The squared wire cost is

$$
f(x_1, x_2) = (x_1 - 1)^2 + (x_1 - x_2)^2
$$

Slope in the $x_1$ direction: both terms contain $x_1$, so by the rule

$$
\frac{\partial f}{\partial x_1} = 2(x_1 - 1) + 2(x_1 - x_2)
$$

Slope in the $x_2$ direction: only the second term contains $x_2$. Since
$(x_1 - x_2)^2 = (x_2 - x_1)^2$, the rule gives

$$
\frac{\partial f}{\partial x_2} = 2(x_2 - x_1)
$$

Set both to zero. The second gives $x_2 = x_1$; then the first gives $x_1 = 1$. So
both cells sit on the pin, at 1, with zero wire. That is obviously the best
placement, and the slope rule found it mechanically. [Chapter 3](03-quadratic-placement.md) does the same thing for
four cells.

**Two more words.**

- The **gradient** $\nabla f$ is the list of all the partial slopes, here
  $[\,2(x_1-1) + 2(x_1-x_2),\ \ 2(x_2-x_1)\,]$. It is an arrow pointing uphill;
  its negative points downhill. The solver in [Chapter 4](04-solving-linear-systems.md) walks downhill along it.
- The **Hessian** is the table of "slopes of the slopes": how fast each slope
  changes. Here $\frac{\partial f}{\partial x_1} = 4x_1 - 2x_2 - 2$ changes by 4 per step
  in $x_1$ and by −2 per step in $x_2$; $\frac{\partial f}{\partial x_2} = 2x_2 - 2x_1$
  changes by −2 and 2. So the Hessian is
  $\begin{bmatrix} 4 & -2\\ -2 & 2 \end{bmatrix}$: constant numbers, a matrix.
  That is why the paper calls its matrix $Q_x$ "the Hessian": for squared wire
  costs, the table in $Q\vec x = \vec b$ *is* the curvature of the cost (up to a
  factor 2, which never matters, see [§3.4](03-quadratic-placement.md#34-matrix-form-equation-3)).

### 2.6 Bowls: why there is exactly one best answer

**The idea.** Drop a marble into a salad bowl. Wherever you drop it, it rolls to the
same single lowest point. A cost shaped like that is called **convex** (no hidden
side valleys to get stuck in), and **strictly convex** if the bottom is a single
point rather than a flat region. A strictly convex cost has exactly one best answer,
and "keep walking downhill" is guaranteed to find it.

**A cost that is not a nice bowl.** Remove the fixed pin from the example above:
$f(x_1, x_2) = (x_1 - x_2)^2$. Now any placement with $x_1 = x_2$ has zero cost:
both at 0, both at 7, both at 1000. The bottom is not a point but a long flat trough,
and there is no single answer. The paper calls this **degenerate**. Physically: two
cells tied to each other but to nothing fixed can slide anywhere together. That is
why every group of cells must be wired, directly or indirectly, to something fixed.

**The name for "nice bowl".** For costs of the form $\tfrac12\vec x^TQ\vec x - \vec b^T\vec x$,
the bowl curves upward in every direction exactly when $\vec x^TQ\vec x > 0$ for
every list $\vec x$ that is not all zeros. A symmetric table with that property is
called **symmetric positive definite (SPD)**. The example table in §2.4 gave 18 > 0;
[§3.7](03-quadratic-placement.md) shows why placement tables are always SPD. When you read "SPD" or
"nondegenerate" in the paper, think: **a real bowl with one bottom**.

### 2.7 Big-O: how running time grows

**The idea.** Big-O describes how the work grows when the problem grows, ignoring
constant factors. Like a recipe: cooking for twice as many guests takes about twice
the chopping ($O(n)$), but if every guest must shake hands with every other guest,
doubling the guests quadruples the handshakes ($O(n^2)$).

What the common ones mean for a chip with $n = 1{,}000{,}000$ cells, on a computer
doing about $10^9$ simple steps per second ($\log_2 n \approx 20$):

| Growth | Steps | Time |
| --- | ---: | ---: |
| $O(n)$ | $10^6$ | 1 millisecond |
| $O(n \log n)$ | $2\times10^7$ | 0.02 seconds |
| $O(n \log^2 n)$ | $4\times10^8$ | 0.4 seconds |
| $O(n^3)$ | $10^{18}$ | about 30 years |

Real implementations have bigger constants than this, but the gap between the rows
is the point: SimPL's steps are all in the first three rows, and textbook
elimination for solving equations is in the last row. That is why [Chapter 4](04-solving-linear-systems.md) exists.

---

[← 1. The placement problem and wirelength](01-placement-problem.md) · [Foundations contents](README.md) · [3. Quadratic placement →](03-quadratic-placement.md)
