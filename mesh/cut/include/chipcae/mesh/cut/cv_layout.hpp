#pragma once
#include "chipcae/core/memory_space.hpp"
#include "chipcae/core/types.hpp"

#include <array>
namespace chipcae::mesh::cut {
// Layout declarations only. GlobalCvKey packs block, parameterized cell bits, 3 sub bits;
// block construction must validate overflow. No merging/numbering implementation in K1.
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
