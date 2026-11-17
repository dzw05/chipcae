"""Solve phase plane-stress compliance and zero resultant using C++ phase data."""
import io
import subprocess
import sys

import numpy as np

rows = np.loadtxt(io.StringIO(subprocess.check_output([sys.argv[1], "reference"], text=True)))
maximum = 0.0
print("pair T phi old_alpha_z new_alpha_z delta_z/% old_G new_G delta_G/% (default T_sf)")
for pair, temperature, override, f, em, nm, ec, nc, am, ac, actual_in, actual_z, g in rows:
    # Unknowns: common strain, phase in-plane stresses, phase z strains.
    # Isotropic compliance with sigma_x=sigma_y and sigma_z=0.
    system = np.array([
        [1, -(1-nm)/em, 0, 0, 0],
        [1, 0, -(1-nc)/ec, 0, 0],
        [0, 1-f, f, 0, 0],
        [0, 2*nm/em, 0, 1, 0],
        [0, 0, 2*nc/ec, 0, 1],
    ])
    strain, _, _, zm, zc = np.linalg.solve(system, [am, ac, 0, am, ac])
    expected = np.array([strain, (1-f)*zm + f*zc])
    error = np.abs(np.array([actual_in, actual_z])-expected) / np.maximum(np.abs(expected), 1e-30)
    maximum = max(maximum, error.max())
    assert np.all(error <= 1e-12), (pair, temperature, f, error)
    if f == 0.5 and override == 0:
        old_in = ((1-f)*em*am + f*ec*ac)/((1-f)*em+f*ec)
        old_z = (1-f)*(1+nm)*am + f*(1+nc)*ac - ((1-f)*nm+f*nc)*old_in
        old_g = (1-f)*em/(2*(1+nm)) + f*ec/(2*(1+nc))
        print(f"{'Cu/PI' if pair else 'Cu/FR4'} {temperature:g} {f:g} {old_z/(temperature-300):.9g} {actual_z/(temperature-300):.9g} {100*(actual_z/old_z-1):.6f} {old_g:.9g} {g:.9g} {100*(g/old_g-1):.6f}")
assert len(rows) == 132
print(f"cases={len(rows)} maximum relative thermal error={maximum:.3g}")
