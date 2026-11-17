#include "chipcae/material/cte_convert.hpp"
#include "cte_fixtures.hpp"

#include <cstddef>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace chipcae::material;

namespace {
void check(bool pass, const char* name) {
    if (!pass) {
        std::fprintf(stderr, "FAIL ctecontract: %s\n", name);
        throw std::runtime_error(name);
    }
}
} // namespace

int main(int argc, char** argv) try {
    Kokkos::ScopeGuard guard(argc, argv);
    const auto& sec = test::sec;
    const auto& tan = test::tan;
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
        Real value = std::numeric_limits<Real>::quiet_NaN();
        check(secant_to_tangent(bad, test::secant_to_tangent_temperature, value) ==
                  StatusCode::Failed,
              "invalid secant scalar");
        check(tangent_to_secant(bad, test::tangent_to_secant_temperature, value) ==
                  StatusCode::Failed,
              "invalid tangent scalar");
    };
    reject(CteTable{});
    reject(CteTable{{sec.temperatures.front()}, {sec.alpha.front()}, sec.reference_temperature});
    for (const auto& fixture : {sec, tan}) {
        auto bad = fixture;
        bad.temperatures.clear();
        reject(bad);
        bad = fixture;
        bad.alpha.clear();
        reject(bad);
        bad = fixture;
        bad.alpha.pop_back();
        reject(bad);
        bad = fixture;
        bad.alpha.push_back(fixture.alpha.back());
        reject(bad);
        bad = fixture;
        bad.temperatures.pop_back();
        reject(bad);
        bad = fixture;
        bad.temperatures.push_back(fixture.temperatures.back());
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
            // All four public overloads consume Tref exclusively through this
            // field; reject() checks both vectors and both scalar signatures.
            // The scalar Real argument below is query T, not a separate Tref.
            bad.reference_temperature = nonfinite;
            reject(bad);
            Real value = std::numeric_limits<Real>::quiet_NaN();
            check(secant_to_tangent(fixture, nonfinite, value) == StatusCode::Failed,
                  "nonfinite secant input temperature");
            check(tangent_to_secant(fixture, nonfinite, value) == StatusCode::Failed,
                  "nonfinite tangent input temperature");
        }
    }
} catch (const std::exception&) {
    return 1;
}
