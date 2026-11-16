#pragma once
#include "chipcae/par/gpu_aware.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace chipcae::par {
struct HaloPeer {
    int rank;
    std::vector<std::int32_t> send_indices;
    std::vector<std::int32_t> recv_indices;
};
/// Mesh supplies indices. Peers are sorted by rank; indices retain mesh order.
/// Each directed message pairs sender send_indices with receiver recv_indices.
class HaloPlan {
public:
    explicit HaloPlan(std::vector<HaloPeer> peers);
    const std::vector<HaloPeer>& peers() const noexcept { return peers_; }
    template <class T, class M>
    static std::vector<T> pack(const View1<T, M>& data, std::span<const std::int32_t> indices) {
        static_assert(std::is_trivially_copyable_v<T>);
        std::vector<T> message(indices.size());
        for (std::size_t i = 0; i < indices.size(); ++i) {
            if (indices[i] < 0 || static_cast<std::size_t>(indices[i]) >= data.extent(0))
                throw std::out_of_range("halo send index");
            std::memcpy(&message[i], &data(indices[i]), sizeof(T));
        }
        return message;
    }
    template <class T, class M>
    static void unpack(const View1<T, M>& data,
                       std::span<const std::int32_t> indices,
                       const std::vector<T>& message) {
        if (indices.size() != message.size())
            throw std::invalid_argument("halo count mismatch");
        for (std::size_t i = 0; i < indices.size(); ++i) {
            if (indices[i] < 0 || static_cast<std::size_t>(indices[i]) >= data.extent(0))
                throw std::out_of_range("halo receive index");
            std::memcpy(&data(indices[i]), &message[i], sizeof(T));
        }
    }
    /// Collective in-process transport. Snapshot all sends before any receives,
    /// then deliver in ascending sender/receiver rank order. Device views stage.
    template <class T, class M>
    static void exchange(std::span<const HaloPlan> plans, std::span<const View1<T, M>> fields) {
        if (plans.size() != fields.size())
            throw std::invalid_argument("halo rank count");
        std::vector<StagingBuffer<T>> host;
        for (const auto& field : fields)
            host.push_back(stage_to_host(field));
        struct Message {
            std::size_t target;
            const HaloPeer* peer;
            std::vector<T> data;
        };
        std::vector<Message> messages;
        for (std::size_t rank = 0; rank < plans.size(); ++rank) {
            for (const auto& peer : plans[rank].peers_) {
                if (peer.rank < 0 || static_cast<std::size_t>(peer.rank) >= plans.size())
                    throw std::invalid_argument("halo peer rank");
                const auto target = static_cast<std::size_t>(peer.rank);
                const HaloPeer* reverse = nullptr;
                for (const auto& candidate : plans[target].peers_)
                    if (candidate.rank == static_cast<int>(rank))
                        reverse = &candidate;
                if (!reverse)
                    throw std::invalid_argument("halo missing reciprocal peer");
                auto data = pack(host[rank], peer.send_indices);
                if (data.size() != reverse->recv_indices.size())
                    throw std::invalid_argument("halo count mismatch");
                messages.push_back({target, reverse, std::move(data)});
            }
        }
        for (const auto& m : messages)
            unpack(host[m.target], m.peer->recv_indices, m.data);
        for (std::size_t r = 0; r < fields.size(); ++r)
            stage_from_host(host[r], fields[r]);
    }

private:
    std::vector<HaloPeer> peers_;
};
} // namespace chipcae::par
