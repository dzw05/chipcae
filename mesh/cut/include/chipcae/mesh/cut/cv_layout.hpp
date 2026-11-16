#pragma once
#include "chipcae/core/memory_space.hpp"
#include "chipcae/core/types.hpp"

#include <array>
#include <cstdint>
namespace chipcae::mesh::cut {
using chipcae::BodyId;
using chipcae::Index;
using chipcae::MaterialId;
using chipcae::RegionId;
using chipcae::View1;

// Rank-local flat control-volume index. See design v0.2 §3.2.
struct CvIndex {
    std::int32_t v = -1;
    constexpr operator std::int32_t() const noexcept { return v; }
};

// BlockKey in the high bits, then a block-local cell index whose width is
// parameterized by the block size, then a 3-bit sub-index. Block construction
// must reject an overflow of that packing.
using GlobalCvKey = std::uint64_t;

enum class CellKind : std::uint8_t { Regular = 0, Cut = 1, Covered = 2 };
enum class FaceKind : std::uint8_t { Full = 0, FullInterface = 1, Closed = 2, Split = 3 };

struct Vec3 {
    double x = 0, y = 0, z = 0;
};

// Device-side value type for one control volume.
struct CvAttr {
    RegionId region{};
    MaterialId material{};
    BodyId body{};
    double vol_frac = 0;
    Vec3 centroid{};
    std::uint8_t flags = 0;
};

template <class T, class MemSpace = chipcae::HostSpace>
using CellArray = View1<T, MemSpace>;
template <class T, class MemSpace = chipcae::HostSpace>
using FaceArray = View1<T, MemSpace>;

template <class MemSpace = chipcae::HostSpace>
struct CvSoA {
    View1<const RegionId, MemSpace> region;
    View1<const MaterialId, MemSpace> material;
    View1<const BodyId, MemSpace> body;
    View1<const double, MemSpace> vol_frac;
    View1<const double, MemSpace> centroid[3];
    View1<const std::uint8_t, MemSpace> flags;
    View1<const double, MemSpace> second_moment[6];
    View1<const GlobalCvKey, MemSpace> key;
};

// One copy per rank. Fields are MemSpace-templated flat views; no View-of-Views.
template <class MemSpace>
struct CvTopologyView {
    View1<const std::int64_t, MemSpace> block_cell_offset;
    View1<const std::int32_t, MemSpace> block_cut_offset;
    CellArray<const CellKind, MemSpace> kind;
    CellArray<const RegionId, MemSpace> cell_region;
    CellArray<const MaterialId, MemSpace> cell_material;
    CellArray<const BodyId, MemSpace> cell_body;
    CellArray<const std::int32_t, MemSpace> cut_ord;
    View1<const std::int32_t, MemSpace> cut_slot;
    View1<const std::int32_t, MemSpace> cv_off;
    View1<const CvIndex, MemSpace> cv_list;
    CvSoA<MemSpace> cv;
};

template <class MemSpace>
struct SubfaceSoA {
    View1<const CvIndex, MemSpace> lo_cv, hi_cv;
    View1<const double, MemSpace> area_frac;
    View1<const double, MemSpace> cent[2];
};

template <class MemSpace>
struct FaceTopologyView {
    View1<const std::int64_t, MemSpace> block_face_offset[3];
    FaceArray<const FaceKind, MemSpace> kind[3];
    FaceArray<const std::int32_t, MemSpace> split_ord[3];
    View1<const std::int32_t, MemSpace> sub_off[3];
    SubfaceSoA<MemSpace> sub[3];
};

template <class MemSpace>
struct InterfaceSoA {
    View1<const CvIndex, MemSpace> cv_a, cv_b;
    View1<const double, MemSpace> normal[3];
    View1<const double, MemSpace> area;
    View1<const double, MemSpace> cent[3];
    View1<const std::int32_t, MemSpace> face_group;
    View1<const GlobalCvKey, MemSpace> key;
};

// Layout declarations only. No merging/numbering implementation in K1.
template <class MemSpace>
struct MergeGroupSoA { // 每个合并组一条（未合并 CV 视作单成员组，不单独存）；S6：全部 SoA
    View1<const GlobalCvKey, MemSpace> master;   // 组主 CV：体积最大，平局取 key 较小
    View1<const std::int32_t, MemSpace> mem_off; // 成员 CSR
    View1<const CvIndex, MemSpace> members;
    View1<const double, MemSpace> member_weight; // 体积权重，用于把组解分回基 CV
    View1<const double, MemSpace> volume, centroid[3], second_moment[6];
    View1<const double, MemSpace> bbox_aspect, moment_eig_ratio,
        lsq_cond; // S4：形状质量（长宽比、二阶矩特征值比、最小二乘矩阵条件数）
    View1<const std::int32_t, MemSpace> nbr_off;
    View1<const std::int64_t, MemSpace> nbr; // 组间邻接（求解 CV 全局号）
    View1<const std::int32_t, MemSpace> face_off;
    View1<const std::int32_t, MemSpace>
        face_signed; // M2：引用唯一面表，±(f+1)，负号 = 本组在 hi 侧
    View1<const std::int32_t, MemSpace> int_face_off;
    View1<const std::int8_t, MemSpace> int_face_dir;
    View1<const std::int32_t, MemSpace> int_face_idx; // 组内被消去的面
    View1<const std::int32_t, MemSpace> eb_off;
    View1<const std::int32_t, MemSpace> eb;       // 组内嵌入边界面片（界面序号）
    View1<const double, MemSpace> eb_area_vec[3]; // 汇总面积矢量，指向固体（C2）
};

template <class MemSpace>
struct UniqueFaceTable { // M2：合并后全局唯一面表；朝同一邻居组的多个子面按子面保留（二阶 FVM
                         // 需要）
    View1<const std::int64_t, MemSpace> lo, hi; // 两侧求解 CV 全局号；外边界或壁面 hi = -1
    View1<const double, MemSpace> area, normal[3], cent[3]; // 面积按面唯一（C1）；法向由 lo 指向 hi
};

template <class MemSpace>
struct SolveNumbering {       // M3：合并后求解 CV 的连续全局编号；确定性，按 SFC 序
    Index row_begin, row_end; // 本 rank 连续区间 [row_begin, row_end)，供 ParCSR 零拷贝与 BoomerAMG
    View1<const Index, MemSpace>
        gid; // Future disc::CvField<HYPRE_BigInt, MemSpace>.    // 按 CvIndex；ghost 部分交换后填好
};

} // namespace chipcae::mesh::cut
