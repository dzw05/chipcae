#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mfem.hpp>
#include <vector>

namespace chipcae::solve::detail {
// RED-phase compile stubs only. No production caller is wired to this header.
inline bool q2_export_requested() {
    return false;
}

struct Q2ExportStats {
    std::size_t elements;
    std::size_t nodes;
    std::uintmax_t bytes;
    double seconds;
};

template <class Gid>
Q2ExportStats write_q2_export(const std::filesystem::path&,
                              mfem::Mesh&,
                              const mfem::FiniteElementSpace&,
                              const mfem::GridFunction&,
                              const mfem::GridFunction&,
                              const std::vector<Gid>&,
                              const std::vector<int>&,
                              int) {
    return {};
}
} // namespace chipcae::solve::detail
