// chipcae/material/state_store.hpp —— 库无关的积分点状态存储（Solver 持有）
#pragma once

#include "chipcae/material/material_batch.hpp"
#include "chipcae/material/material_point.hpp"

#include <Kokkos_Core.hpp>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
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
    std::uint32_t numbering_scheme = 0;
};

// D13：64 位 FNV-1a
inline std::uint64_t
fnv1a64(const void* data, std::size_t bytes, std::uint64_t seed = 14695981039346656037ull) {
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
    QuadratureStateStore(StateLayout layout,
                         std::size_t n_points,
                         std::uint64_t parameter_hash = 0,
                         std::string_view material_id = "",
                         std::uint32_t numbering_scheme = 0)
        : parameter_hash_(parameter_hash), material_id_(material_id),
          numbering_scheme_(numbering_scheme), layout_(layout), n_points_(n_points) {
        committed_ = Soa<Real, MS>("chipcae_state_committed", layout_.n_reals, n_points_);
        trial_ = Soa<Real, MS>("chipcae_state_trial", layout_.n_reals, n_points_);
        Kokkos::deep_copy(committed_, Real{0});
        Kokkos::deep_copy(trial_, Real{0});
    }

    Soa<const Real, MS> committed() const { return committed_; } // 第 n 步，已收敛
    Soa<Real, MS> trial() { return trial_; } // 第 n+1 步；Newton 每次迭代从 committed 重新计算
    void commit() {
        std::swap(committed_, trial_);
        events_prepared_ = false;
    } // 步收敛：交换双缓冲，O(1)
    void discard() noexcept { events_prepared_ = false; } // 步被拒：trial 作废，无需拷贝

    // Event editing is lazy; normal constitutive trial updates overwrite their own fields.
    void prepare_events() {
        if (!events_prepared_) {
            Kokkos::deep_copy(trial_, committed_);
            events_prepared_ = true;
        }
    }
    StateLayout layout() const { return layout_; }
    std::size_t n_points() const { return n_points_; }

    // 只写 committed + layout_hash + 参数哈希 + 点数 + 编号方案
    void save(StateWriter& w) const {
        StateBlobHeader header{};
        header.layout_hash = layout_.layout_hash;
        header.parameter_hash = parameter_hash_;
        header.numbering_scheme = numbering_scheme_;
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
        if (header.parameter_hash != parameter_hash_ ||
            header.numbering_scheme != numbering_scheme_)
            throw std::runtime_error("restore: parameter hash or numbering scheme mismatch");
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
        if (old_store.material_id_ != material_id_ ||
            old_store.layout_.layout_hash != layout_.layout_hash)
            throw std::runtime_error("chipcae::material::transfer_from: layout mismatch");
        auto src = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, old_store.committed_);
        auto dst = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, committed_);
        for (const auto& e : map.entries) {
            const auto k = e.old_points.size();
            if (k < 4 || e.old_coordinates.size() != 3 * k || e.new_point >= n_points_)
                throw std::runtime_error("PointMap: at least four valid 3D neighbors required");
            // Center the affine basis at the destination for improved conditioning.
            Real normal[4][4]{};
            for (std::size_t i = 0; i < k; ++i) {
                if (e.old_points[i] >= old_store.n_points_)
                    throw std::runtime_error("PointMap: old index out of range");
                Real v[4] = {1,
                             e.old_coordinates[3 * i] - e.new_coordinate[0],
                             e.old_coordinates[3 * i + 1] - e.new_coordinate[1],
                             e.old_coordinates[3 * i + 2] - e.new_coordinate[2]};
                for (int a = 0; a < 4; ++a)
                    for (int b = 0; b < 4; ++b)
                        normal[a][b] += v[a] * v[b];
            }
            for (const auto& field : layout_.fields) {
                if (!field.transferable)
                    continue;
                for (std::uint16_t o = 0; o < field.size; ++o) {
                    Real system[4][5]{};
                    for (int a = 0; a < 4; ++a)
                        for (int b = 0; b < 4; ++b)
                            system[a][b] = normal[a][b];
                    for (std::size_t i = 0; i < k; ++i) {
                        Real v[4] = {1,
                                     e.old_coordinates[3 * i] - e.new_coordinate[0],
                                     e.old_coordinates[3 * i + 1] - e.new_coordinate[1],
                                     e.old_coordinates[3 * i + 2] - e.new_coordinate[2]};
                        for (int a = 0; a < 4; ++a)
                            system[a][4] += v[a] * src(field.offset + o, e.old_points[i]);
                    }
                    for (int a = 0; a < 4; ++a) {
                        int pivot = a;
                        for (int b = a + 1; b < 4; ++b)
                            if (std::abs(system[b][a]) > std::abs(system[pivot][a]))
                                pivot = b;
                        if (std::abs(system[pivot][a]) < 1e-24)
                            throw std::runtime_error("PointMap: rank-deficient neighborhood");
                        for (int b = a; b < 5; ++b)
                            std::swap(system[a][b], system[pivot][b]);
                        Real d = system[a][a];
                        for (int b = a; b < 5; ++b)
                            system[a][b] /= d;
                        for (int c = 0; c < 4; ++c)
                            if (c != a) {
                                Real f = system[c][a];
                                for (int b = a; b < 5; ++b)
                                    system[c][b] -= f * system[a][b];
                            }
                    }
                    dst(field.offset + o, e.new_point) = system[0][4];
                }
            }
        }
        Kokkos::deep_copy(committed_, dst);
    }

private:
    bool events_prepared_ = false;
    std::uint64_t parameter_hash_ = 0;
    std::string material_id_;
    std::uint32_t numbering_scheme_ = 0;
    StateLayout layout_{};
    std::size_t n_points_ = 0;
    Soa<Real, MS> committed_;
    Soa<Real, MS> trial_;
};

} // namespace chipcae::material
