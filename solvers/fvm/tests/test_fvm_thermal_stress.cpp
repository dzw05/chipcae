#include "fvm_test_util.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sys/resource.h>

using namespace fvm_test;

namespace {
struct Measurement {
    std::string name, failure;
    double thickness = 0, position = 0, contrast = 1;
    std::optional<double> reference;
    double attempt_seconds = 0;
    std::optional<double> seconds;
    std::optional<double> defect, condition, affine_error, cell_balance, face_jump, imbalance;
    std::optional<int> iterations;
    long rss_kib = 0;
    bool known_risk = false;
};
std::string json_string(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (char c : value) {
        if (c == '\\' || c == '"')
            out << '\\' << c;
        else if (c == '\n')
            out << "\\n";
        else
            out << c;
    }
    return out.str() + '"';
}
template <class T>
void field(std::ostream& out, const char* name, std::optional<T> value) {
    out << ',' << json_string(name) << ':';
    if (value && std::isfinite(static_cast<double>(*value)))
        out << *value;
    else
        out << "null";
}
void diagnostics(Measurement& row, const fv::ThermalResult& r) {
    row.condition = r.max_local_condition;
    row.cell_balance = r.cell_balance_max;
    row.face_jump = r.face_jump_max;
    row.imbalance = r.imbalance;
    row.iterations = r.iterations;
}
void thin(Measurement& row) {
    Vec normal{1, .37, .21};
    normal = mul(normal, 1 / norm(normal));
    mesh::cut::CutGeometry geometry{{{0, normal, row.position + row.thickness / 2},
                                     {0, mul(normal, -1), -row.position + row.thickness / 2}}};
    auto f = cut_fixture(4, geometry);
    auto g = take(fv::build_grid(f.mesh, &f.cut), "thin-slab build_grid");
    row.defect = g.max_identity_defect;
    geometry_gate(g);
    Polynomial u{0, {1, .3, 0}, false};
    auto polys = polynomials(g, f.mesh, geometry);
    auto p = manufactured(g, polys, u, "jacobi", true);
    p.rtol = 1e-12;
    p.T_ref = row.reference;
    auto start = std::chrono::steady_clock::now();
    auto r = solve(g, f.mesh, p);
    row.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    diagnostics(row, r);
    double error = 0;
    for (std::size_t c = 0; c < polys.size(); ++c)
        error = std::max(error, std::abs(r.T_cell[c] - u.cell(polys[c])));
    row.affine_error = error;
    require(error <= 1e-10, "thin-slab affine error <= 1e-10");
    conservation(r);
}
void contrast(Measurement& row, bool tangential) {
    auto m = boxes({uniform(4), uniform(4), {0, .17, 1}}, true);
    auto g = take(fv::build_grid(m), "contrast build_grid");
    row.defect = g.max_identity_defect;
    auto p = tangential ? manufactured(g, polynomials(g, m), {0, {1, .3, 0}, false}) : problem(g);
    p.rtol = 1e-12;
    p.T_ref = row.reference;
    if (!tangential)
        p.bcs = {dirichlet("face:zmin", 310), dirichlet("face:zmax", 290)};
    for (std::size_t c = 0; c < p.k.size(); ++c) {
        double k = m.attribute[g.cv_elem[c]] == 1 ? 1 : row.contrast;
        p.k[c] = {k, k, k};
    }
    auto start = std::chrono::steady_clock::now();
    auto r = solve(g, m, p);
    row.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    diagnostics(row, r);
    double error = 0;
    for (std::size_t c = 0; c < r.T_cell.size(); ++c) {
        double exact = 0;
        if (tangential) {
            exact = g.cv_centroid[c][0] + .3 * g.cv_centroid[c][1];
        } else {
            const auto e = static_cast<std::size_t>(g.cv_elem[c]);
            auto z = (vertex(m, e, 0)[2] + vertex(m, e, 6)[2]) / 2;
            auto resistance = z < .17 ? z : .17 + (z - .17) / row.contrast;
            exact = 310 - 20 * resistance / (.17 + .83 / row.contrast);
        }
        error = std::max(error, std::abs(r.T_cell[c] - exact));
    }
    row.affine_error = error;
    require(error <= 1e-10, "contrast piecewise/affine error <= 1e-10");
    conservation(r); // Includes 1:1e8; known risk does not relax a gate.
}
void benchmark(Measurement& row) {
    auto m = boxes({uniform(68), uniform(68), uniform(6)});
    auto start = std::chrono::steady_clock::now();
    auto g = take(fv::build_grid(m), "68x68x6 build_grid");
    row.defect = g.max_identity_defect;
    auto p = problem(g);
    p.rtol = 1e-12;
    p.bcs = {dirichlet("face:zmin", 1), dirichlet("face:zmax", 0)};
    auto r = solve(g, m, p);
    row.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    diagnostics(row, r);
    conservation(r);
}
} // namespace

int main(int argc, char** argv) {
    require(argc == 3 && std::string(argv[1]) == "--output",
            "usage: stress --output <external-directory>");
    const auto directory = std::filesystem::absolute(argv[2]);
    argc = 1;
    argv[1] = nullptr;
    PetscSession petsc(argc, argv);
    std::filesystem::create_directories(directory);
    std::vector<Measurement> rows;
    Suite suite;
    auto run = [&](Measurement row, const auto& body) {
        auto start = std::chrono::steady_clock::now();
        suite.run(row.name, [&] {
            try {
                body(row);
            } catch (const std::exception& e) {
                row.failure = e.what();
                throw;
            }
        });
        row.attempt_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        rusage usage{};
        require(getrusage(RUSAGE_SELF, &usage) == 0, "getrusage");
        row.rss_kib = usage.ru_maxrss;
        rows.push_back(std::move(row));
    };
    for (int i = 0; i < 10; ++i)
        for (double position : {.43, .67}) {
            Measurement row;
            row.name = "thin slab " + std::to_string(i) + " position " + std::to_string(position);
            row.thickness = .02 * std::pow(1e-8, double(i) / 9);
            row.position = position;
            run(row, thin);
        }
    for (double ratio : {1e6, 1e8, 1e-6, 1e-8})
        for (bool tangential : {false, true})
            for (const std::optional<double> reference : {std::optional<double>{},
                                                          std::optional<double>{0},
                                                          std::optional<double>{300},
                                                          std::optional<double>{1e6}}) {
                Measurement row;
                row.name = std::string(tangential ? "tangential" : "layered") +
                           " contrast=" + std::to_string(ratio) +
                           " T_ref=" + (reference ? std::to_string(*reference) : "default");
                row.contrast = ratio;
                row.reference = reference;
                row.known_risk = ratio >= 1e8 || ratio <= 1e-8;
                run(row, [&](auto& result) { contrast(result, tangential); });
            }
    Measurement perf;
    perf.name = "synthetic 68x68x6 assembly and solve";
    run(perf, benchmark);
    if (suite.rank == 0) {
        std::ofstream out(directory / "measurements.json");
        require(bool(out), "open stress JSON");
        out << std::setprecision(17) << "{\"measured\":true,\"failures\":" << suite.failed
            << ",\"cases\":[";
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto& row = rows[i];
            if (i)
                out << ',';
            out << "{\"name\":" << json_string(row.name)
                << ",\"failure\":" << json_string(row.failure) << ",\"thickness\":" << row.thickness
                << ",\"position\":" << row.position << ",\"contrast\":" << row.contrast
                << ",\"known_risk\":" << (row.known_risk ? "true" : "false")
                << ",\"attempt_seconds\":" << row.attempt_seconds << ",\"rss_kib\":" << row.rss_kib;
            field(out, "assembly_solve_seconds", row.seconds);
            field(out, "T_ref", row.reference);
            field(out, "identity_defect", row.defect);
            field(out, "condition", row.condition);
            field(out, "affine_error", row.affine_error);
            field(out, "cell_balance", row.cell_balance);
            field(out, "face_jump", row.face_jump);
            field(out, "imbalance", row.imbalance);
            field(out, "iterations", row.iterations);
            out << '}';
        }
        out << "]}\n";
        require(bool(out), "write stress JSON");
    }
    return suite.finish();
}
