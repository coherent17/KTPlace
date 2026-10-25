[← 3. Quadratic placement](03-quadratic-placement.md) · [Foundations contents](README.md) · [5. Net models: clique, star and Bound2Bound →](05-net-models.md)

# 4. Solving a million equations

*In the SimPL paper: §2.*

### 4.1 Why not just eliminate like in [§3.5](03-quadratic-placement.md#35-the-worked-example-figure-1-and-equation-4)

Elimination on $n$ unknowns costs about $n^3$ steps, and it also fills the sparse
matrix with nonzeros. For $n = 10^6$ that is $10^{18}$ steps. Instead we use
**iterative** methods: start from a guess and improve it, using only
matrix-times-vector products, which for a sparse matrix cost only "number of
nonzeros" steps.

How wrong is a guess $\vec x$? Measure the **residual**

$$
\vec r = \vec b - Q\vec x
$$

At the true solution $\vec r = 0$. And $\vec r$ is exactly the downhill direction:
with $\Phi(\vec x) = \tfrac12\vec x^TQ\vec x - \vec b^T\vec x$, the gradient is
$Q\vec x - \vec b = -\vec r$. So $\vec r$ is the net force on every cell.

### 4.2 The simplest idea: Jacobi iteration

Use [§3.6](03-quadratic-placement.md#36-every-cell-sits-at-the-weighted-average-of-its-neighbours) directly: move every cell to the weighted average of its neighbours'
*current* positions, all at once, and repeat. In matrix language this is
"divide each row by its diagonal entry":

$$
x_i^{\text{new}} = \frac{b_i - \sum_{j \ne i} Q_{ij}\,x_j^{\text{old}}}{Q_{ii}}
$$

It converges for diagonally dominant matrices, but slowly: information travels
one edge per iteration, so a chain of 1000 cells needs thousands of rounds.

### 4.3 Downhill, and why plain downhill zigzags

The cost $\Phi$ is a bowl. **Steepest descent** steps in the downhill direction
$\vec r$, as far as the bowl keeps going down along that line. How far? Along the
line $\vec x + t\vec p$ the cost is a parabola in $t$:

$$
\Phi(\vec x + t\vec p) = \Phi(\vec x) - t\,\vec p^T\vec r + \tfrac12 t^2\,\vec p^TQ\vec p
$$

Its derivative in $t$ is $-\vec p^T\vec r + t\,\vec p^TQ\vec p$, zero at

$$
t = \frac{\vec p^T\vec r}{\vec p^T Q \vec p}
$$

That is the best step length along any direction $\vec p$.

The trouble: if the bowl is a long narrow valley, the steepest direction points
mostly across the valley, not along it. Steepest descent then zigzags from wall
to wall and creeps along the valley floor very slowly.

### 4.4 Conjugate gradient (CG)

CG fixes the zigzag. Each new search direction is the downhill direction, corrected
so that it does not undo the progress already made along the earlier directions
(such directions are called *conjugate* with respect to $Q$). In exact arithmetic
CG reaches the exact answer in at most $n$ steps; in practice it is close enough
much sooner. Each step costs one matrix-times-vector product plus a few inner
products. The paper's statement "since $Q_x$ is symmetric positive definite, CG
iterations provably minimize the residual norm; convergence is monotonic" means CG
is guaranteed to work on exactly the kind of matrix placement produces, and keeps
getting better every step (strictly, CG minimizes the error in the norm defined by
$Q$ itself; the practical meaning is the same).

The **preconditioned** CG algorithm, with preconditioner $P$ (next section; with
$P = $ the identity it is plain CG):

```
r = b - Q x            residual = force on every cell
z = P^-1 r             rescaled residual
p = z                  first search direction
repeat until |r| is small:
    t     = (r . z) / (p . Q p)     best step along p   ([§4.3](#43-downhill-and-why-plain-downhill-zigzags))
    x     = x + t p                 move
    r_new = r - t Q p               new residual (cheap update)
    z_new = P^-1 r_new
    beta  = (r_new . z_new) / (r . z)
    p     = z_new + beta p          new direction: downhill, corrected to stay conjugate
    r, z  = r_new, z_new
```

Only one product $Q\vec p$ per iteration. Iterative solvers of this family are
called **Krylov-subspace** methods (the paper's term).

### 4.5 Condition number and preconditioning

How narrow the valley is, is measured by the **condition number**
$\kappa$: the ratio between the bowl's steepest curvature and its flattest
curvature. $\kappa = 1$ is a perfectly round bowl (one step solves it). Large
$\kappa$ is a long thin valley. The number of CG iterations grows roughly like
$\sqrt\kappa$.

**Preconditioning** reshapes the bowl before solving. Instead of $Q\vec x = \vec b$
solve the equivalent system

$$
P^{-1}Q\vec x = P^{-1}\vec b
$$

for a matrix $P$ that is (a) a rough approximation of $Q$, so that $P^{-1}Q$ is
close to the identity and the bowl is nearly round, and (b) trivial to invert. Both
sides were multiplied by the same thing, so the solution is unchanged.

SimPL uses the **Jacobi preconditioner**: $P$ = the diagonal of $Q$. Inverting it
is just dividing each row by $Q_{ii}$. This works well because placement matrices
are diagonally dominant (the diagonal already carries most of the matrix), and
because rows differ wildly in scale: a cell on one tiny net has a small diagonal, a
cell on fifty nets a huge one; dividing by the diagonal puts all rows on an equal
footing. The more diagonally dominant $Q$ is, the better $P^{-1}Q$ is conditioned,
which is why the anchors in [SimPL chapter 3](../global-placement/simpl/03-anchors-and-convergence.md), which only add to the diagonal, also speed up
CG.

---

[← 3. Quadratic placement](03-quadratic-placement.md) · [Foundations contents](README.md) · [5. Net models: clique, star and Bound2Bound →](05-net-models.md)
