# KTPlace benchmark status

Generated 2026-10-17 00:33 CST at commit 18d4df9 Fences, I/O pins and FastDP capacity: check what is legal, place it legally.

HPWL is after detailed placement; *legal* is the verdict of ktplace's independent placement check.

## ISPD 2005 vs SimPL

| design | ours | legal | time | SimPL | ours vs SimPL |
|---|---|---|---|---|---|
| adaptec1 | 7.944e+07 | PASS | 22.4 s | 7.687e+07 | +3.3% |
| adaptec2 | 1.022e+08 | PASS | 25.4 s | 9.037e+07 | +13.1% |
| adaptec3 | 2.218e+08 | PASS | 49.6 s | 2.064e+08 | +7.5% |
| adaptec4 | 1.908e+08 | PASS | 55.1 s | 1.860e+08 | +2.6% |
| bigblue1 | 9.892e+07 | PASS | 28.6 s | 9.585e+07 | +3.2% |
| bigblue2 | not run | | | 1.436e+08 | |
| bigblue3 | not run | | | 3.362e+08 | |
| bigblue4 | not run | | | 7.968e+08 | |

Geomean gap to SimPL: +5.9% over 5 design(s).

## ISPD 2015 vs DREAMPlace 3.0 (fenced designs)

| design | fences | ours | legal | time | Eh?Placer | NTUplace4dr | DREAMPlace 3.0 | ours vs DP3 | ours vs NTU4dr |
|---|---|---|---|---|---|---|---|---|---|
| mgc_des_perf_a | 4 | 2.428e+09 | PASS | 35.8 s | 2.690e+09 | 2.440e+09 | 2.230e+09 | +8.9% | -0.5% |
| mgc_des_perf_b | 12 | 2.304e+09 | PASS | 39.7 s | 1.770e+09 | 2.000e+09 | 1.800e+09 | +28.0% | +15.2% |
| mgc_edit_dist_a | 1 | 4.161e+09 | PASS | 20.5 s | 5.080e+09 | 4.910e+09 | 4.260e+09 | -2.3% | -15.3% |
| mgc_matrix_mult_b | 3 | 3.557e+09 | PASS | 13.0 s | 3.820e+09 | 3.640e+09 | 3.160e+09 | +12.6% | -2.3% |
| mgc_matrix_mult_c | | not in benchmark/ | | | 3.710e+09 | 3.480e+09 | 3.060e+09 | | |
| mgc_pci_bridge32_a | 4 | 4.287e+08 | PASS | 4.3 s | 4.100e+08 | 4.720e+08 | 3.880e+08 | +10.5% | -9.2% |
| mgc_pci_bridge32_b | 3 | 6.322e+08 | PASS | 4.5 s | 8.140e+08 | 6.980e+08 | 6.360e+08 | -0.6% | -9.4% |
| mgc_superblue11_a | 4 | 3.892e+10 | PASS | 225.4 s | 3.930e+10 | 4.000e+10 | 3.510e+10 | +10.9% | -2.7% |
| mgc_superblue16_a | 2 | 2.797e+10 | PASS | 85.0 s | 2.860e+10 | 2.940e+10 | 2.690e+10 | +4.0% | -4.9% |

Geomean gap to DREAMPlace 3.0: +8.6% over 8 design(s).

### Other ISPD 2015 designs (no published HPWL)

| design | ours | legal | time |
|---|---|---|---|
| mgc_des_perf_1 | 1.188e+09 | PASS | 25.3 s |
| mgc_fft_1 | 4.115e+08 | PASS | 3.5 s |
| mgc_fft_2 | 3.557e+08 | PASS | 2.5 s |
| mgc_fft_a | 7.044e+08 | PASS | 2.1 s |
| mgc_fft_b | 8.652e+08 | PASS | 1.7 s |
| mgc_matrix_mult_1 | 2.177e+09 | PASS | 18.6 s |
| mgc_matrix_mult_a | 3.225e+09 | PASS | 9.4 s |
| mgc_superblue12 | 2.759e+10 | PASS | 191.3 s |

## ICCAD04 (no published reference here)

| design | ours | legal | time |
|---|---|---|---|
| dma | 3.945e+08 | PASS | 3.9 s |
| ibm01 | 2.890e+06 | PASS | 1.2 s |
| ibm05 | 9.622e+06 | PASS | 2.6 s |
| ibm10 | 6.248e+07 | PASS | 19.2 s |
| ibm18 | 4.733e+07 | PASS | 42.0 s |

## Sources

- SimPL: Kim, Lee, Markov, SimPL, IEEE TCAD 2012, Table I (legal HPWL after FastPlace-DP and Capo cell flipping; flipping is not allowed by the ISPD 2005 contest)
- DREAMPlace 3.0: Gu et al., DREAMPlace 3.0, ICCAD 2020, Table 1 (legal HPWL with fence regions; Eh?Placer, NTUplace4dr and DREAMPlace 3.0)
