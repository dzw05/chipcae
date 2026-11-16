#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
SECONDS=0
with_hypre=OFF
with_kokkos=OFF
for arg in "$@"; do
  case "$arg" in
    --with-hypre) with_hypre=ON ;;
    --with-kokkos) with_kokkos=ON ;;
    *) echo "Usage: $0 [--with-hypre] [--with-kokkos]" >&2; exit 2 ;;
  esac
done
if [[ $with_hypre == ON ]]; then
  export OMPI_MCA_pml=ob1 OMPI_MCA_btl=self OMPI_MCA_coll=^ucc
fi
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export MPLCONFIGDIR="$PWD/build/matplotlib"
export LD_LIBRARY_PATH="/workspace/tools/env/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release -DCUTFEM_WITH_HYPRE="$with_hypre" -DCUTFEM_WITH_KOKKOS="$with_kokkos" -DKokkos_DIR=/workspace/deps/kokkos/lib/cmake/Kokkos -DMPI_CXX_COMPILER=/workspace/tools/env/bin/mpicxx
cmake --build build -j4
mkdir -p results
./build/cutfem_proto unit_tests | tee results/unit_tests.log
for experiment in mms_thermal mms_thermal_bimat mms_elastic timoshenko gp_sweep; do
  extra=()
  [[ "$experiment" == gp_sweep ]] && extra=(--dump-matrices)
  ./build/cutfem_proto "$experiment" --output results "${extra[@]}" | tee "results/$experiment.log"
done
if [[ $with_kokkos == ON ]]; then
  echo 'nx,ny,ndof,backend,threads,volume_seconds,total_seconds,load_average_1min' > results/kokkos_bench.csv
  for size in '960 40' '2048 64'; do
    read -r nx ny <<< "$size"
    for mode in host serial; do
      OMP_NUM_THREADS=1 ./build/kokkos_bench "$nx" "$ny" "$mode" >> results/kokkos_bench.csv
    done
    for threads in 1 2 4; do
      OMP_NUM_THREADS="$threads" ./build/kokkos_bench "$nx" "$ny" openmp >> results/kokkos_bench.csv
    done
  done
fi
python3 - "$with_hypre" "$SECONDS" "$with_kokkos" <<'PYINFO'
import json, sys
from pathlib import Path
Path('results/run_info.json').write_text(json.dumps({
    'hypre': sys.argv[1] == 'ON', 'seconds_before_plot': int(sys.argv[2]),
    'kokkos': sys.argv[3] == 'ON',
    'precision': 'FP64', 'build_type': 'Release -O2', 'build_jobs': 4,
    'experiment_processes': 1, 'gamma_N': 20, 'gamma_g_default': 0.1,
}, indent=2) + '\n')
PYINFO
python3 scripts/spectrum.py --results results
python3 scripts/plot.py --results results
(cd /workspace/chipcae-cutfem && python3 cmake/check_license_policy.py) | tee results/license_policy.log
echo "run_all.sh succeeded (hypre=$with_hypre, kokkos=$with_kokkos, elapsed ${SECONDS}s)"
