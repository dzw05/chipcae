# Run crosschecks

`tools/vv/real_case_crosscheck.py` reads existing first-step result directories
(manifest and VTKHDF). It does not launch a solver. The tests generate all inputs
independently with numpy and h5py in temporary directories, including partitioned
hex and cut cells. Only a missing numpy or h5py dependency skips a test (code 77).

`mesh-seq` accepts exactly three runs, sorts them from fine to coarse, and reports
discretization uncertainty for `warpage_pv`, `U_mag_max`, and, when ambient
temperature is supplied, `T_max_rise = T_max - t_ambient`. Distinct mesh hashes
and grid sizes are required. A displacement QoI whose method starts with
`not solved` has a null estimate and an explanatory note.

`pair` compares FEM and FVM runs with the same mesh hash using the existing
`field_compare` `fem-fvm` preset, including rigid displacement removal. The
embedded comparison is preserved unchanged. Its **only gate** is the relative
maximum-temperature-rise difference of at most 0.01. Other field and QoI metrics
are reported without gates. The two input directories must resolve to different
locations, including when symbolic links are used.

## Numerical method

Reference: Celik, I. B., Ghia, U., Roache, P. J., Freitas, C. J., Coleman, H.,
and Raad, P. E. (2008), “Procedure for Estimation and Reporting of Uncertainty
Due to Discretization in CFD Applications,” *Journal of Fluids Engineering*,
130(7), 078001, doi:10.1115/1.2960953. The implementation uses the three-grid
safety factor `Fs = 1.25` and supports unequal refinement ratios.

The three-dimensional scale is `h = (sum(volume) / N) ** (1/3)`, with owned hex
and cut cells included in `N`. Volumes are summed as float64 in the loader's
canonical order using the default numpy sum order. Missing volumes are invalid.
Adjacent scales separated by at most `1e-12 * h_coarser` are treated as equal.

With `h1 < h2 < h3`, define `r21 = h2/h1`, `r32 = h3/h2`,
`eps21 = phi2 - phi1`, `eps32 = phi3 - phi2`, and `s = sign(eps32/eps21)`.
Starting from `p0 = abs(log(abs(eps32/eps21))) / log(r21)`, update

```text
q(p) = log((r21**p - s) / (r32**p - s))
p_next = abs(log(abs(eps32/eps21)) + q(p)) / log(r21)
```

Stop when the absolute order change is at most `1e-12`, with at most 200 updates.
The extrapolate and errors are

```text
phi_ext21 = (r21**p * phi1 - phi2) / (r21**p - 1)
e_a21 = abs((phi1 - phi2) / phi1)
e_ext21 = abs((phi_ext21 - phi1) / phi_ext21)
gci_fine21 = Fs * e_a21 / (r21**p - 1)
```

The status is `monotone` for converged same-sign differences or `oscillatory`
for converged opposite-sign differences. Either exactly zero difference gives
`flat`, zero updates, and null derived quantities. Iteration exhaustion, invalid
logarithms or powers, or a nonpositive extrapolation denominator gives
`no_convergence` and null derived quantities. A zero fine-grid value gives null
`e_a21` and GCI; a zero extrapolate gives null `e_ext21`. NaN/Inf input is invalid.

**mesh-seq has no pass/fail gate**: neither an order threshold nor a GCI threshold
has been approved. It always writes verdict `report` for valid sequences,
including flat, oscillatory, and nonconvergent estimates.

## Reports and placeholder usage

Run these commands from the repository root, replacing the placeholders:

```sh
python3 tools/vv/real_case_crosscheck.py mesh-seq \
  <run_coarse> <run_fine> <run_middle> --t-ambient <ambient_K> \
  --data-class real --case-id <case_id> -o <outside_git>/mesh_seq.json

python3 tools/vv/real_case_crosscheck.py pair \
  --fem <run_fem> --fvm <run_fvm> --t-ambient <ambient_K> \
  --data-class real --case-id <case_id> -o <outside_git>/pair.json
```

Real FEM reference runs remain blocked by issue #205 until the required solver
support is available. Actual real-case runs are outside these synthetic tests.

`--data-class` is required and accepts `synthetic`, `public`, or `real`.
`--case-id` defaults to an empty string and must be a path-free identifier
(neither slash nor backslash). `-o report.json` writes `report.json` and
`report.md`; both destinations are checked before either file is written. Real
outputs must resolve outside all git trees, including through symbolic links.
All input validation and serialization completes before writing either report.

The schema is `chipcae.vv_crosscheck/0.1`. Each run contains its directory basename,
mesh hash, `N`, `h`, all three QoIs, and their methods. Records include `kind`,
`case_id`, `data_class`, estimates or the embedded pair comparison, `verdict`,
and `notes`. Input absolute paths are not added to records. JSON disallows
nonfinite numbers; Markdown lists all nested quantities with float `repr`
precision. Successful stdout contains only the verdict and output paths.

| Exit code | Meaning |
|---|---|
| 0 | Reports written; mesh-seq report or pair gate passed |
| 1 | Reports written; pair temperature-rise gate failed |
| 2 | Usage error, including protected real output or identical pair directories |
| 3 | Repeated mesh hash or indistinguishable mesh-seq scale, or pair mesh mismatch |
| 14 | Invalid input, nonfinite computed input metric, serialization or I/O error |

## Tests

```sh
ctest --test-dir <build> -R '^real_case_crosscheck_gci$' --output-on-failure
ctest --test-dir <build> -R '^real_case_crosscheck_cli$' --output-on-failure
ctest --test-dir <build> -R '^field_compare_unit$' --output-on-failure
```

The numerical assertions retain the specified analytic-order and extrapolation
tolerances. An independent binary-exact manufactured solution checks GCI to a
relative tolerance of `1e-14`. The synthetic CLI ambient is 1 K to avoid loss of
precision when recovering order from small temperature-rise differences.
