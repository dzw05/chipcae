#!/bin/bash
# Also usable from /workspace/tasks with E7_REPO set to the worktree.
set -eu
repo=${E7_REPO:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)}
export PATH=/workspace/tasks/shim:$PATH
exec python3 "$repo/cmake/tests/ccache_acceptance.py" -v Acceptance.test_shim_ccache_env
