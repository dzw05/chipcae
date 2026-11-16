#include "chipcae/par/halo_plan.hpp"

#include <algorithm>
#include <set>
namespace chipcae::par {
HaloPlan::HaloPlan(std::vector<HaloPeer> peers) : peers_(std::move(peers)) {
    std::sort(
        peers_.begin(), peers_.end(), [](const auto& a, const auto& b) { return a.rank < b.rank; });
    std::set<std::int32_t> destinations;
    int previous = -1;
    for (const auto& peer : peers_) {
        if (peer.rank < 0 || peer.rank == previous)
            throw std::invalid_argument("invalid halo peer");
        previous = peer.rank;
        for (auto index : peer.recv_indices)
            if (!destinations.insert(index).second)
                throw std::invalid_argument("duplicate halo destination");
    }
}
} // namespace chipcae::par
