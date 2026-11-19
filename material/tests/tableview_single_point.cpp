#include "chipcae/material/table_view.hpp"

#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

using namespace chipcae::material;

namespace {
struct Query {
    int column;
    Real temperature;
    bool initial_clamped = false;
    bool use_clamped = true;
};

struct Result {
    StatusCode status;
    Real value;
    bool clamped;
};

const char* status_name(StatusCode status) {
    switch (status) {
    case StatusCode::Ok:
        return "Ok";
    case StatusCode::OutOfRange:
        return "OutOfRange";
    case StatusCode::Failed:
        return "Failed";
    default:
        return "unexpected status";
    }
}

const char* mode_name(Extrapolation mode) {
    return mode == Extrapolation::Error ? "Error" : "Clamp";
}

template <class ExecSpace, class MemSpace>
auto evaluate(const TableView<MemSpace>& table, const std::vector<Query>& queries) {
    const int count = static_cast<int>(queries.size());
    Kokkos::View<Query*, MemSpace> inputs("evaluation_inputs", count);
    auto host_inputs = Kokkos::create_mirror_view(inputs);
    for (int i = 0; i < count; ++i)
        host_inputs(i) = queries[static_cast<std::size_t>(i)];
    Kokkos::deep_copy(inputs, host_inputs);
    Kokkos::View<Result*, MemSpace> outputs("evaluation_outputs", count);
    Kokkos::parallel_for(
        "single_point_evaluate", Kokkos::RangePolicy<ExecSpace>(0, count), KOKKOS_LAMBDA(int i) {
            const auto query = inputs(i);
            Real value = 0;
            bool clamped = query.initial_clamped;
            const auto status =
                query.use_clamped ? table.evaluate(query.column, query.temperature, value, &clamped)
                                  : table.evaluate(query.column, query.temperature, value);
            outputs(i) = {status, value, clamped};
        });
    return Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, outputs);
}

struct SinglePointChecks {
    bool device = true;
    bool host_match = true;
};

template <class ExecSpace, class MemSpace>
SinglePointChecks single_point(const TemperatureTable& host_table,
                               const std::vector<Real>& temperatures) {
    std::vector<Query> queries;
    const int columns = static_cast<int>(host_table.num_columns());
    for (int column = 0; column < columns; ++column)
        for (Real temperature : temperatures) {
            queries.push_back({column, temperature});
            // The optional flag must remain untouched, even when already true.
            queries.push_back({column, temperature, true});
            queries.push_back({column, temperature, false, false});
        }
    const auto results = evaluate<ExecSpace>(TableView<MemSpace>(host_table), queries);
    SinglePointChecks checks;
    for (std::size_t i = 0; i < queries.size(); ++i) {
        const auto& query = queries[i];
        const auto result = results(i);
        const auto column = static_cast<std::size_t>(query.column);
        checks.device &= result.status == StatusCode::Ok &&
                         result.value == host_table.columns()[column][0] &&
                         result.clamped == query.initial_clamped;
        if (!query.use_clamped || query.initial_clamped)
            continue;

        // Warnings are sticky: use a fresh host table for each comparison.
        const auto reference = host_table;
        Real host_value = 0;
        const auto host_status = reference.evaluate(column, query.temperature, host_value);
        checks.host_match &= result.status == host_status && host_status == StatusCode::Ok &&
                             result.value == host_value &&
                             result.clamped == reference.clamped_warning();
        if (query.temperature == 200) {
            std::cout << "MEASURE single_point mode=" << mode_name(host_table.extrapolation())
                      << " columns=" << columns << " column=" << column
                      << " T=" << query.temperature << " status=" << status_name(result.status);
            if (result.status == StatusCode::Ok)
                std::cout << " value=" << result.value;
            std::cout << " clamped=" << result.clamped
                      << " host_status=" << status_name(host_status) << " host_value=" << host_value
                      << " host_warning=" << reference.clamped_warning() << '\n';
        }
    }
    return checks;
}

template <class ExecSpace, class MemSpace>
bool bad_input(Extrapolation mode) {
    const TemperatureTable table({300}, {{1e9}, {0.3}}, Interpolation::Linear, mode);
    const std::vector<Query> queries{{0, std::numeric_limits<Real>::quiet_NaN()},
                                     {0, std::numeric_limits<Real>::infinity()},
                                     {0, -std::numeric_limits<Real>::infinity()},
                                     {-1, 300},
                                     {2, 300}};
    const auto results = evaluate<ExecSpace>(TableView<MemSpace>(table), queries);
    bool ok = true;
    for (std::size_t i = 0; i < queries.size(); ++i)
        ok &= results(i).status == StatusCode::Failed;
    const auto empty = evaluate<ExecSpace>(TableView<MemSpace>{}, {{0, 300}});
    return ok && empty(0).status == StatusCode::Failed;
}

template <class ExecSpace, class MemSpace>
bool integral_single(Extrapolation mode) {
    constexpr Real value = 1e-5;
    const TemperatureTable host_table({300}, {{value}}, Interpolation::Linear, mode);
    const TableView<MemSpace> table(host_table);
    constexpr std::array<std::array<Real, 2>, 5> intervals{
        {{200, 250}, {250, 400}, {350, 450}, {400, 250}, {300, 300}}};
    constexpr int count = static_cast<int>(intervals.size());
    Kokkos::View<Real* [2], MemSpace> inputs("integral_inputs", count);
    auto host_inputs = Kokkos::create_mirror_view(inputs);
    for (int i = 0; i < count; ++i)
        for (int j = 0; j < 2; ++j)
            host_inputs(i, j) = intervals[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)];
    Kokkos::deep_copy(inputs, host_inputs);
    Kokkos::View<Result*, MemSpace> outputs("integral_outputs", count);
    Kokkos::parallel_for(
        "single_point_integral", Kokkos::RangePolicy<ExecSpace>(0, count), KOKKOS_LAMBDA(int i) {
            Real result = 0;
            const auto status = table.integral(0, inputs(i, 0), inputs(i, 1), result);
            outputs(i) = {status, result, false};
        });
    const auto results = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, outputs);
    bool ok = true;
    for (int i = 0; i < count; ++i) {
        const auto& interval = intervals[static_cast<std::size_t>(i)];
        const Real expected = (interval[1] - interval[0]) * value;
        const auto result = results(i);
        std::cout << "MEASURE integral mode=" << mode_name(mode) << " a=" << interval[0]
                  << " b=" << interval[1] << " status=" << status_name(result.status)
                  << " result=" << result.value << " expected=" << expected << '\n';
        ok &= result.status == StatusCode::Ok &&
              std::abs(result.value - expected) <=
                  4 * std::numeric_limits<Real>::epsilon() * std::abs(expected);
        if (interval[1] < interval[0])
            ok &= result.status == StatusCode::Ok && result.value < 0;
        if (interval[1] == interval[0])
            ok &= result.status == StatusCode::Ok && result.value == 0;
    }
    return ok;
}

template <class ExecSpace, class MemSpace>
bool two_point(Extrapolation mode) {
    const TemperatureTable table({250, 400}, {{1e9, 2e9}}, Interpolation::Linear, mode);
    const std::vector<Query> queries{{0, 200}, {0, 250}, {0, 325}, {0, 400}, {0, 450}};
    constexpr std::array<Real, 5> expected{1e9, 1e9, 1.5e9, 2e9, 2e9};
    const auto results = evaluate<ExecSpace>(TableView<MemSpace>(table), queries);
    bool ok = true;
    for (std::size_t i = 0; i < queries.size(); ++i) {
        const bool outside = i == 0 || i == queries.size() - 1;
        const auto expected_status =
            outside && mode == Extrapolation::Error ? StatusCode::OutOfRange : StatusCode::Ok;
        const auto result = results(i);
        ok &= result.status == expected_status;
        if (expected_status == StatusCode::Ok)
            ok &= result.status == StatusCode::Ok && result.value == expected[i];
        ok &= result.clamped == (outside && mode == Extrapolation::Clamp);
        std::cout << "MEASURE two_point mode=" << mode_name(mode) << " T=" << queries[i].temperature
                  << " status=" << status_name(result.status);
        if (result.status == StatusCode::Ok)
            std::cout << " value=" << result.value;
        std::cout << " clamped=" << result.clamped << '\n';
    }
    return ok;
}

template <class ExecSpace, class MemSpace>
int run() {
    std::cout << "BACKEND " << ExecSpace::name() << '\n';
    constexpr std::array<const char*, 7> names{"device_error",
                                               "device_clamp",
                                               "device_multi_column",
                                               "device_bad_input",
                                               "host_device_match",
                                               "integral_single",
                                               "two_point_unchanged"};
    std::array<bool, names.size()> passed;
    passed.fill(true);
    for (auto mode : {Extrapolation::Error, Extrapolation::Clamp}) {
        const TemperatureTable table({300}, {{1e9}, {0.3}}, Interpolation::Linear, mode);
        const auto checks = single_point<ExecSpace, MemSpace>(
            table, {300, 200, 400, 300 - 1e-9, 300 + 1e-9, -1e30, 1e30});
        passed[mode == Extrapolation::Error ? 0 : 1] &= checks.device;
        passed[4] &= checks.host_match;
        const TemperatureTable orthotropic(
            {300}, {{1e-5}, {2e-5}, {3e-5}}, Interpolation::Linear, mode);
        const auto multi_checks = single_point<ExecSpace, MemSpace>(orthotropic, {200});
        passed[2] &= multi_checks.device;
        passed[4] &= multi_checks.host_match;
        passed[3] &= bad_input<ExecSpace, MemSpace>(mode);
        passed[5] &= integral_single<ExecSpace, MemSpace>(mode);
        passed[6] &= two_point<ExecSpace, MemSpace>(mode);
    }
    int failures = 0;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (!passed[i]) {
            std::cerr << "FAIL material_tableview_single_point: " << names[i] << '\n';
            ++failures;
        } else {
            std::cout << "PASS material_tableview_single_point: " << names[i] << '\n';
        }
    }
    return failures;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2 || (std::string_view(argv[1]) != "host" && std::string_view(argv[1]) != "cuda")) {
        std::cerr << "usage: material_tableview_single_point host|cuda\n";
        return 2;
    }
    const std::string_view mode(argv[1]);
    Kokkos::ScopeGuard guard(argc, argv);
    std::cout << std::setprecision(17);
    int failures = 0;
    if (mode == "host") {
        using HostExec = Kokkos::DefaultHostExecutionSpace;
        failures += run<HostExec, HostExec::memory_space>();
#if defined(KOKKOS_ENABLE_SERIAL)
        failures += run<Kokkos::Serial, Kokkos::Serial::memory_space>();
#endif
#if defined(KOKKOS_ENABLE_OPENMP)
        failures += run<Kokkos::OpenMP, Kokkos::OpenMP::memory_space>();
#endif
    } else {
#if defined(KOKKOS_ENABLE_CUDA)
        failures += run<Kokkos::Cuda, Kokkos::CudaSpace>();
#else
        std::cout << "SKIP: CUDA Kokkos unavailable\n";
        return 77;
#endif
    }
    return failures == 0 ? 0 : 1;
}
