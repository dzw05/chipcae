#include "chipcae/par/halo_plan.hpp"
#include "chipcae/par/column_extrude_partitioner.hpp"
#include "chipcae/coupling/step_loop.hpp"
#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
using namespace chipcae;
void require(bool condition) { if (!condition) throw std::runtime_error("par acceptance failed"); }
template<class T> void halo(int ranks) {
    std::vector<par::HaloPlan> plans;
    std::vector<View1<T, DeviceSpace>> fields;
    std::vector<std::vector<T>> originals;
    for (int r=0;r<ranks;++r) {
        std::vector<par::HaloPeer> peers;
        for (int p=ranks-1;p>=0;--p) if (p!=r)
            peers.push_back({p,{static_cast<std::size_t>(p)}, {static_cast<std::size_t>(ranks+p)}});
        plans.emplace_back(std::move(peers));
        par::StagingBuffer<T> host("source", static_cast<std::size_t>(2*ranks));
        originals.emplace_back(static_cast<std::size_t>(ranks));
        for (int p=0;p<ranks;++p) {
            T value;
            if constexpr (std::is_same_v<T,double>) {
                const std::array<std::uint64_t,4> bits{0x8000000000000000ULL,0x7ff8000000000042ULL,
                    0x0000000000000001ULL,0x3ff123456789abcdULL};
                value=std::bit_cast<double>(bits[static_cast<std::size_t>((r+p)%4)]);
            } else value=static_cast<T>(r*100+p);
            std::memcpy(&host(static_cast<std::size_t>(p)), &value, sizeof(T));
            std::memcpy(&originals.back()[static_cast<std::size_t>(p)], &value, sizeof(T));
        }
        fields.emplace_back("rank", host.extent(0));
        par::stage_from_host(host, fields.back());
    }
    par::HaloPlan::exchange<T,DeviceSpace>(plans,fields);
    for (int r=0;r<ranks;++r) {
        auto host=par::stage_to_host(fields[static_cast<std::size_t>(r)]);
        for (int p=0;p<ranks;++p) if (r!=p)
            require(std::memcmp(&host(static_cast<std::size_t>(ranks+p)),
                &originals[static_cast<std::size_t>(p)][static_cast<std::size_t>(r)],sizeof(T))==0);
    }
    // Bounce ghosts back to their original owner using the same transport.
    std::vector<par::HaloPlan> bounce;
    for (const auto& plan : plans) {
        auto peers=plan.peers();
        for (auto& peer : peers) std::swap(peer.send_indices,peer.recv_indices);
        bounce.emplace_back(std::move(peers));
    }
    par::HaloPlan::exchange<T,DeviceSpace>(bounce,fields);
    for (int r=0;r<ranks;++r) {
        auto host=par::stage_to_host(fields[static_cast<std::size_t>(r)]);
        for (int p=0;p<ranks;++p) require(std::memcmp(&host(static_cast<std::size_t>(p)),
            &originals[static_cast<std::size_t>(r)][static_cast<std::size_t>(p)],sizeof(T))==0);
    }
}
struct TestSolver : Solver {
    char label;
    std::string& calls;
    int& state;
    int cutbacks=0;
    bool fail=false;
    TestSolver(char l,std::string& c,int& s):label(l),calls(c),state(s) {}
    StepResult solve() override { return {}; }
    StepResult advance(const LoadStep&,double,double) override {
        calls+=label;
        ++state;
        StepResult r;
        r.status= fail ? StepResult::Status::Failed : cutbacks-- >0 ? StepResult::Status::CutBack : StepResult::Status::Converged;
        r.success=r.status==StepResult::Status::Converged;
        r.code=r.success ? ErrorCode::NONE : ErrorCode::INTERNAL;
        return r;
    }
};
void step_loop() {
    std::string calls;
    int state=0, saved=0;
    TestSolver thermal('T',calls,state), mechanical('M',calls,state);
    std::array<LoadStep,2> steps{};
    steps[0].t1=1; steps[1].t0=1; steps[1].t1=2;
    auto r=coupling::run_steps(thermal,mechanical,steps);
    require(r.status==StepResult::Status::Converged && calls=="TMTM");
    calls.clear(); state=0; mechanical.cutbacks=1;
    coupling::StepLoopPolicy policy;
    policy.save=[&]{saved=state;}; policy.restore=[&]{state=saved;};
    r=coupling::run_steps(thermal,mechanical,std::span(steps).first(1),policy);
    require(r.status==StepResult::Status::Converged && calls=="TMTMTM" && state==4);
    calls.clear(); thermal.fail=true;
    r=coupling::run_steps(thermal,mechanical,steps,policy);
    require(r.status==StepResult::Status::Failed && calls=="T" && r.code.code()==ErrorCode::INTERNAL);
    thermal.fail=false; mechanical.fail=true; calls.clear();
    r=coupling::run_steps(thermal,mechanical,steps,policy);
    require(r.status==StepResult::Status::Failed && calls=="TM");
    mechanical.fail=false; thermal.cutbacks=1; calls.clear();
    r=coupling::run_steps(thermal,mechanical,std::span(steps).first(1),policy);
    require(r.status==StepResult::Status::Converged && calls=="TTMTM");
    thermal.cutbacks=1; calls.clear();
    r=coupling::run_steps(thermal,mechanical,steps);
    require(r.status==StepResult::Status::Failed && calls=="T");
    thermal.fail=false; calls.clear(); thermal.cutbacks=100;
    policy.maximum_cutbacks=2;
    r=coupling::run_steps(thermal,mechanical,steps,policy);
    require(r.status==StepResult::Status::Failed && calls=="TTT");
}
int main(int argc,char** argv) {
#ifdef CHIPCAE_ENABLE_KOKKOS
    Kokkos::ScopeGuard guard(argc,argv);
#else
    (void)argc; (void)argv;
#endif
    for (int parts : {2,4}) {
        auto partition=par::ColumnExtrudePartitioner::partition(4,3,5,parts);
        static_assert(std::is_same_v<decltype(partition)::value_type,std::int32_t>);
        std::vector<int> counts(static_cast<std::size_t>(parts));
        for (std::size_t c=0;c<12;++c) {
            require(partition(c)>=0 && partition(c)<parts);
            ++counts[static_cast<std::size_t>(partition(c))];
            for (std::size_t k=0;k<5;++k) require(partition(k*12+c)==partition(c));
        }
        for (int count:counts) require(count>0);
        auto again=par::ColumnExtrudePartitioner::partition(4,3,5,parts);
        require(std::memcmp(partition.data(),again.data(),60*sizeof(std::int32_t))==0);
    }
    for (int ranks : {2,4}) { halo<int>(ranks); halo<double>(ranks); }
    require(!par::gpu_aware());
    require(par::gpu_aware()==Communicator::world().gpu_aware());
    View1<int,DeviceSpace> device("fallback",3);
    par::StagingBuffer<int> host("portable host",3);
    host(0)=12;host(1)=-3;host(2)=42;
    par::stage_from_host(host,device);
    auto copy=par::stage_to_host(device);
    require(std::memcmp(host.data(),copy.data(),3*sizeof(int))==0);
    step_loop();
    std::cout << "par acceptance passed; METIS=" << par::ColumnExtrudePartitioner::uses_metis() << '\n';
}
