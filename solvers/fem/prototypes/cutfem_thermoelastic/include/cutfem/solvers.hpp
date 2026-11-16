#pragma once
#include <string>
#include <utility>
#include <vector>
namespace chipcae::proto_cutfem {
struct CSRMatrix {
    int n = 0;
    std::vector<int> row, col;
    std::vector<double> val;
    static CSRMatrix pattern(std::vector<std::vector<int>> rows);
    void add(int i, int j, double v);
    void multiply(const std::vector<double>& x, std::vector<double>& y) const;
    double diagonal(int i) const;
    void constrain(std::vector<double>& rhs, const std::vector<std::pair<int, double>>& fixed);
};
struct SolverStats {
    int iterations = 0;
    double residual = 0, cond_est = 0;
    std::string status = "ok";
    bool indefinite = false;
};
struct LinearResult {
    std::vector<double> x;
    SolverStats stats;
};
LinearResult
banded_cholesky(const CSRMatrix&, const std::vector<double>&, bool allow_indefinite = false);
LinearResult conjugate_gradient(const CSRMatrix&,
                                const std::vector<double>&,
                                bool jacobi = true,
                                double tol = 1e-8,
                                int maxit = 20000);
LinearResult hypre_pcg(const CSRMatrix&, const std::vector<double>&, int ncomp = 1);
bool hypre_available();
double relative_residual(const CSRMatrix&, const std::vector<double>&, const std::vector<double>&);
} // namespace chipcae::proto_cutfem
