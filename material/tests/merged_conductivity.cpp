#include "chipcae/material/detail/merged_conductivity.hpp"

#include "chipcae/material/copper_laminate.hpp"

#include <Kokkos_Core.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace {
using chipcae::material::copper_laminate_conductivity;
using namespace chipcae::material::detail;
using Status = MergedConductivityStatus;
using Kind = MergedConductivityKind;
using Result = MergedConductivity;

// material/library/copper.materials.json: materials[0].conductivity, W/(m*K).
constexpr double copper = 394.0;
// material/library/bt_hl832ns_eq.materials.json: materials[0].conductivity, W/(m*K).
constexpr double matrix = 0.6;
constexpr double tolerance = 1e-14;

// Fixed-size, trivially copied input for exactly one evaluation per device work item.
struct Input {
    int n = 0;
    double k[24]{};
    double volume[8]{};
    int ijk[24]{};
};
struct LegalCase {
    std::string name;
    Input input;
    Result host;
    Status status;
    Kind expected_kind;
    int expected_axis;
};
struct Check {
    std::string name;
    int failures = 0;
    void expect(bool condition, const std::string& detail) {
        if (!condition) {
            if (failures < 8)
                std::printf("  %s: %s\n", name.c_str(), detail.c_str());
            ++failures;
        }
    }
    int report() const {
        std::printf("%s %s (%d failed assertions)\n",
                    failures == 0 ? "PASS" : "FAIL",
                    name.c_str(),
                    failures);
        return failures == 0 ? 0 : 1;
    }
};

bool bits(double a, double b) {
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}
bool identical(const Result& a, const Result& b) {
    bool same = a.kind == b.kind && a.axis == b.axis;
    for (int c = 0; c < 6; ++c)
        same = bits(a.k[c], b.k[c]) && same;
    return same;
}
bool close(double actual, double expected) {
    return std::isfinite(actual) && std::abs(actual - expected) <= tolerance * std::abs(expected);
}
Result sentinel() {
    return {{-11.0, -12.0, -13.0, -0.0, 17.0, -19.0}, Kind::ReussBound, 97};
}
Status evaluate(const Input& in, Result& out) {
    return merged_conductivity_tensor(in.n, in.k, in.volume, in.ijk, &out);
}
void member(Input& in,
            int i,
            std::array<int, 3> ijk,
            double volume,
            double fraction,
            double low = matrix,
            double high = copper) {
    copper_laminate_conductivity(low, high, fraction, in.k + 3 * i);
    in.volume[i] = volume;
    std::copy(ijk.begin(), ijk.end(), in.ijk + 3 * i);
}
Input pair(int axis) {
    Input in;
    in.n = 2;
    member(in, 0, {0, 0, 0}, 1, 0);
    std::array<int, 3> next{};
    next[axis] = 1;
    member(in, 1, next, 1, 1);
    return in;
}

// Independent formula oracle: no call to the merged-conductivity function, no sorting
// or classification algorithm. Expected kind/axis come from the explicit fixture geometry.
struct Means {
    double arithmetic[3]{};
    double harmonic[3]{};
    double minimum[3]{};
    double maximum[3]{};
};
Means means(const Input& in) {
    Means m;
    double total = 0;
    for (int i = 0; i < in.n; ++i)
        total += in.volume[i];
    for (int c = 0; c < 3; ++c) {
        double inverse = 0;
        m.minimum[c] = m.maximum[c] = in.k[c];
        for (int i = 0; i < in.n; ++i) {
            const double weight = in.volume[i] / total;
            const double k = in.k[3 * i + c];
            m.arithmetic[c] += weight * k;
            inverse += weight / k;
            m.minimum[c] = std::min(m.minimum[c], k);
            m.maximum[c] = std::max(m.maximum[c], k);
        }
        m.harmonic[c] = 1 / inverse;
    }
    return m;
}

void check_bounds(Check& check, const std::string& name, const Input& in, const Result& out) {
    const auto m = means(in);
    for (int c = 0; c < 3; ++c) {
        check.expect(std::isfinite(out.k[c]) && out.k[c] > 0, name + " SPD diagonal");
        check.expect(out.k[c] >= m.harmonic[c] - tolerance * std::abs(m.harmonic[c]) &&
                         out.k[c] <= m.arithmetic[c] + tolerance * std::abs(m.arithmetic[c]),
                     name + " Reuss <= K <= Voigt");
        check.expect(out.k[c] >= m.minimum[c] && out.k[c] <= m.maximum[c],
                     name + " component extrema");
    }
    for (int c = 3; c < 6; ++c)
        check.expect(bits(out.k[c], 0.0), name + " SPD positive-zero off-diagonal");
}

Result legal(Check& check,
             Check& bounds,
             std::vector<LegalCase>& cases,
             const std::string& name,
             const Input& in,
             Kind kind,
             int axis) {
    Result out = sentinel();
    const auto status = evaluate(in, out);
    check.expect(status == Status::Ok, name + " status Ok");
    check.expect(out.kind == kind, name + " kind");
    check.expect(out.axis == axis, name + " axis");
    const auto m = means(in);
    for (int c = 0; c < 3; ++c) {
        const double expected = kind == Kind::Uniform                 ? in.k[c]
                                : kind == Kind::Laminate && c != axis ? m.arithmetic[c]
                                                                      : m.harmonic[c];
        check.expect(close(out.k[c], expected),
                     name + " closed form component " + std::to_string(c));
        if (kind == Kind::Uniform)
            check.expect(bits(out.k[c], in.k[c]), name + " bitwise Uniform component");
        check.expect(std::isfinite(out.k[c]) && out.k[c] > 0, name + " finite positive diagonal");
        check.expect(out.k[c] >= m.minimum[c] && out.k[c] <= m.maximum[c], name + " extrema");
    }
    for (int c = 3; c < 6; ++c)
        check.expect(bits(out.k[c], 0.0), name + " positive-zero off-diagonal");
    if (kind != Kind::Uniform)
        check_bounds(bounds, name, in, out);
    cases.push_back({name, in, out, status, kind, axis});
    return out;
}

Input permuted(const Input& in, const std::array<int, 8>& order) {
    Input out = in;
    for (int i = 0; i < in.n; ++i) {
        out.volume[i] = in.volume[order[i]];
        for (int c = 0; c < 3; ++c) {
            out.k[3 * i + c] = in.k[3 * order[i] + c];
            out.ijk[3 * i + c] = in.ijk[3 * order[i] + c];
        }
    }
    return out;
}

int named_cases(std::vector<LegalCase>& cases, Check& bounds) {
    int failures = 0;
    auto run = [&](const char* name, const Input& in, Kind kind, int axis) {
        Check check{name};
        const auto out = legal(check, bounds, cases, name, in, kind, axis);
        failures += check.report();
        return out;
    };
    Input single;
    single.n = 1;
    member(single, 0, {0, 0, 0}, 1, 1);
    run("uniform_single", single, Kind::Uniform, -1);
    Input eight;
    eight.n = 8;
    for (int i = 0; i < 8; ++i)
        member(eight, i, {i % 2, (i / 2) % 2, i / 4}, i + 1, 0.3);
    run("uniform_eight", eight, Kind::Uniform, -1);

    const auto z = run("laminate_z_two", pair(2), Kind::Laminate, 2);
    for (int axis : {0, 1}) {
        const std::string name = axis == 0 ? "laminate_x_two" : "laminate_y_two";
        Check check{name};
        const auto out = legal(check, bounds, cases, name, pair(axis), Kind::Laminate, axis);
        for (int c = 0; c < 3; ++c)
            check.expect(bits(out.k[c], z.k[c == axis ? 2 : 0]), "coordinate-permuted z bits");
        failures += check.report();
    }
    Input slabs;
    slabs.n = 4;
    member(slabs, 0, {0, 0, 0}, 1, 0);
    member(slabs, 1, {1, 0, 0}, 2, 0);
    member(slabs, 2, {0, 0, 1}, 3, 1);
    member(slabs, 3, {1, 0, 1}, 4, 1);
    {
        Check check{"laminate_z_slabs"};
        const auto out = legal(check, bounds, cases, check.name, slabs, Kind::Laminate, 2);
        check.expect(close(out.k[0], 0.3 * matrix + 0.7 * copper), "phase weights 0.3/0.7");
        check.expect(close(out.k[1], 0.3 * matrix + 0.7 * copper), "tangential y");
        check.expect(close(out.k[2], 1 / (0.3 / matrix + 0.7 / copper)), "series phase weights");
        failures += check.report();
    }
    auto sliver = pair(2);
    sliver.volume[0] = 1e-4;
    sliver.volume[1] = 0.9999;
    run("g4pp_sliver", sliver, Kind::Laminate, 2);
    Input checkerboard;
    checkerboard.n = 4;
    member(checkerboard, 0, {0, 0, 0}, 1, 1);
    member(checkerboard, 1, {1, 1, 0}, 1, 1);
    member(checkerboard, 2, {1, 0, 0}, 1, 0);
    member(checkerboard, 3, {0, 1, 0}, 1, 0);
    {
        Check check{"checkerboard_reference"};
        const auto out =
            legal(check, bounds, cases, check.name, checkerboard, Kind::ReussBound, -1);
        const double harmonic = 2 * copper * matrix / (copper + matrix);
        const double exact = std::sqrt(copper * matrix);
        for (int c : {0, 1}) {
            check.expect(close(out.k[c], harmonic), "two-phase closed form");
            check.expect(out.k[c] <= exact && exact <= (copper + matrix) / 2,
                         "Reuss <= exact checkerboard <= Voigt");
        }
        failures += check.report();
    }
    Input elbow;
    elbow.n = 3;
    member(elbow, 0, {0, 0, 0}, 1, 1);
    member(elbow, 1, {1, 0, 0}, 1, 0);
    member(elbow, 2, {0, 1, 0}, 1, 0);
    run("multi_direction_L", elbow, Kind::ReussBound, -1);
    auto diagonal = pair(0);
    diagonal.ijk[4] = 1;
    run("diagonal_neighbours", diagonal, Kind::ReussBound, -1);

    auto duplicate = slabs;
    // Duplicate low-phase coordinate still contributes 1+2 of total volume 10.
    duplicate.ijk[3] = 0;
    run("duplicate_identical", duplicate, Kind::Laminate, 2);
    duplicate = pair(2);
    duplicate.ijk[5] = 0;
    duplicate.volume[1] = 3;
    run("duplicate_conflicting", duplicate, Kind::ReussBound, -1);

    // Distinct component values catch scalar-only averaging or classification by kxx.
    for (int axis = 0; axis < 3; ++axis) {
        auto anisotropic = pair(axis);
        for (int i = 0; i < 2; ++i)
            for (int c = 0; c < 3; ++c) {
                double value[3]{};
                copper_laminate_conductivity(matrix * (c + 1), copper / (c + 1), i, value);
                anisotropic.k[3 * i + c] = value[c];
            }
        run(("anisotropic_axis_" + std::to_string(axis)).c_str(),
            anisotropic,
            Kind::Laminate,
            axis);
    }
    auto uniform_anisotropic = eight;
    for (int i = 0; i < uniform_anisotropic.n; ++i)
        for (int c = 0; c < 3; ++c) {
            double value[3]{};
            copper_laminate_conductivity(matrix * (c + 1), copper / (c + 1), 0.3, value);
            uniform_anisotropic.k[3 * i + c] = value[c];
        }
    run("uniform_anisotropic", uniform_anisotropic, Kind::Uniform, -1);
    for (int changed = 0; changed < 3; ++changed) {
        auto component_difference = pair(2);
        for (int c = 0; c < 3; ++c)
            if (c != changed)
                component_difference.k[3 + c] = component_difference.k[c];
        run(("classification_component_" + std::to_string(changed)).c_str(),
            component_difference,
            Kind::Laminate,
            2);
    }
    Input tied;
    tied.n = 4;
    for (int i = 0; i < tied.n; ++i)
        member(tied, i, {-3, 4, -1}, i == 0 ? 1e-12 : i + 1, i % 2);
    run("duplicate_sort_ties", tied, Kind::ReussBound, -1);
    // All three axes qualify vacuously; only a unique candidate may be Laminate.
    auto all_axes = pair(2);
    all_axes.ijk[3] = all_axes.ijk[4] = 1;
    run("three_candidate_axes", all_axes, Kind::ReussBound, -1);
    return failures;
}

int sweep(std::vector<LegalCase>& cases, Check& bounds) {
    Check check{"parameter_sweep"};
    int count = 0;
    for (int n = 1; n <= 8; ++n) {
        for (double contrast : {1.0, 1.0 + 0x1p-40, 10.0, copper / matrix, 1e6}) {
            for (double s : {1e-12, 1e-4, 0.5 / n}) {
                for (int layout = 0; layout < 4; ++layout) {
                    Input in;
                    in.n = n;
                    for (int i = 0; i < n; ++i) {
                        std::array<int, 3> ijk{};
                        if (layout < 3)
                            ijk[layout] = i;
                        else if (n == 2)
                            ijk = {i, i, 0};
                        else if (i == 0)
                            ijk = {1, 0, 0};
                        else if (i >= 2)
                            ijk = {0, i - 1, 0};
                        // The odd (high) phase at i=1 is the L corner. Both low
                        // arms share a different coordinate with that corner.
                        const double volume = n == 1 ? 1 : i == 0 ? s : (1 - s) / (n - 1);
                        member(in, i, ijk, volume, i % 2, matrix, matrix * contrast);
                    }
                    const bool uniform = n == 1 || bits(contrast, 1.0);
                    const Kind kind = uniform      ? Kind::Uniform
                                      : layout < 3 ? Kind::Laminate
                                                   : Kind::ReussBound;
                    const int axis = !uniform && layout < 3 ? layout : -1;
                    legal(check, bounds, cases, "sweep_" + std::to_string(count), in, kind, axis);
                    ++count;
                }
            }
        }
    }
    check.expect(count == 480, "executed matrix count");
    std::printf("parameter_sweep executed=%d\n", count);
    return check.report();
}

int permutations(std::vector<LegalCase>& cases, Check& bounds) {
    Check check{"permutation_invariance"};
    // Include duplicate ties: sorting only by coordinates cannot guarantee invariance.
    for (const char* name : {"laminate_z_slabs",
                             "multi_direction_L",
                             "uniform_eight",
                             "duplicate_identical",
                             "duplicate_conflicting",
                             "duplicate_sort_ties"}) {
        const auto found = std::find_if(
            cases.begin(), cases.end(), [&](const LegalCase& item) { return item.name == name; });
        const LegalCase base = *found;
        std::array<int, 8> order{};
        std::iota(order.begin(), order.end(), 0);
        int count = 0;
        if (base.input.n == 8) {
            std::mt19937_64 generator(0x5343434d34ULL);
            for (int trial = 0; trial < 40; ++trial) {
                // Explicit Fisher-Yates keeps the shuffle stable across standard libraries.
                for (int i = 7; i > 0; --i)
                    std::swap(order[i], order[generator() % static_cast<unsigned>(i + 1)]);
                const auto in = permuted(base.input, order);
                const auto out = legal(check, bounds, cases, name, in, Kind::Uniform, -1);
                check.expect(identical(out, base.host), "uniform shuffle all fields");
                ++count;
            }
        } else {
            const Kind kind = base.expected_kind;
            const int axis = base.expected_axis;
            do {
                const auto in = permuted(base.input, order);
                const auto out = legal(check, bounds, cases, name, in, kind, axis);
                check.expect(identical(out, base.host), std::string(name) + " all fields");
                ++count;
            } while (std::next_permutation(order.begin(), order.begin() + base.input.n));
        }
        const int expected = base.input.n == 8   ? 40
                             : base.input.n == 4 ? 24
                             : base.input.n == 3 ? 6
                                                 : 2;
        check.expect(count == expected, std::string(name) + " permutation count");
        std::printf("permutation_invariance %s executed=%d\n", name, count);
    }
    return check.report();
}

int scaling(std::vector<LegalCase>& cases, Check& bounds) {
    Check check{"scaling"};
    const std::size_t original_count = cases.size();
    for (std::size_t i = 0; i < original_count; ++i) {
        const LegalCase base = cases[i];
        for (bool scale_k : {true, false}) {
            auto in = base.input;
            for (int j = 0; j < in.n; ++j) {
                if (scale_k) {
                    for (int c = 0; c < 3; ++c)
                        in.k[3 * j + c] *= 2;
                } else {
                    in.volume[j] *= 2;
                }
            }
            const std::string name = base.name + (scale_k ? "_k2" : "_v2");
            const auto out =
                legal(check, bounds, cases, name, in, base.expected_kind, base.expected_axis);
            Result expected = base.host;
            if (scale_k)
                for (int c = 0; c < 3; ++c)
                    expected.k[c] *= 2;
            check.expect(identical(out, expected), name + " scaled result bits");
        }
    }
    std::printf("scaling executed=%zu\n", 2 * original_count);
    return check.report();
}

int invalid_inputs() {
    Check check{"invalid_inputs"};
    Input valid;
    valid.n = 8;
    for (int i = 0; i < valid.n; ++i)
        member(valid, i, {0, 0, i}, i + 1, i % 2);
    int count = 0;
    auto call =
        [&](const Input& in, Status expected, int null_pointer, const std::string& context) {
            Result out = sentinel();
            const Result before = out;
            const auto status = merged_conductivity_tensor(in.n,
                                                           null_pointer == 0 ? nullptr : in.k,
                                                           null_pointer == 1 ? nullptr : in.volume,
                                                           null_pointer == 2 ? nullptr : in.ijk,
                                                           null_pointer == 3 ? nullptr : &out);
            check.expect(status == expected, context + " exact status");
            check.expect(identical(out, before), context + " sentinel fields unchanged");
            ++count;
        };
    for (int n : {-1, 0, 9}) {
        auto in = valid;
        in.n = n;
        const auto status = n <= 0 ? Status::EmptyGroup : Status::TooManyMembers;
        call(in, status, -1, "n=" + std::to_string(n));
        for (int pointer = 0; pointer < 4; ++pointer)
            call(in, status, pointer, "count precedes null pointer");
    }
    for (int pointer = 0; pointer < 4; ++pointer)
        call(valid, Status::NullPointer, pointer, "each null pointer");
    const double bad_values[] = {0.0,
                                 -0.0,
                                 -1.0,
                                 std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::infinity(),
                                 -std::numeric_limits<double>::infinity()};
    for (double bad : bad_values) {
        for (int i = 0; i < valid.n; ++i) {
            auto in = valid;
            in.volume[i] = bad;
            call(in, Status::InvalidVolume, -1, "invalid volume member " + std::to_string(i));
            for (int pointer = 0; pointer < 4; ++pointer)
                call(in, Status::NullPointer, pointer, "null precedes invalid volume");
            for (int c = 0; c < 3; ++c) {
                in = valid;
                in.k[3 * i + c] = bad;
                call(in,
                     Status::InvalidConductivity,
                     -1,
                     "invalid conductivity member/component " + std::to_string(3 * i + c));
            }
        }
        for (int volume_member = 0; volume_member < valid.n; ++volume_member) {
            for (int k_member = 0; k_member < valid.n; ++k_member) {
                auto in = valid;
                in.volume[volume_member] = bad;
                in.k[3 * k_member] = -1;
                call(in, Status::InvalidVolume, -1, "all volumes validated before conductivities");
            }
        }
    }
    std::printf("invalid_inputs executed=%d\n", count);
    return check.report();
}

int scale_exponent_ranges() {
    Check check{"scale_exponent_ranges"};
    struct ScaleInput {
        double value;
        int exponent;
    };
    std::vector<ScaleInput> probes;
    // A power outside the normal range can still scale a finite input to a finite
    // result. Include a value just above the half-denorm tie to catch premature
    // subnormal rounding during chunked scaling.
    for (double value : {0x1p-1074,
                         0x1p-1022,
                         0x1p-512,
                         0.5,
                         1.0,
                         0x1.0000000000001p0,
                         0x1.fffffffffffffp0,
                         0x1p512,
                         0x1p1023,
                         std::numeric_limits<double>::max()}) {
        for (int exponent : {-4194,
                             -2097,
                             -1076,
                             -1075,
                             -1074,
                             -1023,
                             -1022,
                             -513,
                             -512,
                             -1,
                             0,
                             1,
                             512,
                             513,
                             1023,
                             1024,
                             1074,
                             2097,
                             4194}) {
            const double expected = std::scalbn(value, exponent);
            check.expect(bits(merged_k_scale(value, exponent), expected),
                         "binary64 rounding for " + std::to_string(value) + " * 2^" +
                             std::to_string(exponent));
            probes.push_back({value, exponent});
        }
    }
    using ExecutionSpace = Kokkos::DefaultExecutionSpace;
    const int count = static_cast<int>(probes.size());
    Kokkos::View<ScaleInput*, ExecutionSpace> inputs("scale_inputs", count);
    Kokkos::View<double*, ExecutionSpace> outputs("scale_outputs", count);
    auto host_inputs = Kokkos::create_mirror_view(inputs);
    for (int i = 0; i < count; ++i)
        host_inputs(i) = probes[i];
    Kokkos::deep_copy(inputs, host_inputs);
    Kokkos::parallel_for(
        "scale_exponent_ranges",
        Kokkos::RangePolicy<ExecutionSpace>(0, count),
        KOKKOS_LAMBDA(const int i) {
            outputs(i) = merged_k_scale(inputs(i).value, inputs(i).exponent);
        });
    auto host_outputs = Kokkos::create_mirror_view(outputs);
    Kokkos::deep_copy(host_outputs, outputs);
    for (int i = 0; i < count; ++i)
        check.expect(bits(host_outputs(i), std::scalbn(probes[i].value, probes[i].exponent)),
                     "execution space binary64 rounding for probe " + std::to_string(i));
    return check.report();
}

int extreme_arithmetic(std::vector<LegalCase>& cases) {
    Check check{"extreme_arithmetic"};
    auto run = [&](const char* name, const Input& in, double tangential, double normal) {
        Result out = sentinel();
        const auto status = evaluate(in, out);
        check.expect(status == Status::Ok, std::string(name) + " status");
        check.expect(out.kind == Kind::Laminate && out.axis == 2,
                     std::string(name) + " classification");
        for (int c = 0; c < 3; ++c) {
            const double expected = c == 2 ? normal : tangential;
            check.expect(close(out.k[c], expected), std::string(name) + " scaled closed form");
            check.expect(std::isfinite(out.k[c]) && out.k[c] > 0,
                         std::string(name) + " finite positive diagonal");
        }
        for (int c = 3; c < 6; ++c)
            check.expect(bits(out.k[c], 0.0), std::string(name) + " positive zero");
        cases.push_back({name, in, out, status, Kind::Laminate, 2});
    };
    auto in = pair(2);
    in.volume[0] = in.volume[1] = std::numeric_limits<double>::max();
    run("overflowing_volume_sum",
        in,
        0.5 * matrix + 0.5 * copper,
        1 / (0.5 / matrix + 0.5 / copper));

    const double denorm = std::numeric_limits<double>::denorm_min();
    in.volume[0] = denorm;
    in.volume[1] = 2 * denorm;
    run("subnormal_volumes",
        in,
        matrix / 3 + (2.0 / 3) * copper,
        1 / ((1.0 / 3) / matrix + (2.0 / 3) / copper));

    // Independent power-of-two closed forms avoid overflowing products/reciprocals.
    for (int i = 0; i < 2; ++i)
        member(in, i, {0, 0, i}, 1, i, 0x1p1022, 0x1p1023);
    run("large_conductivities", in, 0x1.8p1022, (4.0 / 3) * 0x1p1022);
    for (int i = 0; i < 2; ++i)
        member(in, i, {0, 0, i}, 1, i, 16 * denorm, 32 * denorm);
    // The harmonic mean is (64/3)*denorm, rounded to 21*denorm in binary64.
    run("subnormal_conductivities", in, 24 * denorm, 21 * denorm);

    for (int i = 0; i < 2; ++i)
        member(in, i, {0, 0, i}, i == 0 ? 0x1p-1000 : 0x1p1000, i, 0x1p-1000, 0x1p1000);
    // Both V/k terms are exactly 1: H=(2^-1000+2^1000)/2 rounds to 2^999.
    // A=(2^-2000+2^2000)/(2^-1000+2^1000) rounds to 2^1000.
    run("coupled_extreme_ratios", in, 0x1p1000, 0x1p999);
    for (int i = 0; i < 2; ++i) {
        in.volume[i] = i == 0 ? denorm : std::numeric_limits<double>::max();
        for (int c = 0; c < 3; ++c)
            in.k[3 * i + c] = in.volume[i];
    }
    // Each V/k is exactly 1 even though the exponent gap is 2097. The tiny
    // phase is negligible for A but contributes half the resistance for H.
    run("full_exponent_gap",
        in,
        std::numeric_limits<double>::max(),
        0.5 * std::numeric_limits<double>::max());
    return check.report();
}

int device_matches_host(const std::vector<LegalCase>& cases) {
    Check check{"device_matches_host"};
    const int count = static_cast<int>(cases.size());
    using ExecutionSpace = Kokkos::DefaultExecutionSpace;
    Kokkos::View<Input*, ExecutionSpace> inputs("merged_inputs", count);
    Kokkos::View<Result*, ExecutionSpace> outputs("merged_outputs", count);
    Kokkos::View<Status*, ExecutionSpace> statuses("merged_statuses", count);
    auto host_inputs = Kokkos::create_mirror_view(inputs);
    auto host_outputs = Kokkos::create_mirror_view(outputs);
    auto host_statuses = Kokkos::create_mirror_view(statuses);
    for (int i = 0; i < count; ++i)
        host_inputs(i) = cases[i].input;
    Kokkos::deep_copy(inputs, host_inputs);
    Kokkos::parallel_for(
        "merged_conductivity_acceptance",
        Kokkos::RangePolicy<ExecutionSpace>(0, count),
        KOKKOS_LAMBDA(const int i) {
            const auto& in = inputs(i);
            Result out{{-11.0, -12.0, -13.0, -0.0, 17.0, -19.0}, Kind::ReussBound, 97};
            statuses(i) = merged_conductivity_tensor(in.n, in.k, in.volume, in.ijk, &out);
            outputs(i) = out;
        });
    Kokkos::deep_copy(host_outputs, outputs);
    Kokkos::deep_copy(host_statuses, statuses);
    for (int i = 0; i < count; ++i) {
        check.expect(host_statuses(i) == Status::Ok && host_statuses(i) == cases[i].status,
                     cases[i].name + " device/host status");
        check.expect(identical(host_outputs(i), cases[i].host),
                     cases[i].name + " device/host bits");
    }
    std::printf(
        "device_matches_host execution_space=%s executed=%d\n", ExecutionSpace::name(), count);
    return check.report();
}

int cost_report() {
    Check check{"cost_report"};
    Input in;
    in.n = 8;
    for (int i = 0; i < 8; ++i)
        member(in, i, {0, 0, i}, i + 1, i % 2);
    constexpr int count = 1000000;
    // Volatile function pointer prevents a compiler from hoisting the fixed-input call.
    using Function = Status (*)(int, const double*, const double*, const int*, Result*);
    Function volatile function = &merged_conductivity_tensor;
    double checksum = 0;
    int bad_statuses = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < count; ++i) {
        Result out = sentinel();
        const auto status = function(in.n, in.k, in.volume, in.ijk, &out);
        checksum += out.k[i % 3];
        bad_statuses += status == Status::Ok ? 0 : 1;
    }
    const double elapsed =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    std::printf("cost_report n=8 calls=%d ns/group=%.3f checksum=%.17g\n",
                count,
                elapsed / count,
                checksum);
    check.expect(bad_statuses == 0, "all timed calls return Ok");
    check.expect(std::isfinite(checksum) && checksum > 0, "consume valid outputs");
    return check.report();
}
} // namespace

int main(int argc, char** argv) {
    Kokkos::ScopeGuard guard(argc, argv);
    std::vector<LegalCase> cases;
    Check bounds{"bounds_order"};
    int failures = named_cases(cases, bounds);
    failures += sweep(cases, bounds);
    failures += permutations(cases, bounds);
    failures += scaling(cases, bounds);
    failures += bounds.report();
    failures += invalid_inputs();
    failures += scale_exponent_ranges();
    failures += extreme_arithmetic(cases);
    failures += device_matches_host(cases);
    failures += cost_report();
    std::printf("material_merged_conductivity: %d failing subitems\n", failures);
    return failures == 0 ? 0 : 1;
}
