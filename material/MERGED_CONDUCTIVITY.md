# Internal merged-group conductivity

`detail/merged_conductivity.hpp` evaluates at most eight positive, diagonal member
tensors, in W/(m K). This is an M4 implementation detail, with no production
callers or frozen cross-module interface. Inputs are member-major triples
`k_diag[3*i+c]` and `cell_ijk[3*i+a]`, plus `volume[i]`; output order is
`xx, yy, zz, yz, xz, xy`.

## Model and derivations

Write `K_i = diag(k_i,x, k_i,y, k_i,z)` and `w_i = V_i / sum(V_i)`.
The following calculations specialize the brief's physical model to positive
diagonal tensors; they do not reconstruct a continuous microgeometry.

For complete parallel layers normal to axis `a`, steady flux conservation makes
`q_a` constant. With driving gradient `g = -grad(T)`, the average normal gradient
is `sum(w_i q_a/k_i,a)`. Thus

```text
q_a = K_aa <g_a>,   K_aa = H_a = 1 / sum(w_i/k_i,a).
```

Continuity of temperature makes each tangential gradient `g_b` constant across
the interfaces. Averaging `q_i,b = k_i,b g_b` gives

```text
<q_b> = sum(w_i k_i,b) g_b,   K_bb = A_b = sum(w_i k_i,b), b != a.
```

With aligned diagonal constituents these experiments produce no transverse
coupling: `K_ab = 0` for `a != b`. Series and parallel experiments attain the
harmonic and arithmetic bounds, respectively. The exact laminate formulas are
also given by Milton [2], section 9.2, pp. 159–162, equation (9.7) and its
diagonal specialization.

For arbitrary periodic or statistically homogeneous microgeometry, let `< >`
denote volume average. Minimizing gradient energy at prescribed average gradient
`g0`, and trying the constant admissible gradient, yields

```text
g0^T K* g0 = min_<g>=g0 <g^T K g> <= g0^T <K> g0.
```

The complementary minimum at prescribed divergence-free average flux `q0`,
with constant flux as trial field, yields

```text
q0^T (K*)^-1 q0 = min_<q>=q0 <q^T K^-1 q> <= q0^T <K^-1> q0.
```

Inversion reverses the positive-definite matrix order. Since both inequalities
hold for every test vector, the Wiener (Voigt/Reuss) bounds are

```text
<K^-1>^-1 <= K* <= <K>       (Loewner order).
```

See [2], section 13.1, pp. 271–273, equations (13.10)–(13.13).
For diagonal inputs the lower tensor is `diag(H_x,H_y,H_z)`. Each weighted
mean lies between its component extrema. Positive diagonals and zero
off-diagonals establish symmetry and positive definiteness.

For nonlaminate classification this implementation selects that Reuss lower
tensor. In the homogenization model it does not overestimate conductivity, and
therefore increases resistance for a prescribed flux. This choice needs the
architect's confirmation as the desired conservative direction; it does not
prove an upper bound on every temperature in a general boundary-value problem.
Hill [3] supplies the analogous elasticity naming/history, not a thermal
constitutive law.

The equal-area, two-dimensional square checkerboard has in-plane conductivity
`G = sqrt(k1*k2)` by phase-interchange duality [4,5]. For positive `a,b`,
`(sqrt(a)-sqrt(b))^2 >= 0` implies `2 sqrt(ab) <= a+b`. Dividing or multiplying
by positive quantities gives

```text
H = 2ab/(a+b) <= sqrt(ab) = G <= (a+b)/2 = A.
```

Equality throughout requires `a=b`. The test uses `a=394`, `b=0.6` and compares
the returned harmonic in-plane values with this exact reference. This reference
concerns a repeated two-dimensional checkerboard; it is not an exact solution
for an arbitrary finite collection or its out-of-plane direction. Dykhne [5],
section 2, p. 64, equation (9) explicitly includes the chessboard geometry.

The `g4pp_sliver` fixture uses weights `1e-4` and `0.9999` in the same laminate
equations. It tests the series/parallel laws without imposing a ratio threshold.
The conductivity constants come from the `materials[0].conductivity` fields in
`library/copper.materials.json` and `library/bt_hl832ns_eq.materials.json`.
All ordinary per-cell inputs reuse `copper_laminate_conductivity`.

## Classification and reproducibility

Insertion sort orders eight stack indices by signed `(ix,iy,iz)`, followed by
unsigned binary64 representations of `(kxx,kyy,kzz,volume)`. Every sum follows
this order. Fully tied members are numerically identical, so their order does
not matter. Duplicates remain separate volume contributions.

Bitwise equality of every conductivity triple selects `Uniform` and copies the
input diagonals exactly. Otherwise an axis qualifies when every pair sharing
its index has the same triple. A unique candidate selects `Laminate`, with the
harmonic normal and arithmetic tangents; zero or multiple candidates select
`ReussBound`. An axis with one member per layer qualifies vacuously. Conflicting
conductivities at duplicate coordinates disqualify all axes.

This discrete rule does not check connectivity, face adjacency, layer thickness,
complete lateral coverage, or whether cut volumes represent actual slabs. A
`Laminate` result is exact only under the physical layering assumptions above.
This function does not replace a geometry-resolved homogenization solve.

Validation order is count (`n<=0`, then `n>8`), pointers, all volumes, then all
conductivities. Positive finite binary64 values, including subnormals, are
accepted. Every error returns without writing any output field. Successful
results are assembled locally; off-diagonals are explicitly positive zero.
There is no allocation, exception, recursion, I/O, or Kokkos dependency.

## Arithmetic and its limits

The usual path computes `v_i=V_i/max(V)` and `w_i=v_i/sum(v_i)`, avoiding overflow
in the volume total. For each component it evaluates the equivalent expressions

```text
A = max(k) * sum(w_i * (k_i/max(k))),
H = min(k) / sum(w_i * (min(k)/k_i)).
```

No raw reciprocal of a subnormal conductivity is needed. If a normalized term
is subnormal or zero, a fallback first decomposes each positive input as
`mantissa * 2^exponent`, with mantissa in `[1,2)`. It sums `V`, and either `V*k`
or `V/k`, after independently shifting their exponents to their maxima. The
final quotient restores the exponent difference. This preserves significant
resistance when a very small volume and conductivity occur together. Scaling
uses representable powers of two, including exact normalization of subnormal
inputs. All fallback sums still follow the sorted member order. Component
extrema clamp floating-point overshoot; they do not change the mathematical
mean.

Products have explicit volatile binary64 stores before accumulation to prevent
FMA contraction even when callers do not inherit the material target's private
FP flags. Bit copies use standard character access, without inactive union reads
or host-only calls. The header requires IEEE binary64. Reproducibility assumes
round-to-nearest arithmetic, gradual underflow, and no unsafe reassociation,
reciprocal approximation, or flush-to-zero options.

The acceptance sweep verifies relative error `1e-14` over its specified physical
range; it is not an exhaustive full-range accuracy proof. Five extra cases
cover overflowing volume totals, subnormal volumes, large conductivities,
subnormal conductivities, and coupled extreme volume/conductivity ratios.
Uniform copies remain exact. For nonuniform subnormal outputs, quantization
can exceed a `1e-14` relative error, and doubling a rounded subnormal mean need
not equal recomputing the mean after doubling its inputs. Such guarantees cannot
hold for all positive binary64 values. No new arithmetic-error status is added.

The original 480-case sweep, 120 permutations, 1242 scaling variants and 787
invalid-input evaluations retain their assertions. The five extra legal cases
also enter the one-call-per-work-item DefaultExecutionSpace comparison, for
1868 total legal evaluations. The available OpenMP backend verifies CPU parity;
CUDA/HIP bitwise parity and Apple clang/macOS require separate execution on those
platforms. Host measurements and regression status/time comparisons belong in
the task's external verification log.

## Verified references

Bibliography and cited locations checked on 2026-11-26. “已读” below refers only
to the specified passages actually consulted, not an entire book. “转引” means
the original article was not read. Page numbers are printed page numbers.

1. **O. Wiener**, *Die Theorie des Mischkörpers für das Feld der stationären
   Strömung. Erste Abhandlung: Die Mittelwertsätze für Kraft, Polarisation und
   Energie*, Abhandlungen der Mathematisch-Physischen Klasse der Königlich
   Sächsischen Gesellschaft der Wissenschaften **32**, no. 6 (1912), pp. 509–604.
   **转引** through [2], section 27.3, p. 574, equation (27.5), and bibliography
   p. 601. Title, series and extent checked against the
   [SLUB catalogue](https://katalog.slub-dresden.de/en/id/0-1373700157), which
   records publication as “1912 [erschienen] 1913”. No unverified page within
   Wiener's original is asserted for the bounds.
2. **G. W. Milton**, *The Theory of Composites*, Cambridge University Press
   (2002), ISBN 978-0-521-78125-1. **已读**: section 9.2, pp. 159–162; section
   13.1, pp. 271–273; section 3.2, pp. 49–50; section 27.3, p. 574; relevant
   bibliography entries. Consulted the
   [author-hosted text](https://gwmilton.com/TheoryCompositesNOPRINT.pdf), whose
   copyright page identifies the 2002 Cambridge original and subsequent rights
   reassignment. Locations above use its printed pagination, not the PDF index.
3. **R. Hill**, “The Elastic Behaviour of a Crystalline Aggregate”,
   *Proceedings of the Physical Society. Section A* **65**(5) (1952), pp. 349–354,
   [doi:10.1088/0370-1298/65/5/307](https://doi.org/10.1088/0370-1298/65/5/307).
   **转引** through [2], section 13.1, p. 273 and bibliography p. 287. Original
   publisher full text was inaccessible; only its bibliographic identity and
   the secondary attribution are used.
4. **J. B. Keller**, “A Theorem on the Conductivity of a Composite Medium”,
   *Journal of Mathematical Physics* **5**(4) (1964), pp. 548–549,
   [doi:10.1063/1.1704146](https://doi.org/10.1063/1.1704146). **转引** through
   [2], section 3.2, p. 49, equations (3.9)–(3.11). The original publisher text
   was inaccessible; the checkerboard application was also checked directly
   in [5].
5. **A. M. Dykhne**, “Conductivity of a Two-Dimensional Two-Phase System”,
   *Soviet Physics JETP* **32**(1) (1971), pp. 63–65; English translation of
   *Zh. Eksp. Teor. Fiz.* **59** (1970), pp. 110–115. **已读**: section 2,
   pp. 63–64, especially equation (9) and its following chessboard statement,
   in the [English article](https://chair.itp.ac.ru/biblio/papers/ClassicPapersSeminar/Dykhne1970Eng.pdf).
   The 1970 original and 1971 translation explain the differing citation years.

Architect follow-up remains unchanged: confirm the conservative direction,
freeze any future M3 consumer contract, and decide any replacement for the
transition conductivity-ratio constraint in a separate task.
