// chipcae/material/state_store.hpp —— 库无关的积分点状态存储（Solver 持有）
#pragma once

#include "chipcae/material/material_batch.hpp"
#include "chipcae/material/material_point.hpp"

#include <Kokkos_Core.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace chipcae::material {

// ---- 主机侧字节流接口（io 层 checkpoint 的抽象）----
class StateWriter {
public:
    virtual ~StateWriter() = default;
    virtual void write(const void* data, std::size_t bytes) = 0;
};

class StateReader {
public:
    virtual ~StateReader() = default;
    virtual void read(void* data, std::size_t bytes) = 0;
};

// D12：POD 字节块头部（格式版本、layout 哈希、字节序标记），不依赖 HDF5
struct StateBlobHeader {
    std::uint32_t magic = 0x4b335354u; // "K3ST"
    std::uint16_t format_version = 1;
    std::uint16_t endian_marker = 0x0102; // 0x0102 => 小端；0x0201 => 大端
    std::uint64_t layout_hash = 0;        // 模型 id + 版本 + 字段表
    std::uint64_t parameter_hash = 0;     // 参数哈希（v0 由主机侧填写）
    std::uint64_t n_points = 0;
    std::uint32_t n_reals = 0;
    std::uint32_t reserved = 0;
};

// D13：64 位 FNV-1a
inline std::uint64_t
fnv1a64(const void* data, std::size_t bytes, std::uint64_t seed = 1469598103934665603ull) {
    const auto* p = static_cast<const unsigned char*>(data);
    std::uint64_t h = seed;
    for (std::size_t i = 0; i < bytes; ++i) {
        h ^= static_cast<std::uint64_t>(p[i]);
        h *= 1099511628211ull;
    }
    return h;
}

inline std::uint16_t host_endian_marker() {
    const std::uint16_t probe = 0x0102;
    unsigned char b[2];
    std::memcpy(b, &probe, 2);
    return (b[0] == 0x02) ? static_cast<std::uint16_t>(0x0102) : static_cast<std::uint16_t>(0x0201);
}

// 重分网映射（D11）：每个新点给出 k 个旧点下标与旧点坐标，v0 只做点级实现。
struct PointMapEntry {
    std::uint32_t new_point = 0;
    std::array<Real, 3> new_coordinate{};
    std::span<const std::uint32_t> old_points{};
    std::span<const Real> old_coordinates{}; // 3 * old_points.size()
};

struct PointMap {
    std::span<const PointMapEntry> entries{};
};

template <class MS> // 双缓冲 View 位于内存空间 MS
class QuadratureStateStore {
public:
    QuadratureStateStore(StateLayout layout, std::size_t n_points)
        : layout_(layout), n_points_(n_points) {
        committed_ = Soa<Real, MS>("chipcae_state_committed", layout_.n_reals, n_points_);
        trial_ = Soa<Real, MS>("chipcae_state_trial", layout_.n_reals, n_points_);
        Kokkos::deep_copy(committed_, Real{0});
        Kokkos::deep_copy(trial_, Real{0});
    }

    Soa<const Real, MS> committed() const { return committed_; } // 第 n 步，已收敛
    Soa<Real, MS> trial() { return trial_; } // 第 n+1 步；Newton 每次迭代从 committed 重新计算
    void commit() { std::swap(committed_, trial_); } // 步收敛：交换双缓冲，O(1)
    void discard() noexcept {}                       // 步被拒：trial 作废，无需拷贝

    StateLayout layout() const { return layout_; }
    std::size_t n_points() const { return n_points_; }

    // 只写 committed + layout_hash + 参数哈希 + 点数 + 编号方案
    void save(StateWriter& w) const {
        StateBlobHeader header{};
        header.layout_hash = layout_.layout_hash;
        header.parameter_hash = 0; // TODO(K3): 由主机侧参数哈希填充
        header.n_points = n_points_;
        header.n_reals = layout_.n_reals;
        header.endian_marker = host_endian_marker();
        w.write(&header, sizeof(header));

        auto host = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, committed_);
        if (layout_.n_reals != 0 && n_points_ != 0) {
            w.write(host.data(),
                    sizeof(Real) * static_cast<std::size_t>(layout_.n_reals) * n_points_);
        }
    }

    // layout_hash 或参数哈希不符则报错，绝不静默
    void restore(StateReader& r) {
        StateBlobHeader header{};
        r.read(&header, sizeof(header));
        if (header.magic != 0x4b335354u)
            throw std::runtime_error("chipcae::material::restore: bad magic");
        if (header.format_version != 1)
            throw std::runtime_error("chipcae::material::restore: unsupported format version");
        if (header.endian_marker != host_endian_marker())
            throw std::runtime_error("chipcae::material::restore: endianness mismatch");
        if (header.layout_hash != layout_.layout_hash)
            throw std::runtime_error("chipcae::material::restore: layout hash mismatch");
        if (header.n_points != n_points_ || header.n_reals != layout_.n_reals)
            throw std::runtime_error("chipcae::material::restore: state shape mismatch");

        auto host = Kokkos::create_mirror_view(Kokkos::HostSpace{}, committed_);
        if (layout_.n_reals != 0 && n_points_ != 0) {
            r.read(host.data(),
                   sizeof(Real) * static_cast<std::size_t>(layout_.n_reals) * n_points_);
        }
        Kokkos::deep_copy(committed_, host);
    }

    // 重分网 / 加密：只转移 transferable 字段，且只在同一 material_id 区域内转移（Q12 已定）
    void transfer_from(const QuadratureStateStore& old_store, const PointMap& map) {
        if (old_store.layout_.layout_hash != layout_.layout_hash)
            throw std::runtime_error("chipcae::material::transfer_from: layout mismatch");
        auto src = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, old_store.committed_);
        auto dst = Kokkos::create_mirror_view(Kokkos::HostSpace{}, committed_);
        for (const auto& e : map.entries) {
            if (e.old_points.empty())
                continue;
            // TODO(K3): 线性局部最小二乘（D11）；v0 先取最近邻保证可运行。
            std::uint32_t best = e.old_points.front();
            Real best_d2 = std::numeric_limits<Real>::max();
            for (std::size_t k = 0; k < e.old_points.size(); ++k) {
                Real d2 = 0;
                for (int c = 0; c < 3; ++c) {
                    const Real d = e.old_coordinates[3 * k + c] - e.new_coordinate[c];
                    d2 += d * d;
                }
                if (d2 < best_d2) {
                    best_d2 = d2;
                    best = e.old_points[k];
                }
            }
            for (const auto& field : layout_.fields) {
                if (!field.transferable)
                    continue;
                for (std::uint16_t o = 0; o < field.size; ++o) {
                    dst(field.offset + o, e.new_point) = src(field.offset + o, best);
                }
            }
        }
        Kokkos::deep_copy(committed_, dst);
    }

private:
    StateLayout layout_{};
    std::size_t n_points_ = 0;
    Soa<Real, MS> committed_;
    Soa<Real, MS> trial_;
};

} // namespace chipcae::material
