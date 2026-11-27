#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <jsoncons/json.hpp>
#include <mfem.hpp>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

namespace chipcae::solve::detail {
inline bool q2_export_requested() {
    const char* value = std::getenv("CHIPCAE_ACCURACY_Q2_EXPORT");
    return value && std::string_view(value) == "1";
}

struct Q2ExportStats {
    std::size_t elements;
    std::size_t nodes;
    std::uintmax_t bytes;
    double seconds;
};

template <class Gid>
Q2ExportStats write_q2_export(const std::filesystem::path& file,
                              mfem::Mesh& mesh,
                              const mfem::FiniteElementSpace& scalar,
                              const mfem::GridFunction& displacement,
                              const mfem::GridFunction& temperature,
                              const std::vector<Gid>& elem_gid,
                              const std::vector<int>& partitioning,
                              int rank) {
    static_assert(std::is_integral_v<Gid> && sizeof(Gid) <= sizeof(std::uint64_t));
    const auto start = std::chrono::steady_clock::now();
    const int ne = mesh.GetNE();
    const auto count = static_cast<std::size_t>(ne);
    if (elem_gid.size() != count || partitioning.size() != count)
        throw std::invalid_argument("Q2 export: element array length does not match mesh");
    const auto* vector = displacement.FESpace();
    const auto* thermal = temperature.FESpace();
    if (mesh.Dimension() != 3 || mesh.SpaceDimension() != 3 || scalar.GetMesh() != &mesh ||
        scalar.GetVDim() != 1 || !vector || !thermal || vector->GetMesh() != &mesh ||
        thermal->GetMesh() != &mesh || vector->GetVDim() != 3 || thermal->GetVDim() != 1 ||
        displacement.Size() != vector->GetVSize() || temperature.Size() != thermal->GetVSize())
        throw std::invalid_argument("Q2 export: incompatible mesh or field space");
    // Validate all spaces before MFEM evaluation, including on ranks with no owned cells.
    for (int e = 0; e < ne; ++e)
        for (const auto* space : {&scalar, vector, thermal}) {
            const auto* fe = space->GetFE(e);
            if (!fe || fe->GetGeomType() != mfem::Geometry::CUBE || fe->GetOrder() != 2 ||
                fe->GetDof() != 27 || fe->GetNodes().GetNPoints() != 27 ||
                fe->GetRangeType() != mfem::FiniteElement::SCALAR)
                throw std::invalid_argument("Q2 export: expected scalar Q2 hexahedral elements");
        }
    displacement.HostRead();
    temperature.HostRead();

    Q2ExportStats stats{};
    bool monotonic = true;
    std::optional<Gid> previous;
    for (std::size_t e = 0; e < count; ++e)
        if (partitioning[e] == rank) {
            if (previous && elem_gid[e] <= *previous)
                monotonic = false;
            previous = elem_gid[e];
            ++stats.elements;
        }
    stats.nodes = 27 * stats.elements;
    auto temporary = file;
    temporary += ".tmp";
    // jsoncons' sink flushes in a noexcept destructor. Keep ostream exceptions disabled
    // until the encoder is destroyed, then report any failed write through our exception path.
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream)
        throw std::runtime_error("Q2 export: cannot open temporary file");
    {
        jsoncons::json_options options;
        options.float_format(jsoncons::float_chars_format::general).precision(17);
        jsoncons::compact_json_stream_encoder encoder(stream, options);
        auto finite_value = [&](double value) {
            if (!std::isfinite(value))
                throw std::runtime_error("Q2 export: nonfinite coordinate or field value");
            encoder.double_value(value);
        };
        auto write_element = [&](int e) {
            const auto& nodes = scalar.GetFE(e)->GetNodes();
            std::array<int, 27> order;
            std::iota(order.begin(), order.end(), 0);
            std::sort(order.begin(), order.end(), [&](int a, int b) {
                const auto& p = nodes.IntPoint(a);
                const auto& q = nodes.IntPoint(b);
                return std::tie(p.z, p.y, p.x) < std::tie(q.z, q.y, q.x);
            });
            std::array<std::array<double, 3>, 27> points{}, values{};
            std::array<double, 27> temperatures{};
            mfem::Vector x(3), u(3);
            auto* transform = mesh.GetElementTransformation(e);
            for (std::size_t q = 0; q < order.size(); ++q) {
                const auto& ip = nodes.IntPoint(order[q]);
                transform->Transform(ip, x);
                displacement.GetVectorValue(e, ip, u);
                for (int d = 0; d < 3; ++d) {
                    points[q][static_cast<std::size_t>(d)] = x[d];
                    values[q][static_cast<std::size_t>(d)] = u[d];
                }
                temperatures[q] = temperature.GetValue(e, ip);
            }
            encoder.begin_object();
            encoder.key("gid");
            const auto gid = elem_gid[static_cast<std::size_t>(e)];
            if constexpr (std::is_signed_v<Gid>)
                encoder.int64_value(static_cast<std::int64_t>(gid));
            else
                encoder.uint64_value(static_cast<std::uint64_t>(gid));
            encoder.key("points");
            auto triples = [&](const auto& data) {
                encoder.begin_array();
                for (const auto& row : data) {
                    encoder.begin_array();
                    for (double value : row)
                        finite_value(value);
                    encoder.end_array();
                }
                encoder.end_array();
            };
            triples(points);
            encoder.key("U");
            triples(values);
            encoder.key("T");
            encoder.begin_array();
            for (double value : temperatures)
                finite_value(value);
            encoder.end_array();
            encoder.end_object();
        };
        encoder.begin_object();
        encoder.key("elements");
        encoder.begin_array();
        if (monotonic) {
            for (int e = 0; e < ne; ++e)
                if (partitioning[static_cast<std::size_t>(e)] == rank)
                    write_element(e);
        } else {
            // Unordered GIDs use O(ne * stats.elements) repeated minimum selection to retain
            // constant auxiliary memory (quadratic when this rank owns all elements).
            // Production arrays are normally increasing and take the linear path.
            previous.reset();
            for (std::size_t n = 0; n < stats.elements; ++n) {
                int selected = -1;
                for (int e = 0; e < ne; ++e) {
                    const auto i = static_cast<std::size_t>(e);
                    if (partitioning[i] != rank || (previous && elem_gid[i] <= *previous))
                        continue;
                    if (selected >= 0 &&
                        elem_gid[i] == elem_gid[static_cast<std::size_t>(selected)])
                        throw std::invalid_argument("Q2 export: duplicate owned GID");
                    if (selected < 0 || elem_gid[i] < elem_gid[static_cast<std::size_t>(selected)])
                        selected = e;
                }
                if (selected < 0)
                    throw std::invalid_argument("Q2 export: duplicate owned GID");
                write_element(selected);
                previous = elem_gid[static_cast<std::size_t>(selected)];
            }
        }
        encoder.end_array();
        encoder.end_object();
        encoder.flush();
    }
    stream.flush();
    stream.close();
    if (!stream)
        throw std::runtime_error("Q2 export: failed writing temporary file");
    stats.bytes = std::filesystem::file_size(temporary);
    std::filesystem::rename(temporary, file);
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return stats;
}
} // namespace chipcae::solve::detail
