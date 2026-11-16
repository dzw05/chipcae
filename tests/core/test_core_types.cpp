#include "chipcae/core/communicator.hpp"
#include "chipcae/core/cv_field.hpp"
#include "chipcae/core/parallel.hpp"
#include "chipcae/core/solver.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {
void expect(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
class TestSolver final : public chipcae::Solver {
public:
    std::string_view name() const override { return "test"; }
    void configure(const chipcae::CaseNode&) override {}
    void setup(chipcae::SimulationContext&) override {}
    chipcae::StepResult advance(const chipcae::LoadStep&, double t0, double t1) override {
        chipcae::StepResult result;
        result.success = true;
        result.failure_reason = chipcae::FailureReason::none;
        result.iterations = 1;
        result.suggested_next_step_size = t1 - t0;
        result.status = chipcae::StepResult::Status::Converged;
        result.nonlinear_iters = 1;
        result.linear_iters = 2;
        result.residual = 0.0;
        result.dt_next_hint = t1 - t0;
        result.code = {};
        return result;
    }
    chipcae::StepResult solve() override { return advance({}, 0.0, 0.0); }
    void checkpoint(chipcae::io::Writer&) const override {}
    void restart(chipcae::io::Reader&) override {}
};
static_assert(!std::is_abstract_v<TestSolver>);
static_assert(chipcae::ErrorCode(chipcae::ErrorCode::INTERNAL).code() == 1);
static_assert(chipcae::ErrorCode(chipcae::ErrorCode::INTERNAL).name() == "INTERNAL");

#ifdef CHIPCAE_ENABLE_KOKKOS
template <class ExecutionSpace>
void exercise() {
    using Memory = typename ExecutionSpace::memory_space;
    static_assert(Kokkos::SpaceAccessibility<ExecutionSpace, Memory>::accessible);
    chipcae::View1<double, Memory> line("line", 4);
    chipcae::CvField<Memory> field({"vector", 4, 2}, {0, 1, 4});
    auto raw = field.raw();
    auto offsets = field.block_offset();
    Kokkos::parallel_for(
        "fill core types", Kokkos::RangePolicy<ExecutionSpace>(0, 4), KOKKOS_LAMBDA(const int cv) {
            line(cv) = cv + 0.5;
            raw(cv, 0) = line(cv);
            raw(cv, 1) = 10.0 + cv + offsets(1);
        });
    ExecutionSpace().fence();
    auto host_line = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, line);
    auto host = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, raw);
    auto host_offsets = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, offsets);
    expect(host_offsets(0) == 0 && host_offsets(1) == 1 && host_offsets(2) == 4, "block offsets");
    for (int cv = 0; cv < 4; ++cv) {
        expect(host_line(cv) == cv + 0.5 && host(cv, 0) == cv + 0.5, "view kernel fill");
        expect(host(cv, 1) == 11.0 + cv, "field kernel fill");
    }
    expect(&host(0, 1) - &host(0, 0) == 4, "LayoutLeft component stride");
    std::cout << "Passed execution space: " << ExecutionSpace::name() << '\n';
}
#else
void exercise_host() {
    chipcae::View1<double, chipcae::HostSpace> line("line", 4);
    auto shared = line;
    shared(2) = 7.0;
    chipcae::View1<const double, chipcae::HostSpace> read = line;
    expect(read(2) == 7.0, "shared owning view and const conversion");
    double buffer[] = {1, 2};
    chipcae::View1<double> borrowed(buffer, 2);
    borrowed(1) = 3;
    expect(buffer[1] == 3 && borrowed.extent(0) == 2, "borrowed view");
    chipcae::CvField<chipcae::HostSpace> field({"vector", 4, 2}, {0, 1, 4});
    auto raw = field.raw();
    for (std::size_t cv = 0; cv < 4; ++cv) {
        raw(cv, 0) = static_cast<double>(cv);
        raw(cv, 1) = 10.0 + static_cast<double>(cv);
    }
    expect(field.raw()(3, 1) == 13 && raw.data()[4] == 10, "field layout and sharing");
    expect(field.block_offset()(1) == 1 && field.block_offset()(2) == 4, "block offsets");
}
#endif
void common() {
    const auto comm = chipcae::Communicator::world();
    expect(comm.rank() == 0 && comm.size() == 1 && comm.is_serial(), "serial communicator");
    expect(!comm.gpu_aware(), "no GPU-aware runtime");
    std::array<double, 3> values = {1.5, -2, 0};
    const auto original = values;
    comm.allreduce_sum(values);
    expect(values == original, "allreduce local identity");
    comm.deterministic_allreduce_sum(values);
    expect(values == original, "deterministic local identity");
    comm.allreduce_sum({});
    comm.deterministic_allreduce_sum({});
    chipcae::CvField<> field({"scalar", 3, 1});
    expect(field.desc().name == "scalar" && field.raw().extent(0) == 3, "descriptor");
    chipcae::CvField<> empty({"empty", 0, 1});
    expect(empty.raw().extent(0) == 0, "empty field");
    for (const auto& offsets : {std::vector<std::int32_t>{1, 3}, {0, 4, 3}, {0, 2}}) {
        bool rejected = false;
        try {
            chipcae::CvField<> invalid({"bad", 3, 1}, offsets);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        expect(rejected, "invalid offsets rejected");
    }
    bool rejected = false;
    try {
        chipcae::CvField<> invalid({"bad", 3, 0});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    expect(rejected, "zero components rejected");
    TestSolver solver;
    chipcae::LoadStep load;
    load.t0 = 1;
    load.t1 = 2;
    load.temperature_curve_id = 7;
    load.shrinkage_increment = 0.01;
    load.activation_mask_id = 3;
    chipcae::Solver& interface = solver;
    const auto result = interface.advance(load, load.t0, load.t1);
    expect(interface.name() == "test" && result.status == chipcae::StepResult::Status::Converged &&
               result.code.code() == 0 && result.dt_next_hint == 1 && result.nonlinear_iters == 1 &&
               result.linear_iters == 2 && result.residual == 0,
           "solver contract");
}
} // namespace
int main(int argc, char** argv) {
#ifdef CHIPCAE_ENABLE_KOKKOS
    Kokkos::ScopeGuard guard(argc, argv);
#else
    (void)argc;
    (void)argv;
#endif
    try {
        common();
#ifdef CHIPCAE_ENABLE_KOKKOS
        exercise<Kokkos::Serial>();
#ifdef KOKKOS_ENABLE_OPENMP
        exercise<Kokkos::OpenMP>();
#endif
        // Compile and allocate both public memory-space specializations.
        chipcae::CvField<chipcae::HostSpace> host({"host", 1, 1});
        chipcae::CvField<chipcae::DeviceSpace> device({"device", 1, 1});
#else
        exercise_host();
#endif
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
