"""Call the native JSON validator CLI; Python development headers are unnecessary."""
import os
import pathlib
import subprocess
root = pathlib.Path(__file__).resolve().parents[2]
exe = pathlib.Path(os.environ.get('K3_CARD_CHECK', root/'build-cpu/material_card_check'))
fixture = root/'tests/negative/cte_missing.json'
result = subprocess.run([str(exe), str(fixture)], text=True, capture_output=True,
                        env=dict(os.environ, OMP_NUM_THREADS='1', OMP_PROC_BIND='false'))
assert result.returncode == 1 and 'cte_type' in result.stderr, result
print(result.stderr.strip())
