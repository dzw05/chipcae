// Standalone Poisson microbenchmark. It does not link chipcae.
// Emits one JSON object on stdout: phase times, DOF/s, iterations, peak RSS.

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(__linux__)
#include <sys/resource.h>
#endif

namespace {

struct Options {
    std::string kind = "microbenchmark";
    int grid = 48;
    double linear_tolerance = 1e-12;
};

struct Csr {
    std::vector<int> row_ptr;
    std::vector<int> col;
    std::vector<double> val;
};

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

long peak_rss_bytes() {
#if defined(__linux__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        // Linux reports ru_maxrss in kilobytes.
        return static_cast<long>(usage.ru_maxrss) * 1024L;
    }
#endif
    return 0;
}

Options parse_args(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto need = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << flag << '\n';
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--kind") {
            options.kind = need("--kind");
        } else if (arg == "--grid") {
            options.grid = std::atoi(need("--grid"));
        } else if (arg == "--linear-tolerance") {
            options.linear_tolerance = std::atof(need("--linear-tolerance"));
        } else if (arg == "--help") {
            std::cout << "usage: poisson_microbench [--kind NAME] [--grid N] "
                         "[--linear-tolerance TOL]\n";
            std::exit(0);
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            std::exit(2);
        }
    }
    if (options.grid < 2) {
        std::cerr << "grid must be >= 2\n";
        std::exit(2);
    }
    return options;
}

int index_of(int n, int i, int j, int k) {
    return i + n * (j + n * k);
}

Csr assemble(int n, std::vector<double>& rhs) {
    const int dof = n * n * n;
    rhs.assign(static_cast<std::size_t>(dof), 1.0);
    Csr matrix;
    matrix.row_ptr.reserve(static_cast<std::size_t>(dof) + 1);
    matrix.col.reserve(static_cast<std::size_t>(dof) * 7);
    matrix.val.reserve(static_cast<std::size_t>(dof) * 7);
    matrix.row_ptr.push_back(0);
    for (int k = 0; k < n; ++k) {
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const int row = index_of(n, i, j, k);
                const int neighbors[6][3] = {
                    {i - 1, j, k},
                    {i + 1, j, k},
                    {i, j - 1, k},
                    {i, j + 1, k},
                    {i, j, k - 1},
                    {i, j, k + 1},
                };
                // Dirichlet Laplacian: diagonal stays 6. A missing neighbor is a
                // zero boundary value and does not change the unit right-hand side.
                for (const auto& neighbor : neighbors) {
                    const int ni = neighbor[0];
                    const int nj = neighbor[1];
                    const int nk = neighbor[2];
                    if (ni < 0 || nj < 0 || nk < 0 || ni >= n || nj >= n || nk >= n) {
                        continue;
                    }
                    matrix.col.push_back(index_of(n, ni, nj, nk));
                    matrix.val.push_back(-1.0);
                }
                matrix.col.push_back(row);
                matrix.val.push_back(6.0);
                matrix.row_ptr.push_back(static_cast<int>(matrix.val.size()));
            }
        }
    }
    return matrix;
}

void matvec(const Csr& matrix, const std::vector<double>& x, std::vector<double>& y) {
    const int rows = static_cast<int>(matrix.row_ptr.size()) - 1;
    for (int row = 0; row < rows; ++row) {
        double sum = 0.0;
        for (int p = matrix.row_ptr[static_cast<std::size_t>(row)];
             p < matrix.row_ptr[static_cast<std::size_t>(row) + 1];
             ++p) {
            const std::size_t entry = static_cast<std::size_t>(p);
            sum += matrix.val[entry] * x[static_cast<std::size_t>(matrix.col[entry])];
        }
        y[static_cast<std::size_t>(row)] = sum;
    }
}

double dot(const std::vector<double>& a, const std::vector<double>& b) {
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        sum += a[i] * b[i];
    }
    return sum;
}

struct SolveResult {
    int iterations = 0;
    double residual = 0.0;
    bool converged = false;
};

SolveResult conjugate_gradient(const Csr& matrix,
                               const std::vector<double>& b,
                               double tolerance,
                               std::vector<double>& x) {
    const std::size_t n = b.size();
    x.assign(n, 0.0);
    std::vector<double> r = b;
    std::vector<double> p = r;
    std::vector<double> ap(n, 0.0);
    double rr = dot(r, r);
    const double bnorm = std::sqrt(dot(b, b));
    const double threshold = tolerance * (bnorm > 0.0 ? bnorm : 1.0);
    SolveResult result;
    const int max_iterations = static_cast<int>(n);
    for (int iteration = 0; iteration < max_iterations; ++iteration) {
        matvec(matrix, p, ap);
        const double denom = dot(p, ap);
        if (!(std::fabs(denom) > 0.0)) {
            break;
        }
        const double alpha = rr / denom;
        for (std::size_t i = 0; i < n; ++i) {
            x[i] += alpha * p[i];
            r[i] -= alpha * ap[i];
        }
        const double rr_next = dot(r, r);
        result.iterations = iteration + 1;
        result.residual = std::sqrt(rr_next) / (bnorm > 0.0 ? bnorm : 1.0);
        if (std::sqrt(rr_next) <= threshold) {
            result.converged = true;
            break;
        }
        const double beta = rr_next / rr;
        for (std::size_t i = 0; i < n; ++i) {
            p[i] = r[i] + beta * p[i];
        }
        rr = rr_next;
    }
    return result;
}

std::string json_escape(std::string_view text) {
    std::string out;
    for (char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

void emit_stub(const Options& options) {
    std::cout << '{' << "\"schema_version\":1,"
              << "\"benchmark\":\"poisson_cg\","
              << "\"kind\":\"" << json_escape(options.kind) << "\","
              << "\"implemented\":false,"
              << "\"dof\":0,"
              << "\"phases_s\":{\"mesh\":0,\"assembly\":0,\"solve\":0,\"io\":0},"
              << "\"wall_s\":0,"
              << "\"dof_per_s\":0,"
              << "\"linear_iterations\":0,"
              << "\"newton_iterations\":0,"
              << "\"peak_rss_bytes\":" << peak_rss_bytes() << ","
              << "\"peak_gpu_bytes\":0,"
              << "\"linear_relative_residual\":0,"
              << "\"newton_tolerance\":0"
              << "}\n";
}

} // namespace

int main(int argc, char** argv) {
    const Options options = parse_args(argc, argv);
    // End-to-end package solves and GPU launches are not in this skeleton.
    // Scaling uses the same Poisson kernel when the harness asks for it;
    // GPU and end-to-end kinds stay explicit stubs (compile-only CUDA is separate).
    if (options.kind == "end_to_end_gpu" || options.kind == "end_to_end_cpu") {
        emit_stub(options);
        return 0;
    }
    if (options.kind != "microbenchmark" && options.kind != "scaling") {
        std::cerr << "unknown kind: " << options.kind << '\n';
        return 2;
    }

    auto t0 = std::chrono::steady_clock::now();
    const int n = options.grid;
    const int dof = n * n * n;
    std::vector<double> rhs;
    // Touch the coordinate tables so the mesh phase is real work, not an empty timer.
    std::vector<double> coordinates(static_cast<std::size_t>(dof) * 3);
    for (int k = 0; k < n; ++k) {
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const std::size_t base = static_cast<std::size_t>(index_of(n, i, j, k)) * 3;
                coordinates[base] = static_cast<double>(i + 1) / static_cast<double>(n + 1);
                coordinates[base + 1] = static_cast<double>(j + 1) / static_cast<double>(n + 1);
                coordinates[base + 2] = static_cast<double>(k + 1) / static_cast<double>(n + 1);
            }
        }
    }
    const double mesh_s = seconds_since(t0);

    t0 = std::chrono::steady_clock::now();
    const Csr matrix = assemble(n, rhs);
    const double assembly_s = seconds_since(t0);

    t0 = std::chrono::steady_clock::now();
    std::vector<double> x;
    const SolveResult solved = conjugate_gradient(matrix, rhs, options.linear_tolerance, x);
    const double solve_s = seconds_since(t0);
    if (!solved.converged) {
        std::cerr << "CG did not reach relative residual " << options.linear_tolerance << " (last "
                  << solved.residual << ")\n";
        return 1;
    }

    t0 = std::chrono::steady_clock::now();
    std::ostringstream discarded;
    discarded << x[0] << ',' << x[x.size() / 2] << ',' << x.back();
    const std::string preview = discarded.str();
    const double io_s = seconds_since(t0);
    const double wall_s = mesh_s + assembly_s + solve_s + io_s;
    const double dof_per_s = solve_s > 0.0 ? static_cast<double>(dof) / solve_s : 0.0;
    const long rss = peak_rss_bytes();

    std::cout.setf(std::ios::fmtflags(0), std::ios::floatfield);
    std::cout.precision(17);
    std::cout << '{' << "\"schema_version\":1,"
              << "\"benchmark\":\"poisson_cg\","
              << "\"kind\":\"" << json_escape(options.kind) << "\","
              << "\"implemented\":true,"
              << "\"grid\":" << n << ',' << "\"dof\":" << dof << ',' << "\"phases_s\":{"
              << "\"mesh\":" << mesh_s << ',' << "\"assembly\":" << assembly_s << ','
              << "\"solve\":" << solve_s << ',' << "\"io\":" << io_s << "},"
              << "\"wall_s\":" << wall_s << ',' << "\"dof_per_s\":" << dof_per_s << ','
              << "\"linear_iterations\":" << solved.iterations << ',' << "\"newton_iterations\":1,"
              << "\"peak_rss_bytes\":" << rss << ',' << "\"peak_gpu_bytes\":0,"
              << "\"linear_relative_residual\":" << solved.residual << ','
              << "\"newton_tolerance\":0,"
              << "\"preview\":\"" << json_escape(preview) << "\""
              << "}\n";
    return 0;
}
