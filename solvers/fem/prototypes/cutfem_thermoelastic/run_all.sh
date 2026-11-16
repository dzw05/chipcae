#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
SECONDS=0
with_hypre=OFF
if [[ ${1:-} == --with-hypre ]]; then
  with_hypre=ON
  # This driver is strictly one rank; self transport avoids unused network probes.
  export OMPI_MCA_pml=ob1 OMPI_MCA_btl=self OMPI_MCA_coll=^ucc
  shift
fi
if [[ $# -ne 0 ]]; then
  echo "Usage: $0 [--with-hypre]" >&2
  exit 2
fi
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export MPLCONFIGDIR="$PWD/build/matplotlib"
export LD_LIBRARY_PATH="/workspace/tools/env/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release -DCUTFEM_WITH_HYPRE="$with_hypre" -DMPI_CXX_COMPILER=/workspace/tools/env/bin/mpicxx
cmake --build build -j4
mkdir -p results
./build/cutfem_proto unit_tests | tee results/unit_tests.log
for experiment in mms_thermal mms_thermal_bimat mms_elastic timoshenko gp_sweep; do
  ./build/cutfem_proto "$experiment" --output results | tee "results/$experiment.log"
done
python3 - "$with_hypre" "$SECONDS" <<'PYINFO'
import json, sys
from pathlib import Path
Path('results/run_info.json').write_text(json.dumps({
    'hypre': sys.argv[1] == 'ON', 'seconds_before_plot': int(sys.argv[2]),
    'precision': 'FP64', 'build_type': 'Release -O2', 'build_jobs': 4,
    'experiment_processes': 1, 'gamma_N': 10, 'gamma_g_default': 0.1,
}, indent=2) + '\n')
PYINFO
python3 scripts/plot.py --results results
(cd /workspace/chipcae && python3 cmake/check_license_policy.py) | tee results/license_policy.log
echo "run_all.sh succeeded (hypre=$with_hypre, elapsed ${SECONDS}s)"
