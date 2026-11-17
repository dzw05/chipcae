#include "chipcae/material/cte_convert.hpp"

#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace chipcae::material;

namespace {
void check(bool pass, const char* name) {
    if (!pass) {
        std::fprintf(stderr, "FAIL ctecontract: %s\n", name);
        std::exit(EXIT_FAILURE);
    }
}
} // namespace

int main(int argc, char** argv) {
    Kokkos::ScopeGuard guard(argc, argv);
    const CteTable sec{{250, 300, 350}, {1e-5, 2e-5, 3e-5}, 250};
    const CteTable tan{{250, 300, 350}, {1e-5, 3e-5, 5e-5}, 250};
    auto reject = [&](const CteTable& bad) {
        std::vector<Real> out;
        for (bool prefill : {false, true}) {
            out.clear();
            if (prefill)
                check(secant_to_tangent(sec, out) == StatusCode::Ok && !out.empty(),
                      "successful secant prefill");
            check(secant_to_tangent(bad, out) == StatusCode::Failed,
                  "invalid secant vector status");
            check(out.empty(), "invalid secant clears vector");
            if (prefill)
                check(tangent_to_secant(tan, out) == StatusCode::Ok && !out.empty(),
                      "successful tangent prefill");
            check(tangent_to_secant(bad, out) == StatusCode::Failed,
                  "invalid tangent vector status");
            check(out.empty(), "invalid tangent clears vector");
        }
        Real value;
        check(secant_to_tangent(bad, 325, value) == StatusCode::Failed, "invalid secant scalar");
        check(tangent_to_secant(bad, 325, value) == StatusCode::Failed, "invalid tangent scalar");
    };
    reject(CteTable{});
    reject(CteTable{{250}, {1e-5}, 250});
    for (const auto& fixture : {sec, tan}) {
        auto bad = fixture;
        bad.alpha.pop_back();
        reject(bad);
        bad = fixture;
        bad.alpha.push_back(fixture.alpha.back());
        reject(bad);
        bad = fixture;
        bad.temperatures[1] = bad.temperatures[0];
        reject(bad);
        bad = fixture;
        bad.temperatures = {350, 300, 250};
        reject(bad);
        for (Real nonfinite : {std::numeric_limits<Real>::quiet_NaN(),
                               std::numeric_limits<Real>::infinity(),
                               -std::numeric_limits<Real>::infinity()}) {
            for (std::size_t i = 0; i < fixture.alpha.size(); ++i) {
                bad = fixture;
                bad.alpha[i] = nonfinite;
                reject(bad);
                bad = fixture;
                bad.temperatures[i] = nonfinite;
                reject(bad);
            }
            bad = fixture;
            bad.reference_temperature = nonfinite;
            reject(bad);
            Real value;
            check(secant_to_tangent(fixture, nonfinite, value) == StatusCode::Failed,
                  "nonfinite secant input temperature");
            check(tangent_to_secant(fixture, nonfinite, value) == StatusCode::Failed,
                  "nonfinite tangent input temperature");
        }
    }
}
