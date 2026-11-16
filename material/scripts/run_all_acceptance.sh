#!/usr/bin/env bash
# Every report metric comes from a completed test, never from a placeholder.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD_DIR:-${ROOT}/build-cpu}"
REPORT="${REPORT_PATH:-${ROOT}/acceptance_report.json}"
export OMP_NUM_THREADS="${OMP_NUM_THREADS:-4}"
export OMP_PROC_BIND="${OMP_PROC_BIND:-false}"
bash "${ROOT}/scripts/build_cpu.sh" -DCHIPCAE_MATERIAL_ENABLE_PYTHON=OFF
set +e
ctest --test-dir "${BUILD}" --output-on-failure --output-junit "${BUILD}/acceptance_junit.xml"
status=$?
set -e
python3 "${ROOT}/tools/acceptance_report.py" "${BUILD}/acceptance_junit.xml" "${REPORT}" "${status}"
exit "${status}"
