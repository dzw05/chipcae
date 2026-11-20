#include "cutfem/solvers.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#ifdef CUTFEM_WITH_HYPRE
#include <HYPRE.h>
#include <HYPRE_IJ_mv.h>
#include <HYPRE_parcsr_ls.h>
#include <mpi.h>
#endif
namespace chipcae::proto_cutfem {
CSRMatrix CSRMatrix::pattern(std::vector<std::vector<int>> rows) {
    CSRMatrix A;
    A.n = rows.size();
    A.row.push_back(0);
    for (auto& r : rows) {
        std::sort(r.begin(), r.end());
        r.erase(std::unique(r.begin(), r.end()), r.end());
        A.col.insert(A.col.end(), r.begin(), r.end());
        A.row.push_back(A.col.size());
    }
    A.val.assign(A.col.size(), 0);
    return A;
}
void CSRMatrix::add(int i, int j, double v) {
    auto it = std::lower_bound(col.begin() + row[i], col.begin() + row[i + 1], j);
    if (it == col.begin() + row[i + 1] || *it != j)
        throw std::runtime_error("Missing sparse pattern");
    val[it - col.begin()] += v;
}
void CSRMatrix::multiply(const std::vector<double>& x, std::vector<double>& y) const {
    y.resize(n);
    for (int i = 0; i < n; ++i) {
        double v = 0;
        for (int j = row[i]; j < row[i + 1]; ++j)
            v += val[j] * x[col[j]];
        y[i] = v;
    }
}
double CSRMatrix::diagonal(int i) const {
    auto it = std::lower_bound(col.begin() + row[i], col.begin() + row[i + 1], i);
    return val[it - col.begin()];
}
void CSRMatrix::constrain(std::vector<double>& rhs,
                          const std::vector<std::pair<int, double>>& fixed) {
    std::vector<char> is(n, 0);
    std::vector<double> g(n, 0), diag(n);
    for (auto [i, v] : fixed) {
        if (i < 0 || i >= n)
            throw std::runtime_error("Invalid constraint");
        is[i] = 1;
        g[i] = v;
        diag[i] = diagonal(i);
    }
    for (int i = 0; i < n; ++i) {
        for (int j = row[i]; j < row[i + 1]; ++j) {
            int k = col[j];
            if (!is[i] && is[k])
                rhs[i] -= val[j] * g[k];
            if (is[i] || is[k])
                val[j] = 0;
        }
        if (is[i]) {
            add(i, i, diag[i]);
            rhs[i] = diag[i] * g[i];
        }
    }
}
namespace {
double inner(const std::vector<double>& a, const std::vector<double>& b) {
    double v = 0;
    for (size_t i = 0; i < a.size(); ++i)
        v += a[i] * b[i];
    return v;
}
// Extreme eigenvalues of the CG/Lanczos symmetric tridiagonal, by Sturm bisection.
// O(m) storage and O(80*m) work, even for a 20000-step run.
double condition_estimate(const std::vector<double>& alpha, const std::vector<double>& beta) {
    int n = alpha.size();
    if (!n)
        return std::numeric_limits<double>::quiet_NaN();
    std::vector<double> d(n), off(n, 0);
    double lo = 1e300, hi = -1e300;
    for (int i = 0; i < n; ++i) {
        d[i] = 1 / alpha[i] + (i ? beta[i - 1] / alpha[i - 1] : 0);
        if (i + 1 < n)
            off[i] = std::sqrt(std::max(0., beta[i])) / alpha[i];
    }
    for (int i = 0; i < n; ++i) {
        double r = (i ? std::abs(off[i - 1]) : 0) + std::abs(off[i]);
        lo = std::min(lo, d[i] - r);
        hi = std::max(hi, d[i] + r);
    }
    auto count = [&](double x) {
        int count = 0;
        double p = d[0] - x;
        const double tiny = 1e-280;
        if (p <= 0)
            ++count;
        for (int i = 1; i < n; ++i) {
            if (std::abs(p) < tiny)
                p = -tiny;
            p = d[i] - x - off[i - 1] * off[i - 1] / p;
            if (p <= 0)
                ++count;
        }
        return count;
    };
    auto eigen = [&](int index) {
        double a = lo, b = hi;
        for (int k = 0; k < 90; ++k) {
            double x = (a + b) / 2;
            if (count(x) <= index)
                a = x;
            else
                b = x;
        }
        return (a + b) / 2;
    };
    double l = eigen(0), u = eigen(n - 1);
    return l > 0 ? u / l : std::numeric_limits<double>::infinity();
}
} // namespace
double
relative_residual(const CSRMatrix& A, const std::vector<double>& b, const std::vector<double>& x) {
    std::vector<double> r;
    A.multiply(x, r);
    for (int i = 0; i < A.n; ++i)
        r[i] -= b[i];
    return std::sqrt(inner(r, r) / std::max(inner(b, b), 1e-300));
}
LinearResult
banded_cholesky(const CSRMatrix& A, const std::vector<double>& b, bool allow_indefinite) {
    int n = A.n, bw = 0;
    for (int i = 0; i < n; ++i)
        for (int j = A.row[i]; j < A.row[i + 1]; ++j)
            if (A.val[j] != 0)
                bw = std::max(bw, std::abs(i - A.col[j]));
    // Row-major lower band, diagonal at offset bw. LDL^T permits honest reporting
    // of indefinite GP-off MMS systems without silently changing the bilinear form.
    size_t stride = bw + 1;
    std::vector<double> L(size_t(n) * stride, 0), D(n), work(n);
    LinearResult out;
    out.x = b;
    for (int i = 0; i < n; ++i)
        for (int j = A.row[i]; j < A.row[i + 1]; ++j)
            if (A.col[j] <= i && i - A.col[j] <= bw)
                L[size_t(i) * stride + bw + A.col[j] - i] = A.val[j];
    for (int i = 0; i < n; ++i) {
        int start = std::max(0, i - bw);
        double* li = &L[size_t(i) * stride];
        for (int k = start; k < i; ++k)
            work[k] = li[bw + k - i] * D[k];
        for (int j = start; j < i; ++j) {
            double* lj = &L[size_t(j) * stride];
            double s = li[bw + j - i];
            int first = std::max(start, j - bw);
            for (int k = first; k < j; ++k)
                s -= work[k] * lj[bw + k - j];
            li[bw + j - i] = s / D[j];
            work[j] = s;
        }
        double di = li[bw];
        for (int k = start; k < i; ++k)
            di -= li[bw + k - i] * work[k];
        D[i] = di;
        if (!std::isfinite(di) || di == 0)
            throw std::runtime_error("Zero/nonfinite LDL pivot");
        if (di < 0) {
            out.stats.indefinite = true;
            if (!allow_indefinite)
                throw std::runtime_error("Banded LDL found indefinite matrix");
        }
    }
    for (int i = 0; i < n; ++i) {
        double s = out.x[i];
        for (int j = std::max(0, i - bw); j < i; ++j)
            s -= L[size_t(i) * stride + bw + j - i] * out.x[j];
        out.x[i] = s;
    }
    for (int i = 0; i < n; ++i)
        out.x[i] /= D[i];
    for (int i = n - 1; i >= 0; --i)
        for (int j = i + 1; j < std::min(n, i + bw + 1); ++j)
            out.x[i] -= L[size_t(j) * stride + bw + i - j] * out.x[j];
    out.stats.iterations = 1;
    out.stats.residual = relative_residual(A, b, out.x);
    out.stats.status = out.stats.indefinite ? "indefinite" : "ok";
    return out;
}
LinearResult conjugate_gradient(
    const CSRMatrix& A, const std::vector<double>& b, bool jacobi, double tol, int maxit) {
    LinearResult out;
    out.x.assign(A.n, 0);
    std::vector<double> r = b, z(A.n), p(A.n), Ap, inv(A.n), alpha, beta;
    double normb = std::sqrt(inner(b, b));
    if (normb == 0) {
        out.stats.cond_est = 1;
        return out;
    }
    for (int i = 0; i < A.n; ++i) {
        double d = A.diagonal(i);
        if (jacobi && d <= 0) {
            out.stats.status = "indefinite";
            out.stats.indefinite = true;
            out.stats.residual = 1;
            return out;
        }
        inv[i] = jacobi ? 1 / d : 1;
        z[i] = r[i] * inv[i];
    }
    p = z;
    double rz = inner(r, z);
    out.stats.status = "no_conv";
    for (int it = 0; it < maxit; ++it) {
        A.multiply(p, Ap);
        double pap = inner(p, Ap);
        if (pap <= 0 || !std::isfinite(pap)) {
            out.stats.status = "indefinite";
            out.stats.indefinite = true;
            break;
        }
        double a = rz / pap;
        alpha.push_back(a);
        for (int i = 0; i < A.n; ++i) {
            out.x[i] += a * p[i];
            r[i] -= a * Ap[i];
        }
        out.stats.iterations = it + 1;
        double resid = std::sqrt(inner(r, r)) / normb;
        if (resid < tol) {
            out.stats.residual = relative_residual(A, b, out.x);
            if (out.stats.residual < tol * 1.1) {
                out.stats.status = "ok";
                break;
            }
        }
        for (int i = 0; i < A.n; ++i)
            z[i] = r[i] * inv[i];
        double rznew = inner(r, z), be = rznew / rz;
        beta.push_back(be);
        for (int i = 0; i < A.n; ++i)
            p[i] = z[i] + be * p[i];
        rz = rznew;
    }
    out.stats.residual = relative_residual(A, b, out.x);
    out.stats.cond_est = out.stats.indefinite ? std::numeric_limits<double>::quiet_NaN()
                                              : condition_estimate(alpha, beta);
    return out;
}
bool hypre_available() {
#ifdef CUTFEM_WITH_HYPRE
    return true;
#else
    return false;
#endif
}
LinearResult hypre_pcg(const CSRMatrix& A, const std::vector<double>& b, int ncomp) {
#ifndef CUTFEM_WITH_HYPRE
    (void)A;
    (void)b;
    (void)ncomp;
    LinearResult out;
    out.stats.status = "unavailable";
    return out;
#else
    static_assert(sizeof(HYPRE_Int) == 4, "This prototype expects 32-bit hypre integers");
    HYPRE_ClearAllErrors();
    LinearResult out;
    out.x.assign(A.n, 0);
    HYPRE_IJMatrix M;
    HYPRE_IJVector rhs, sol;
    HYPRE_ParCSRMatrix parM;
    HYPRE_ParVector parR, parX;
    HYPRE_IJMatrixCreate(MPI_COMM_WORLD, 0, A.n - 1, 0, A.n - 1, &M);
    HYPRE_IJMatrixSetObjectType(M, HYPRE_PARCSR);
    std::vector<HYPRE_Int> sizes(A.n);
    for (int i = 0; i < A.n; ++i)
        sizes[i] = A.row[i + 1] - A.row[i];
    HYPRE_IJMatrixSetRowSizes(M, sizes.data());
    HYPRE_IJMatrixInitialize(M);
    std::vector<HYPRE_BigInt> columns(A.col.begin(), A.col.end()), ids(A.n);
    std::iota(ids.begin(), ids.end(), 0);
    for (int i = 0; i < A.n; ++i)
        HYPRE_IJMatrixSetValues(
            M, 1, &sizes[i], &ids[i], columns.data() + A.row[i], A.val.data() + A.row[i]);
    HYPRE_IJMatrixAssemble(M);
    HYPRE_IJMatrixGetObject(M, (void**)&parM);
    auto makevec = [&](HYPRE_IJVector& v, const double* data, HYPRE_ParVector& par) {
        HYPRE_IJVectorCreate(MPI_COMM_WORLD, 0, A.n - 1, &v);
        HYPRE_IJVectorSetObjectType(v, HYPRE_PARCSR);
        HYPRE_IJVectorInitialize(v);
        HYPRE_IJVectorSetValues(v, A.n, ids.data(), data);
        HYPRE_IJVectorAssemble(v);
        HYPRE_IJVectorGetObject(v, (void**)&par);
    };
    makevec(rhs, b.data(), parR);
    makevec(sol, out.x.data(), parX);
    HYPRE_Solver amg, pcg;
    HYPRE_BoomerAMGCreate(&amg);
    HYPRE_BoomerAMGSetPrintLevel(amg, 0);
    HYPRE_BoomerAMGSetCoarsenType(amg, 10);
    HYPRE_BoomerAMGSetInterpType(amg, 6);
    HYPRE_BoomerAMGSetRelaxType(amg, 6);
    HYPRE_BoomerAMGSetNumSweeps(amg, 1);
    HYPRE_BoomerAMGSetStrongThreshold(amg, ncomp == 2 ? .5 : .25);
    HYPRE_BoomerAMGSetTol(amg, 0);
    HYPRE_BoomerAMGSetMaxIter(amg, 1);
    HYPRE_BoomerAMGSetNumFunctions(amg, ncomp);
    if (ncomp == 2)
        HYPRE_BoomerAMGSetNodal(amg, 1);
    HYPRE_ParCSRPCGCreate(MPI_COMM_WORLD, &pcg);
    HYPRE_ParCSRPCGSetTol(pcg, 1e-8);
    HYPRE_ParCSRPCGSetMaxIter(pcg, 20000);
    HYPRE_ParCSRPCGSetTwoNorm(pcg, 1);
    HYPRE_ParCSRPCGSetPrintLevel(pcg, 0);
    HYPRE_ParCSRPCGSetLogging(pcg, 1);
    HYPRE_ParCSRPCGSetPrecond(pcg,
                              (HYPRE_PtrToParSolverFcn)HYPRE_BoomerAMGSolve,
                              (HYPRE_PtrToParSolverFcn)HYPRE_BoomerAMGSetup,
                              amg);
    int err = HYPRE_ParCSRPCGSetup(pcg, parM, parR, parX);
    if (!err)
        err = HYPRE_ParCSRPCGSolve(pcg, parM, parR, parX);
    HYPRE_Int its = 0;
    HYPRE_ParCSRPCGGetNumIterations(pcg, &its);
    out.stats.iterations = its;
    HYPRE_IJVectorGetValues(sol, A.n, ids.data(), out.x.data());
    out.stats.residual = relative_residual(A, b, out.x);
    out.stats.status = (!err && out.stats.residual < 1.1e-8) ? "ok" : "no_conv";
    HYPRE_ParCSRPCGDestroy(pcg);
    HYPRE_BoomerAMGDestroy(amg);
    HYPRE_IJVectorDestroy(rhs);
    HYPRE_IJVectorDestroy(sol);
    HYPRE_IJMatrixDestroy(M);
    HYPRE_ClearAllErrors();
    return out;
#endif
}
} // namespace chipcae::proto_cutfem
