"""Pure Python illustration of tangent/secant CTE; no native module required."""
def thermal_strain(T, T0=300., alpha0=1e-5, slope=1e-7):
    x = T-T0
    return alpha0*x + .5*slope*x*x


def secant_cte(T, T0=300.):
    return 1e-5 if T == T0 else thermal_strain(T, T0)/(T-T0)


if __name__ == '__main__':
    for T in [300., 325., 350.]:
        assert abs(secant_cte(T)*(T-300)-thermal_strain(T)) < 1e-15
        print(f'{T:g} K: secant={secant_cte(T):g}/K, strain={thermal_strain(T):g}')
