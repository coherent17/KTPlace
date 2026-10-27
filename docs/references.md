[Documentation home](README.md)

# References

The papers the guides follow. The guides explain them in their own words, so the
papers themselves are not part of this repository; follow the links.

## Global placement

- **SimPL.** M.-C. Kim, D.-J. Lee, I. L. Markov. "SimPL: An Algorithm for Placing
  VLSI Circuits." *Communications of the ACM* 56(6), 105–113, June 2013.
  [doi:10.1145/2461256.2461279](https://doi.org/10.1145/2461256.2461279).
  Original version: "SimPL: An Effective Placement Algorithm", *IEEE Transactions on
  Computer-Aided Design* 31(1), January 2012. Guide:
  [SimPL](global-placement/simpl/README.md).
- **GORDIAN** (quadratic placement with springs). J. M. Kleinhans, G. Sigl,
  F. M. Johannes, K. J. Antreich. "GORDIAN: VLSI Placement by Quadratic Programming
  and Slicing Optimization." *IEEE TCAD* 10(3), 356–365, 1991. Guide:
  [Quadratic placement](foundations/03-quadratic-placement.md).
- **Kraftwerk2** (origin of the Bound2Bound net model). P. Spindler, U. Schlichtmann,
  F. M. Johannes. "Kraftwerk2 – A Fast Force-Directed Quadratic Placement Approach
  Using an Accurate Net Model." *IEEE TCAD* 27(8), 1398–1411, 2008.
- **NTUplace.** T.-C. Chen, T.-C. Hsu, Z.-W. Jiang, Y.-W. Chang. "NTUplace: A Ratio
  Partitioning Based Placement Algorithm for Large-Scale Mixed-Size Designs."
  *ISPD* 2005.

## Legalization

- **Abacus.** P. Spindler, U. Schlichtmann, F. M. Johannes. "Abacus: Fast
  Legalization of Standard Cell Circuits with Minimal Movement." *ISPD* 2008, 47–53.
  [doi:10.1145/1353629.1353640](https://doi.org/10.1145/1353629.1353640). Guide:
  [Abacus](legalization/abacus/README.md).
- **Tetris.** D. Hill. "Method and System for High Speed Detailed Placement of Cells
  Within Integrated Circuit Designs." US Patent 6,370,673, 2002.

## Detailed placement

- **FastDP.** M. Pan, N. Viswanathan, C. Chu. "An Efficient and Effective Detailed
  Placement Algorithm." *ICCAD* 2005, 48–55.
  [doi:10.1109/ICCAD.2005.1560039](https://doi.org/10.1109/ICCAD.2005.1560039). Guide:
  [FastDP](detailed-placement/fastdp/README.md).
- **Single-row problem.** A. B. Kahng, P. Tucker, A. Zelikovsky. "Optimization of Linear
  Placements for Wirelength Minimization with Free Sites." *ASP-DAC* 1999, 241–244. The
  clustering optimality argument both Abacus and FastDP rely on.
- **Median rule.** S. Goto. "An Efficient Algorithm for the Two-Dimensional Placement
  Problem in Electrical Circuit Layout." *IEEE Transactions on Circuits and Systems*
  28(1), 12–18, 1981.

## Background

- A. B. Kahng, J. Lienig, I. L. Markov, J. Hu. *VLSI Physical Design: From Graph
  Partitioning to Timing Closure.* Springer, 2011. Chapter 4 covers placement.
- Y. Saad. *Iterative Methods for Sparse Linear Systems.* SIAM, 2003. Conjugate
  gradient and preconditioning.
- L. N. Trefethen, D. Bau. *Numerical Linear Algebra.* SIAM, 1997.
