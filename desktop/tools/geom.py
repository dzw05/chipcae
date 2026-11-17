"""Deterministic SI cells and tensor Q2 interpolation (numpy only)."""
import numpy as np

CORNERS = np.array([[0,0,0],[1,0,0],[1,1,0],[0,1,0],
                    [0,0,1],[1,0,1],[1,1,1],[0,1,1]], dtype=float)
FACES = [[0,3,2,1], [4,5,6,7], [0,1,5,4], [1,2,6,5], [2,3,7,6], [3,0,4,7]]

def analytic(p, step):
    x, y, z = np.asarray(p).T
    t = 193.15 + 7*step + 3*x*x + 2*y*z + z*z
    u = np.column_stack((1e-4*x*z, 2e-4*y*z,
                         (step+1)*1e-3*(x*x + 2*y*y)))
    return t, u

def interpolate(p, step):
    """Interpolate all 27 Q2 DOFs; cut points are inside the parent unit cube."""
    nodes = np.array([[x,y,z] for x in (0,.5,1) for y in (0,.5,1) for z in (0,.5,1)])
    def basis(a):
        return np.column_stack((2*(a-.5)*(a-1), 4*a*(1-a), 2*a*(a-.5)))
    b = [basis(np.asarray(p)[:, i]) for i in range(3)]
    weights = np.column_stack([b[0][:,i]*b[1][:,j]*b[2][:,k]
                               for i in range(3) for j in range(3) for k in range(3)])
    t, u = analytic(nodes, step)
    return weights @ t, weights @ u

def volume(points, faces):
    """Oriented surface divergence, independent of VTK triangulation."""
    total = 0.
    for f in faces:
        p = points[f]
        for j in range(1, len(f)-1):
            total += np.dot(p[0], np.cross(p[j], p[j+1])) / 6
    return total
