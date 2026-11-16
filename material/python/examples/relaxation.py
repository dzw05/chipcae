"""Pure Python analytic illustrative relaxation curve (SI); no native bindings."""
from math import exp
for t in [0., .1, 1., 10.]:
    shear_modulus = 2e9+3e9*exp(-t/.7)+1e9*exp(-t/3.)
    print(f'{t:g} s: shear stress={shear_modulus*.01:g} Pa for gamma=.01')
