# CI, performance gate, and hosted-minute budget

Linux only. There is no macOS or Windows job.

The hosted budget for this private repository is **1,500 minutes per month**. GitHub's Free plan includes 2,000 minutes; the remaining 500 minutes are headroom and are not part of this budget. The estimate below is the planning number for chipcae. It is not a GitHub invoice.

Repository settings (branch protection, variables, secrets) are not changed by the workflows. Names to set by hand are listed below.

## What runs where

| Workflow | When | Runner | Blocks a merge? |
| --- | --- | --- | --- |
| `CI / Lint` | Non-draft pull request (`opened`, `synchronize`, `ready_for_review`, `reopened`) and push to `main` | Always `ubuntu-latest` | This is the required check |
| `CI / Build and test` | Non-draft pull request that changes something other than docs | `vars.TIER_A_RUNNER` or `ubuntu-latest` | No. Do not mark it required |
| `Tier B and nightly / Nightly clang build` | 18:00 UTC daily, or manual dispatch | `ubuntu-latest` | No |
| `Tier B and nightly / Nightly CUDA compile` | Same nightly schedule. `nvcc -c` only, the object is not run | `ubuntu-latest` | No |
| `Tier B and nightly / Hosted minutes` is a different workflow | See usage alert | `ubuntu-latest` | No |
| Tier B MPI, GPU, perf | Nightly, manual dispatch, or a pull request labeled `run-heavy`, and only when `TIER_B_ENABLED` is `true` | Self-hosted labels below | Only after runners exist |

Push to `main` does **not** build or test again. Merge already ran the pull-request build.

Draft pull requests skip every job (the job `if` is false, so GitHub does not bill them). Marking a draft ready fires `ready_for_review` and runs the checks.

Concurrency group `ci-<workflow>-<ref>` uses `cancel-in-progress`. A newer push cancels the older run on the same ref.

### Docs-only pulls

`CI / Lint` still runs. `CI / Build and test` is skipped when every changed file is `docs/**`, a `*.md` file, `LICENSE`, or `CODEOWNERS`. A skipped build is why that job must not be a required check: GitHub treats a missing required check as not passed.

### Single configuration

The pull-request build is one configuration: GCC, `Release`, no OS matrix. The extra configuration is the nightly clang `Release` build. It does not run on pull requests.

### Moving the build off GitHub-hosted runners

Set the repository variable `TIER_A_RUNNER` to one runner label. Leave it unset to keep `ubuntu-latest`. The variable is a single label string, not a JSON array. `CI / Lint` stays on `ubuntu-latest` so the required check does not depend on a self-hosted pool.

Do not point `TIER_A_RUNNER` at a label that has no runner. The build job waits until `timeout-minutes` (15) and then fails.

### Compiler cache

The build job installs `ccache` and restores `~/.cache/ccache` with `actions/cache`. The key is `ccache-gcc-release-` plus a hash of the C++ sources and CMake files. CMake is configured with `-DCMAKE_CXX_COMPILER_LAUNCHER=ccache`. There is no CMake file change for this; the flag is on the workflow command line.

### Artifacts

The build job uploads `build/junit.xml` from `ctest --output-junit`, plus the perf summary and compare JSON. Retention is 7 days. The nightly clang job uploads its own JUnit file.

## Required status check

In branch protection (or a ruleset) for `main`, mark this check required:

- `CI / Lint`

Do not mark required:

- `CI / Build and test`
- `Tier B and nightly / ...`
- `Actions usage / Hosted minutes`

`CI / Lint` is the job named `Lint` in the workflow named `CI`. The checks UI shows that pair as `CI / Lint`.

How to set it, without this change doing it for you:

1. Repository **Settings → Branches** (or **Rules → Rulesets**).
2. Edit the rule for `main`.
3. Enable **Require status checks to pass**.
4. Add `CI / Lint`.
5. Leave `CI / Build and test` off the list.

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

The MPI job does not launch `mpirun` until `build/chipcae_test_core` exists. The communicator in `core/src/parallel.cpp` is still a serial stub, and this change does not modify it. When that path is real, the job should run `mpirun -np 4` on a Tier B runner. Rank 2 is not part of the hosted pull-request job.

The GPU job does not launch a device binary. CUDA on the hosted nightly is compile-only (`nvcc -c bench/cuda/smoke.cu`).

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

`block_next_day` is a hold signal for the **next day's merges**, not a red X on the nightly job. It is written to the job output and to `build/perf-compare.json`. Hosted runs never set it. Branch protection does not read it until someone marks a check that publishes it. Treat a Tier B nightly run whose output `block_next_day` is `true` as a hold: do not merge the following day until the regression is explained or accepted with `perf-baseline-update`.

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

## Monthly estimate

Billed minutes are the job duration rounded up to the next minute. A skipped job costs 0. Drafts are skipped. Docs-only pulls do not start the build. Pushes to `main` do not build.

### Measured job time

No GitHub-hosted job has run on this branch, so there is no duration to round up. Do not treat the cells below as measurements.

What was queried on 2026-11-16, after merging `main` into the branch (`cbb8b5b`, pull request mergeable and clean):

- `gh run list` for `cursor/k7-ci-perf-gate-1e60` returned no runs. `actions/runs?head_sha=` for `0303604` and `cbb8b5b` returned `total_count: 0`.
- The commit status is `pending` with an empty status list. The only check suite is the `cursor` app, stuck `queued` since the push, with zero check runs. There are no job logs.
- Every Actions run currently visible on the repository was triggered by `dzw05`. `actions/runs?actor=cursoragent` is empty. These commits are authored and pushed by `cursoragent`, which did not start Actions. The first push also could not run: the pull request conflicted with `main`, so GitHub had no merge ref.
- Nothing is `queued`, `waiting`, or `action_required` in Actions.

| Job | Wall time | Billed minutes (ceil) | Cache |
| --- | --- | --- | --- |
| `CI / Lint` | not run | — | — |
| `CI / Build and test` | not run | — | cold ccache not observed |
| `CI / Build and test` | not run | — | warm ccache not observed |
| Nightly clang | not run | — | — |
| Nightly CUDA compile | not run | — | — |
| Actions usage | not run | — | — |

A push or workflow re-run by an actor that Actions accepts (the runs above used `dzw05`) is what produces the missing numbers. The one-shot measurement workflow was removed so that a later push does not also compile CUDA on every pull-request update.

### Assumptions

Planning rate for the monthly total, once billed minutes exist:

- 22 workdays in a 30-day month, 8 weekend days.
- **2 code pull requests per workday.** Each runs Lint and Build and test (4 jobs).
- **1 push to `main` per workday**, Lint only (1 job).
- Every calendar day: nightly clang, nightly CUDA compile, and the usage check (3 jobs).
- That is **8 jobs on a workday** (4 + 1 + 3). Weekend days are the 3 nightly jobs only.
- Tier B stays off, so it adds 0 hosted minutes. Drafts are not an extra line. The 44 builds use the warm-cache billed minute; the cold-cache build is reported beside it and is not multiplied.

```
builds = 22 * 2 = 44
lints  = 22 * 2 + 22 = 66
nightly jobs = 30 clang + 30 cuda + 30 usage

monthly = 44 * billed(build, warm)
        + 66 * billed(lint)
        + 30 * billed(clang)
        + 30 * billed(cuda)
        + 30 * billed(usage)
```

The numeric total is **not computed**. There are no billed minutes to multiply, so this document does not claim the sum is under 1,500. The cap remains 1,500. Fill the table from `gh run view --json jobs` and evaluate the formula before treating the budget as checked. The usage issue still fires at 1,200 account minutes.

## What is intentionally out of this tree

- No edits under `CMakeLists.txt`, `cmake/`, or `core/`. The Poisson binary is built with `bench/Makefile`.
- FCBGA and 2.5D benchmark specs are not added. A later change will land them.
- No Ansys project files, APDL, or comparison results.
- Open-source reference solvers are not dependencies and are not linked.
- No cloud account, runner image, or credential.
