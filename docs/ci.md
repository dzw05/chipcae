# CI, performance gate, and hosted-minute budget

Linux only. There is no macOS or Windows job.

The hosted budget for this private repository is **1,500 minutes per month**. GitHub's Free plan includes 2,000 minutes; the remaining 500 minutes are headroom and are not part of this budget. The estimate below is the planning number for chipcae. It is not a GitHub invoice.

Repository settings (branch protection, variables, secrets) are not changed by the workflows. Names to set by hand are listed below.

## What runs where

| Workflow | When | Runner | Blocks a merge? |
| --- | --- | --- | --- |
| `CI / ci-ok` | After `Lint` and `Build and test` (including when the build is skipped) | `ubuntu-latest` | Yes. This is the only hard gate (ADR-0006) |
| `CI / Lint` | Pull request (`opened`, `synchronize`, `reopened`, `ready_for_review`), including drafts, and push to `main` | Always `ubuntu-latest` | No. It is an input to `ci-ok` |
| `CI / Build and test` | Non-draft pull request that changes something other than docs, and a push to `main` that changes code | `vars.TIER_A_RUNNER` or `ubuntu-latest` | No. Success or skipped both count as passing for `ci-ok` |
| `Tier B and nightly / Nightly clang build` | 18:00 UTC daily, or manual dispatch; skipped if main had no commits in 24h | `ubuntu-latest` | No |
| `Tier B and nightly / Nightly CUDA compile` | Same nightly schedule; skipped if main had no commits in 24h. `nvcc -c` only, the object is not run. Kept until the Kokkos CUDA job's first green run | `ubuntu-latest` | No |
| `Nightly MPI and CUDA Kokkos / Nightly MPI communication` | 18:00 UTC Sunday–Thursday (`0 18 * * 0-4`), or manual dispatch; skipped if main had no commits in 24h. Real `mpirun -np 2` and `-np 4` allreduce tests | `ubuntu-latest` | No |
| `Nightly MPI and CUDA Kokkos / Nightly CUDA Kokkos core compile` | Same weekday schedule, skipped if main had no commits in 24h. Compile-only with Kokkos CUDA 12.6 (`nvcc` from NVIDIA's apt repo); binaries are not run. Timeout 40 minutes. First run is the verification (no local nvcc) | `ubuntu-latest` | No |
| `Tier B and nightly / Hosted minutes` is a different workflow | See usage alert | `ubuntu-latest` | No |
| Tier B MPI, GPU, perf | Nightly, manual dispatch, or a pull request labeled `run-heavy`, and only when `TIER_B_ENABLED` is `true` | Self-hosted labels below | Only after runners exist |

Pushes to `main` that change code also run `Build and test`, so the squash-merged tree is built and later pull requests can reuse the Kokkos and ccache entries on the default branch. Docs-only pushes (`docs/**`, `*.md`, `LICENSE`, `CODEOWNERS`) are ignored by `paths-ignore` and do not start the workflow. `cancel-in-progress` is on for main as well, so a burst of merges keeps only the latest build.

Draft pull requests run lint and `ci-ok`. The build-and-test job stays skipped until the pull request is marked ready (`ready_for_review`), which then runs the full check. `ci-ok` still requires lint to succeed; a skipped build counts as passing.

Concurrency group `ci-<workflow>-<ref>` uses `cancel-in-progress`. A newer push cancels the older run on the same ref.

### Docs-only pulls

`CI / Lint` still runs on docs-only pull requests. `CI / Build and test` is skipped when every changed file is `docs/**`, a `*.md` file, `LICENSE`, or `CODEOWNERS`. A docs-only push to `main` does not start the workflow. `CI / ci-ok` still passes on a docs-only pull because a skipped build is treated as success. That is why individual jobs are not required checks: GitHub treats a skipped required check as not passed.

### Single configuration

The pull-request build is one configuration: GCC, `Release`, no OS matrix. The extra configuration is the nightly clang `Release` build. It does not run on pull requests.

### Moving the build off GitHub-hosted runners

Set the repository variable `TIER_A_RUNNER` to one runner label. Leave it unset to keep `ubuntu-latest`. The variable is a single label string, not a JSON array. `CI / Lint` stays on `ubuntu-latest` so the lint input to `ci-ok` does not depend on a self-hosted pool.

Do not point `TIER_A_RUNNER` at a label that has no runner. The build job waits until `timeout-minutes` (15) and then fails.

### Compiler cache

The build job installs `ccache` and restores `~/.cache/ccache` with `actions/cache`. The key is `ccache-gcc-release-` plus a hash of the C++ sources and CMake files. CMake is configured with `-DCMAKE_CXX_COMPILER_LAUNCHER=ccache`. There is no CMake file change for this; the flag is on the workflow command line.

### Artifacts

The build job uploads `build/junit.xml` from `ctest --output-junit`, plus the perf summary and compare JSON. Retention is 7 days. The nightly clang job uploads its own JUnit file.

## Merge gate

`main` does not use branch protection. The only hard gate is the aggregate job `CI / ci-ok` in the `CI` workflow (ADR-0006). Do not enable branch protection or a ruleset, and do not mark `CI / Lint` or `CI / Build and test` as required checks.

`ci-ok` runs with `if: always()` after `lint` and `build-test`. It passes only when lint is `success` and the build is `success` or `skipped`. It fails when lint is skipped or either job fails, including a per-commit iteration or memory-gate failure in the build job. Drafts and docs-only pulls skip the build, so `ci-ok` still passes if lint passed. A code push to `main` runs the build.

A later mesh job (A4) may be added to `ci-ok`'s `needs` the same way, counting `skipped` as passing. Do not give Mesh its own required check.

## Tier B

File: `.github/workflows/nightly-mpi-gpu.yml`.

Schedule: `0 18 * * *` (18:00 UTC every day). That is **02:00 Asia/Shanghai** on the following calendar day. China standard time is UTC+8 and does not observe daylight saving.

Triggers besides the schedule:

- `workflow_dispatch`
- `pull_request` type `labeled`, and the label is `run-heavy`

Other labels start the workflow and then skip every job, which does not consume a runner.

### Gate

Tier B jobs do not start unless the repository variable `TIER_B_ENABLED` is the string `true`. A skipped job is not billed and does not request a self-hosted runner.

No cloud vendor is selected. The workflows contain no cloud credentials, no provider CLI, and no cloud login action.

When `run-heavy` is applied and `TIER_B_ENABLED` is not `true`, one short hosted job (`Tier B disabled`) prints that fact and exits. It does not provision anything.

### Runner labels

Override a label list with a repository variable set to a JSON array. Unset means the default in the workflow.

| Job | Default `runs-on` | Variable |
| --- | --- | --- |
| MPI (more than 2 ranks) | `[self-hosted, linux, x64, mpi]` | `TIER_B_MPI_RUNS_ON` |
| GPU | `[self-hosted, linux, gpu]` | `TIER_B_GPU_RUNS_ON` |
| Perf baseline | `[self-hosted, linux, x64]` | `TIER_B_PERF_RUNS_ON` |

The hosted weekday job `Nightly MPI communication` (`.github/workflows/nightly-mpi.yml`) builds with `-DCHIPCAE_ENABLE_MPI=ON` and runs `par_mpi_2` / `par_mpi_4`, which call `Communicator::allreduce_sum` and `deterministic_allreduce_sum` under `mpirun`. That is the real MPI communication path. The Tier B MPI job remains a self-hosted skeleton for ranks above the hosted quota and still does not block a merge.

The GPU job does not launch a device binary. Daily CUDA on the hosted nightly remains compile-only (`nvcc -c bench/cuda/smoke.cu`). The weekday `Nightly CUDA Kokkos core compile` job additionally builds Kokkos with CUDA and compiles `chipcae_core` plus the core/par tests so the A3 pinned-staging `static_assert` is checked. Those binaries are not executed. This CUDA Kokkos path has not been compiled on the development machine (no nvcc); the first scheduled run is the verification.

### Enabling Tier B later

1. Attach ephemeral runners that carry the labels above. The vendor choice stays outside this repo.
2. Set `TIER_B_ENABLED` to `true`.
3. Optionally replace the JSON label arrays.
4. Add the `run-heavy` label to a pull request, or wait for 18:00 UTC.

## Performance gate

Defaults live in `bench/perf_config.yaml`. The unit tests lock those numbers. The harness and the compare script are `tools/perf/harness.py` and `tools/perf/compare_baseline.py`. They do not link into the solvers.

The hosted pull-request job runs one microbenchmark: a standalone 3D Poisson conjugate-gradient solve (`bench/poisson_microbench`, grid 48, 110,592 unknowns) built by `bench/Makefile`, not by the chipcae CMake build. It solves to a linear relative residual of at most `1e-12`. Newton iteration count is 1 because the problem is linear. End-to-end CPU, end-to-end GPU, and scaling kinds are configured and covered by tests; the binary reports end-to-end kinds as not implemented. Scaling, when a Tier B job runs it, keeps the faster of two runs.

### Repetitions

| Kind | Warm-up (discarded) | N | Aggregate |
| --- | --- | --- | --- |
| Microbenchmark | 1 | 7 | Median |
| End-to-end CPU | 0 | 3 | Median |
| End-to-end GPU | 1 | 3 | Median |
| Scaling | 0 | 2 | The faster run (lower wall time), and the other fields from that same run |

Each run's JSON has wall time split into mesh, assembly, solve, and IO, plus DOF/s, linear and Newton iteration counts, and peak memory (host RSS and GPU bytes). The summary reports the median and the median absolute deviation (MAD) of the repeats.

### Noise

If the coefficient of variation of wall time is above 5%, the harness repeats the set once. If it is still above 5%, that timing metric is **inconclusive**. An inconclusive metric does not block. The compare step writes `inconclusive=true` to the job output. Hosted pull requests do not open an issue for that flag (it would fire on ordinary runner noise). The Tier B perf job's output is the hook for a later step that opens an issue. Iteration counts and peak memory are judged on their own scatter; a noisy timer does not erase an iteration or memory regression.

### Baseline

The reference is the median of the last K successful samples with the same runner label and build config.

- Nightly: K = 10
- Per-commit: K = 20

Samples live in `bench/baselines/`. The file shipped here has an empty sample list, so the first compares report `no_baseline` and do not block.

Baselines are never rewritten by CI. `tools/perf/append_baseline.py` refuses to run when `CI` is set. A change to a baseline file that already exists on the base branch fails `CI / Lint` unless the pull request has the label `perf-baseline-update`. Adding a new baseline file does not need the label.

An improvement larger than 10% sets `improvement_hint` and does not fail the job.

### Thresholds

Per-commit, the only blocking metrics are:

| Metric | Warn | Block |
| --- | --- | --- |
| Newton and linear iteration counts | above 2% | above 10% |
| Peak memory (host RSS and GPU bytes) | above 5% | above 10% |

Wall time and DOF/s are informational on every per-commit compare, including Tier B. The compare report still records whether wall time is above 10% **and** outside the baseline median plus 3×MAD (`legacy_would_block`). That flag does not fail the job.

Hosted Tier A timing never blocks and never sets the next-day hold.

Nightly, on Tier B (`--tier b --profile nightly`):

| Metric | Warn | Effect above the ceiling |
| --- | --- | --- |
| Wall time or DOF/s | above 5% | Above 15%, status `block_next_day`. The process still exits 0 |
| Iteration counts and peak memory | same as per-commit | Exit 1. This fails the Tier B perf job |

`block_next_day` is a hold signal for the **next day's merges**, not a red X on the nightly job. It is written to the job output and to `build/perf-compare.json`. Hosted runs never set it. It is not an automatic merge gate. Treat a Tier B nightly run whose output `block_next_day` is `true` as a hold: do not merge the following day until the regression is explained or accepted with `perf-baseline-update`.

## CPU / GPU consistency

`tools/consistency/compare.py` compares two JSON records. Bitwise equality is not required.

A case is eligible only when both sides report:

- linear relative residual ≤ `1e-12` (a direct solver may report 0)
- Newton tolerance ≤ `1e-10`

On an eligible pair the compare blocks when any of these fail:

- warpage relative difference > `1e-6`
- displacement relative L2 difference > `1e-6`
- max absolute integration-point stress difference > `1e-6` × max `|stress|`

This is a correctness gate. Any failure exits 1. The pull-request lint job runs the synthetic unit tests. There is no GPU execution on a hosted runner.

## Usage alert

`.github/workflows/actions-usage.yml` runs at **01:00 UTC** (09:00 Asia/Shanghai) and on manual dispatch.

`GITHUB_TOKEN` cannot read Actions billing. The job calls the billing API only when the secret `ACTIONS_BILLING_TOKEN` is set. This change does not create that secret. Without it, the job prints a skip line and exits 0.

The token has to be able to read the owner billing endpoint:

- `GET /orgs/{owner}/settings/billing/actions`, or
- `GET /users/{owner}/settings/billing/actions`

A classic personal access token needs the `user` or `admin:org` scope that can read that endpoint. A fine-grained token needs read access to Actions billing for the owner. Store it as the Actions secret `ACTIONS_BILLING_TOKEN`. Do not put it in the workflow file.

The API returns the **account** total for the billing period, not this repository alone. The alert treats that total as the number that must stay inside the 1,500-minute chipcae budget.

At 80% of the budget (1,200 minutes, unless `ACTIONS_MINUTE_BUDGET` or `ACTIONS_USAGE_ALERT_FRACTION` is set) the job opens or updates an issue titled `[actions-usage] Hosted Actions minutes are past 80% of the chipcae budget`.

## Who can start a run

Pushes by the Cursor agent identity (`cursoragent` / `cursor[bot]`) are not a reliable way to start Actions on this repository. A push by `dzw05` is what reliably starts the workflows. The cold and nightly runs below exist because `dzw05` pushed empty commit `c30a289`; GitHub then ran the workflows for `cbb8b5b`. Commit `c3d65bc` (the ccache path fix, pushed by `cursoragent`) has no Actions run. A later `cursor[bot]` pull-request run did complete on `fbb160e` (run 43900495478). Do not read a missing check on an agent push as a passing or failing build.

## Monthly estimate

Billed minutes are the job duration rounded up to the next minute. A skipped job costs 0. Drafts still run lint (about 11 seconds) and skip the build. Docs-only pulls do not start the build. Pushes to `main` do not build.

### Measured job time

Both workflows succeeded. Times are UTC, from `gh run view --json jobs` on 2026-11-16.

- CI: run [43899064936](https://github.com/dzw05/chipcae/actions/runs/43899064936), head `cbb8b5b`.
- Measure hosted nightly: run [43899065127](https://github.com/dzw05/chipcae/actions/runs/43899065127), same head. That workflow has since been removed so later pull requests do not compile CUDA.

| Job | Run | Wall time | Billed minutes (ceil) | Cache |
| --- | --- | --- | --- | --- |
| `CI / Lint` | 43899064936 | 15:41:24→15:41:33, 9 s | 1 | pip cache miss |
| `CI / Build and test` | 43899064936 | 15:41:37→15:42:27, 50 s | 1 | cold. `actions/cache` reported no `~/.cache/ccache` entry and did not save one |
| `CI / Build and test` | [43900495478](https://github.com/dzw05/chipcae/actions/runs/43900495478) | 15:49:12→15:49:51, 39 s | 1 | warm. ccache 21/21 hits, 0 misses. Head `fbb160e` |
| Nightly clang | 43899065127 | 39 s | 1 | cold |
| Nightly CUDA compile | 43899065127 | 15:41:25→15:44:26, about 3 min 1 s | 4 | `nvcc -c` only, not executed |
| Actions usage | 43899065127 | 7 s | 1 | no billing token, skip path |

The cold build is already at the 1-minute billing floor (50 s rounds up to 1). The warm rebuild is faster on the clock (39 s, 100% ccache hits) and still bills 1 minute.

### Assumptions (updated 2026-11-17)

- 22 workdays in a 30-day month, 8 weekend days.
- **2 code pull requests per workday.** Each runs Lint and Build and test.
- **2–3 code pushes to `main` per workday** also run Build and test (concurrency cancels superseded runs). Docs-only pushes do not start the workflow.
- Every calendar day: nightly clang, nightly CUDA smoke (`nvcc -c bench/cuda/smoke.cu`), and the usage check, each skipped when main has had no commits in the last 24 hours.
- Beijing workdays only (cron `0 18 * * 0-4`): hosted MPI communication tests and CUDA Kokkos core compile, also skipped when main is unchanged in the last 24 hours. Remeasure billed minutes after the first green CUDA Kokkos run. Do not delete the daily smoke.cu job until that first green run.
- Tier B stays off, so it adds 0 hosted minutes.
- PR Build and test is billed at 3 minutes (measured 2:26 on PR #19).

| Item | Times / month | Billed minutes each | Total |
| --- | --- | --- | --- |
| PR build-test | 44 | 3 | 132 |
| Lint (PR + push) | 66 | 1 | 66 |
| Daily clang | 30 (often skipped) | 2 | 60 |
| Daily CUDA smoke | 30 (often skipped; drop after first green Kokkos CUDA run, saving ~120) | 4 | 120 |
| Daily usage check | 30 | 1 | 30 |
| Weekday MPI | 22 | 3 | 66 |
| Weekday CUDA Kokkos (hot cache ~8 min; 1–2 cold starts at 30–40 min amortized) | 22 | ~10 | ~220 |
| Main build-test (gross 130–200; net ~60–130 after cache reuse and nightly dedup) | ~44–66 | 3 | net ~60–130 |
| **Total after this change** | | | **about 650–830** |
| After dropping the daily smoke.cu job | | | **about 530–710** |

650–830 minutes stays under the 1,200-minute warning line and the 2,000-minute free allowance (the project budget is 1,500). The risk is a cold CUDA Kokkos build hitting the 40-minute timeout; the cache key and separate ccache entry are there to keep the hot path short. Re-measure after the first green CUDA Kokkos run.

When a writer drops an event-log tail it writes `<path>.corrupt-<offset>`, or `<path>.corrupt-<offset>-<n>` if that name already exists. The library does not rotate or delete these files; the operator cleans them up.

## What is intentionally out of this tree

- The Poisson binary is built with `bench/Makefile`, not the chipcae CMake build. The branch also merges `main`, which has its own `cmake/` and `core/` changes.
- FCBGA and 2.5D benchmark specs are not added. A later change will land them.
- No Ansys project files, APDL, or comparison results.
- Open-source reference solvers are not dependencies and are not linked.
- No cloud account, runner image, or credential.
