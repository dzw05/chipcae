"""Minimal fixed-tau Lawson-Hanson NNLS. Synthetic illustrative SI data only."""
import json
import numpy as np
from scipy.optimize import nnls


def matrix(omega, tau):
    x = np.outer(omega, tau)
    return np.vstack((np.column_stack((np.ones(len(omega)), x*x/(1+x*x))),
                      np.column_stack((np.zeros(len(omega)), x/(1+x*x)))))


def fit(omega, tau, storage, loss):
    a = matrix(omega, tau)
    y = np.r_[storage, loss]
    weights = 1/np.maximum(np.abs(y), 1e-30)
    return nnls(a*weights[:, None], y*weights)[0]


def main():
    seed = 20261116
    tau = np.array([.01, .1, 1., 10.])
    moduli = np.array([2e9, 1e9, 3e9, 2e9, 4e9])
    omega = np.logspace(-3, 4, 150)
    worst = 0.
    shifts = []
    for T in [280., 300., 320., 340.]:
        loga = 30000/(8.314462618*np.log(10))*(1/T-1/300.)
        reduced = omega*10**loga
        y = matrix(reduced, tau) @ moduli
        fitted = fit(reduced, tau, y[:len(omega)], y[len(omega):])
        worst = max(worst, float(np.linalg.norm(matrix(reduced, tau) @ fitted-y)/np.linalg.norm(y)))
        # Recover the horizontal factor independently by bounded scalar fitting.
        from scipy.optimize import least_squares
        residual = lambda z: (matrix(omega*10**z[0], tau) @ fitted-y)/y
        recovered = least_squares(residual, [0.], xtol=1e-14, ftol=1e-14, gtol=1e-14).x[0]
        shifts.append(abs(recovered-loga))
    worst = max(worst, max(shifts))
    assert worst <= 1e-6, (seed, tau, moduli, worst)
    print('METRIC '+json.dumps(dict(id='T14', measured_error=worst, tolerance=1e-6, seed=seed)))


if __name__ == '__main__':
    main()
