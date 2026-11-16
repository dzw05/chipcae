#include "chipcae/coupling/step_loop.hpp"
#include "chipcae/par/column_extrude_partitioner.hpp"
#include "chipcae/par/halo_plan.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
using namespace chipcae;
void require(bool condition) {
    if (!condition)
        throw std::runtime_error("par acceptance failed");
}
template <class T>
void halo(int ranks) {
    std::vector<par::HaloPlan> plans;
    std::vector<View1<T, DeviceSpace>> fields;
    std::vector<std::vector<T>> originals;
    for (int r = 0; r < ranks; ++r) {
        std::vector<par::HaloPeer> peers;
        for (int p = ranks - 1; p >= 0; --p)
            if (p != r)
                peers.push_back(
                    {p, {static_cast<std::int32_t>(p)}, {static_cast<std::int32_t>(ranks + p)}});
        plans.emplace_back(std::move(peers));
        par::StagingBuffer<T> host("source", static_cast<std::size_t>(2 * ranks));
        originals.emplace_back(static_cast<std::size_t>(ranks));
        for (int p = 0; p < ranks; ++p) {
            T value;
            if constexpr (std::is_same_v<T, double>) {
                const std::array<std::uint64_t, 4> bits{0x8000000000000000ULL,
                                                        0x7ff8000000000042ULL,
                                                        0x0000000000000001ULL,
                                                        0x3ff123456789abcdULL};
                value = std::bit_cast<double>(bits[static_cast<std::size_t>((r + p) % 4)]);
            } else
                value = static_cast<T>(r * 100 + p);
            std::memcpy(&host(static_cast<std::size_t>(p)), &value, sizeof(T));
            std::memcpy(&originals.back()[static_cast<std::size_t>(p)], &value, sizeof(T));
        }
        fields.emplace_back("rank", host.extent(0));
        par::stage_from_host(host, fields.back());
    }
    par::HaloPlan::exchange<T, DeviceSpace>(plans, fields);
    for (int r = 0; r < ranks; ++r) {
        auto host = par::stage_to_host(fields[static_cast<std::size_t>(r)]);
        for (int p = 0; p < ranks; ++p)
            if (r != p)
                require(std::memcmp(
                            &host(static_cast<std::size_t>(ranks + p)),
                            &originals[static_cast<std::size_t>(p)][static_cast<std::size_t>(r)],
                            sizeof(T)) == 0);
    }
    // Bounce ghosts back to their original owner using the same transport.
    std::vector<par::HaloPlan> bounce;
    for (const auto& plan : plans) {
        auto peers = plan.peers();
        for (auto& peer : peers)
            std::swap(peer.send_indices, peer.recv_indices);
        bounce.emplace_back(std::move(peers));
    }
    par::HaloPlan::exchange<T, DeviceSpace>(bounce, fields);
    for (int r = 0; r < ranks; ++r) {
        auto host = par::stage_to_host(fields[static_cast<std::size_t>(r)]);
        for (int p = 0; p < ranks; ++p)
            require(
                std::memcmp(&host(static_cast<std::size_t>(p)),
                            &originals[static_cast<std::size_t>(r)][static_cast<std::size_t>(p)],
                            sizeof(T)) == 0);
    }
}
struct TestSolver : Solver {
    std::string_view name() const override { return "test"; }
    char label;
    std::string& calls;
    int& state;
    int cutbacks = 0;
    bool fail = false;
    TestSolver(char l, std::string& c, int& s) : label(l), calls(c), state(s) {}
    StepResult advance(const LoadStep&) override {
        calls += label;
        ++state;
        StepResult r;
        r.status = fail             ? StepResult::Status::Failed
                   : cutbacks-- > 0 ? StepResult::Status::CutBack
                                    : StepResult::Status::Converged;
        r.code = r.status == StepResult::Status::Converged ? ErrorCode::NONE : ErrorCode::INTERNAL;
        return r;
    }
};
struct FunctionSolver : Solver {
    std::string_view name() const override { return "function"; }
    std::function<StepResult(const LoadStep&)> fn;
    StepResult advance(const LoadStep& sub) override { return fn(sub); }
};
StepResult outcome(StepResult::Status status = StepResult::Status::Converged, double hint = 0) {
    StepResult r;
    r.status = status;
    r.code = ErrorCode::NONE;
    r.dt_next_hint = hint;
    return r;
}
void load_regressions() {
    FunctionSolver thermal, mechanical;
    thermal.fn = [](const LoadStep&) { return outcome(); };
    LoadStep parent;
    parent.t1 = 1;
    parent.temperature_t0 = 20;
    parent.temperature = 120;
    parent.shrinkage_increment = 0.125;
    parent.activation_mask_id = 42;
    const std::array steps{parent};
    auto elastic = [&](int cuts) {
        double u = 0, saved = 0, sum = 0;
        int remaining = cuts;
        mechanical.fn = [&](const LoadStep& sub) {
            u += 0.002 * (*sub.temperature - *sub.temperature_t0) + sub.shrinkage_increment;
            if (remaining-- > 0)
                return outcome(StepResult::Status::CutBack);
            sum += sub.shrinkage_increment;
            return outcome();
        };
        coupling::StepLoopPolicy policy;
        policy.save = [&] { saved = u; };
        policy.restore = [&] { u = saved; };
        require(coupling::run_steps(thermal, mechanical, steps, policy).status ==
                StepResult::Status::Converged);
        require(std::abs(sum - parent.shrinkage_increment) / parent.shrinkage_increment <= 1e-15);
        std::cerr << "shrinkage cuts=" << cuts << " sum=" << sum
                  << " parent=" << parent.shrinkage_increment << '\n';
        return u;
    };
    const double full = elastic(0), cut = elastic(2);
    const double relative = std::abs(cut - full) / std::max(std::abs(full), 1e-16);
    require(relative <= 1e-12);
    std::cerr << "elastic relative difference=" << relative << '\n';

    // Start with a thermal cutback to establish a reduced dt, then force two
    // mechanical cutbacks on a later interval. Activation must stay at t0.
    bool first = true;
    int later_cuts = 2, start_masks = 0, later_masks = 0, start_attempts = 0;
    thermal.fn = [&](const LoadStep& sub) {
        if (sub.t0 == parent.t0) {
            ++start_attempts;
            if (sub.activation_mask_id == parent.activation_mask_id)
                ++start_masks;
        } else if (sub.activation_mask_id)
            ++later_masks;
        if (first) {
            first = false;
            return outcome(StepResult::Status::CutBack, 0.5);
        }
        return outcome();
    };
    mechanical.fn = [&](const LoadStep& sub) {
        if (sub.t0 > parent.t0 && later_cuts-- > 0)
            return outcome(StepResult::Status::CutBack);
        return outcome();
    };
    coupling::StepLoopPolicy policy;
    policy.save = [] {};
    policy.restore = [] {};
    require(coupling::run_steps(thermal, mechanical, steps, policy).status ==
            StepResult::Status::Converged);
    require(start_masks > 0 && start_masks == start_attempts && later_masks == 0 && later_cuts < 0);
    std::cerr << "activation start masks=" << start_masks << " later masks=" << later_masks << '\n';

    auto curve = parent;
    curve.temperature_curve_id = 7;
    int curve_calls = 0;
    thermal.fn = [&](const LoadStep& sub) {
        ++curve_calls;
        require(sub.temperature_curve_id == curve.temperature_curve_id && !sub.temperature &&
                !sub.temperature_t0);
        return sub.t1 - sub.t0 > 0.25 ? outcome(StepResult::Status::CutBack, 0.25) : outcome();
    };
    mechanical.fn = [](const LoadStep&) { return outcome(); };
    require(coupling::run_steps(thermal, mechanical, std::span(&curve, 1), policy).status ==
            StepResult::Status::Converged);
    require(curve_calls == 5);

    // A single Prony branch: q' + q/tau = E1*kappa, integrated by
    // backward Euler. The equilibrium branch is E_inf * total strain.
    constexpr double einf = 2, e1 = 3, tau = 0.7, kappa = 0.125;
    const double analytic = einf * kappa + e1 * kappa * tau * (1 - std::exp(-1 / tau));
    auto prony_error = [&](int n) {
        double q = 0, strain = 0, saved_q = 0, saved_strain = 0;
        bool initial = true;
        thermal.fn = [](const LoadStep&) { return outcome(); };
        mechanical.fn = [&](const LoadStep& sub) {
            const double dt = sub.t1 - sub.t0;
            if (initial && dt > 1.0 / n) {
                initial = false;
                return outcome(StepResult::Status::CutBack, 1.0 / n);
            }
            q = (q + e1 * sub.shrinkage_increment) / (1 + dt / tau);
            strain += sub.shrinkage_increment;
            return outcome();
        };
        policy.save = [&] {
            saved_q = q;
            saved_strain = strain;
        };
        policy.restore = [&] {
            q = saved_q;
            strain = saved_strain;
        };
        require(coupling::run_steps(thermal, mechanical, steps, policy).status ==
                StepResult::Status::Converged);
        return std::abs(einf * strain + q - analytic);
    };
    const double e8 = prony_error(8), e16 = prony_error(16), ratio = e8 / e16;
    require(e8 > 0 && e16 > 0 && ratio >= 1.9);
    std::cerr << "Prony e(8)=" << e8 << " e(16)=" << e16 << " ratio=" << ratio << '\n';
}
#if defined(KOKKOS_ENABLE_CUDA) || defined(KOKKOS_ENABLE_HIP)
static_assert(std::is_same_v<par::StagingSpace, Kokkos::SharedHostPinnedSpace>);
#else
static_assert(std::is_same_v<typename par::StagingBuffer<int>::memory_space, HostSpace>);
#endif

void step_loop() {
    std::string calls;
    int state = 0, saved = 0;
    TestSolver thermal('T', calls, state), mechanical('M', calls, state);
    std::array<LoadStep, 2> steps{};
    steps[0].t1 = 1;
    steps[1].t0 = 1;
    steps[1].t1 = 2;
    auto r = coupling::run_steps(thermal, mechanical, steps);
    require(r.status == StepResult::Status::Converged && calls == "TMTM");
    calls.clear();
    state = 0;
    mechanical.cutbacks = 1;
    coupling::StepLoopPolicy policy;
    policy.save = [&] { saved = state; };
    policy.restore = [&] { state = saved; };
    r = coupling::run_steps(thermal, mechanical, std::span(steps).first(1), policy);
    require(r.status == StepResult::Status::Converged && calls == "TMTMTM" && state == 4);
    calls.clear();
    thermal.fail = true;
    r = coupling::run_steps(thermal, mechanical, steps, policy);
    require(r.status == StepResult::Status::Failed && calls == "T" &&
            r.code.code() == ErrorCode::INTERNAL);
    thermal.fail = false;
    mechanical.fail = true;
    calls.clear();
    r = coupling::run_steps(thermal, mechanical, steps, policy);
    require(r.status == StepResult::Status::Failed && calls == "TM");
    mechanical.fail = false;
    thermal.cutbacks = 1;
    calls.clear();
    r = coupling::run_steps(thermal, mechanical, std::span(steps).first(1), policy);
    require(r.status == StepResult::Status::Converged && calls == "TTMTM");
    thermal.cutbacks = 1;
    calls.clear();
    r = coupling::run_steps(thermal, mechanical, steps);
    require(r.status == StepResult::Status::Failed && calls == "T");
    thermal.fail = false;
    calls.clear();
    thermal.cutbacks = 100;
    policy.maximum_cutbacks = 2;
    r = coupling::run_steps(thermal, mechanical, steps, policy);
    require(r.status == StepResult::Status::Failed && calls == "TTT");
}
int main(int argc, char** argv) {
#ifdef CHIPCAE_ENABLE_KOKKOS
    Kokkos::ScopeGuard guard(argc, argv);
#else
    (void)argc;
    (void)argv;
#endif
    for (int parts : {2, 4}) {
        auto partition = par::ColumnExtrudePartitioner::partition(4, 3, 5, parts);
        static_assert(std::is_same_v<decltype(partition)::value_type, std::int32_t>);
        std::vector<int> counts(static_cast<std::size_t>(parts));
        for (std::size_t c = 0; c < 12; ++c) {
            require(partition(c) >= 0 && partition(c) < parts);
            ++counts[static_cast<std::size_t>(partition(c))];
            for (std::size_t k = 0; k < 5; ++k)
                require(partition(k * 12 + c) == partition(c));
        }
        for (int count : counts)
            require(count > 0);
        auto again = par::ColumnExtrudePartitioner::partition(4, 3, 5, parts);
        require(std::memcmp(partition.data(), again.data(), 60 * sizeof(std::int32_t)) == 0);
    }
    for (int ranks : {2, 4}) {
        halo<int>(ranks);
        halo<double>(ranks);
    }
    require(!par::gpu_aware());
    require(par::gpu_aware() == Communicator::world().gpu_aware());
    View1<int, DeviceSpace> device("fallback", 3);
    par::StagingBuffer<int> host("portable host", 3);
    host(0) = 12;
    host(1) = -3;
    host(2) = 42;
    par::stage_from_host(host, device);
    auto copy = par::stage_to_host(device);
    require(std::memcmp(host.data(), copy.data(), 3 * sizeof(int)) == 0);
#ifdef CHIPCAE_ENABLE_METIS
    require(par::ColumnExtrudePartitioner::uses_metis());
#else
    require(!par::ColumnExtrudePartitioner::uses_metis());
#endif
    std::cerr << std::setprecision(17);
    step_loop();
    load_regressions();
    std::cout << "par acceptance passed; METIS=" << par::ColumnExtrudePartitioner::uses_metis()
              << '\n';
}
