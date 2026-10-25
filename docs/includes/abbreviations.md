*[HPWL]: Half-perimeter wirelength: half the perimeter of each net's bounding box, summed over all nets
*[CG]: Conjugate gradient: an iterative method that solves Qx = b by walking downhill along directions that do not undo each other
*[PCG]: Preconditioned conjugate gradient
*[B2B]: Bound2Bound net model: every pin is joined to the net's two extreme pins, with weights that make the squared cost equal HPWL
*[Bound2Bound]: A net model that joins every pin to the net's two extreme pins, with weights that make the squared cost equal HPWL at the current placement
*[LAL]: Look-ahead legalization: SimPL's step that spreads the lower-bound placement into a nearly legal upper-bound placement
*[LSS]: Linear system solver: the CG solve that produces SimPL's lower-bound placement
*[SPD]: Symmetric positive definite: the cost is a bowl with exactly one bottom
*[CSR]: Compressed sparse row: a sparse matrix stored as only its non-zero entries, row by row
*[SpMxV]: Sparse matrix-vector multiplication, the core operation of CG
*[SIMD]: Single instruction, multiple data: one CPU instruction that works on several numbers at once
*[SSE2]: Streaming SIMD Extensions 2: x86 instructions that work on four floats at once
*[BFS]: Breadth-first search: visit a starting point, then its neighbours, then theirs, and so on
*[ISPD]: International Symposium on Physical Design, whose contests provide the standard placement benchmarks
*[OpenMP]: A standard for running C and C++ loops on several threads
*[TCAD]: IEEE Transactions on Computer-Aided Design of Integrated Circuits and Systems
*[CACM]: Communications of the ACM
*[netlist]: The cells of a design and the nets (wires) that connect them
*[hyperedge]: An edge that joins more than two nodes: a net with three or more pins
*[hyperedges]: Edges that join more than two nodes: nets with three or more pins
*[pseudonet]: An imaginary two-pin net that ties a cell to its anchor
*[pseudonets]: Imaginary two-pin nets that tie each cell to its anchor
*[Jacobi preconditioner]: Dividing each equation by its diagonal entry before solving, which makes the bowl rounder
