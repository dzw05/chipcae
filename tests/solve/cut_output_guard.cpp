#include "chipcae/mesh/background/tensor_grid.hpp"
#include "chipcae/mesh/cut/plane_cut.hpp"
#include "chipcae/solvers/fem/cut_fem.hpp"
#include "cut_output.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <iostream>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace chipcae;
using mesh::Local;
using mesh::cut::CutPolyhedra;

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct Fixture {
    mesh::HexMeshArrays mesh;
    mesh::cut::CutLayerArrays cut;

    Fixture() {
        mesh::StackModel stack;
        stack.outline.box = {0, 0, 1.5, 1};
        for (const auto name : {"solid", "second"}) {
            mesh::Layer layer;
            layer.name = layer.material = name;
            layer.kind = "Explicit";
            layer.thickness = .5;
            layer.n_elem_min = 1;
            stack.layers.push_back(layer);
        }
        mesh::GridOptions options;
        options.nx = 3;
        options.ny = 2;
        options.levels = 1;
        options.max_ratio = 100;
        mesh = mesh::build_tensor_grid(stack, options).value();

        mesh::cut::CutGeometry geometry;
        for (const auto& [name, body] : mesh.attr_table.bodies) {
            geometry.planes.push_back({body, {1, 0, 0}, 1.125});
            geometry.planes.push_back({body, {-1, 0, 0}, -.125});
        }
        mesh::cut::CutOptions cut_options;
        cut_options.export_polyhedra = true;
        cut = mesh::cut::build_cut_layer(mesh, geometry, cut_options).value();
        mesh.partitioning.assign(mesh.elem_gid.size(), 0);
    }

    Local attribute_layer(Local parent) const {
        const auto& record = mesh.attr_table.entries.at(mesh.attribute.at(parent) - 1);
        return mesh.attr_table.layers.at(record.layer);
    }

    io::ResultsStep output() {
        mfem::Mesh grid(mesh.vertices.data(),
                        mesh.node_gid.size(),
                        mesh.hex.data(),
                        mfem::Geometry::CUBE,
                        mesh.attribute.data(),
                        mesh.attribute.size(),
                        mesh.bdr_quad.data(),
                        mfem::Geometry::SQUARE,
                        mesh.bdr_attribute.data(),
                        mesh.bdr_attribute.size(),
                        3,
                        3);
        grid.Finalize(false, false);
        mfem::H1_FECollection collection(2, 3);
        mfem::FiniteElementSpace scalar(&grid, &collection);
        mfem::FiniteElementSpace vector(&grid, &collection, 3, mfem::Ordering::byVDIM);
        mfem::GridFunction T(&scalar), U(&vector);
        T = 0.;
        U = 0.;
        io::ResultsStep step;
        solve::append_cut_output(step, mesh, cut, T, U, 0);
        return step;
    }

    void construct_penalty() const {
        fem::GhostPenalty penalty(
            mesh, cut, std::vector<double>(mesh.attr_table.entries.size(), 1.0), .1);
    }

    Local first_endpoint_partition(std::size_t f) const {
        const auto it =
            std::find(mesh.elem_gid.begin(), mesh.elem_gid.end(), cut.gp_elem_gid[0].at(f));
        return mesh.partitioning.at(it - mesh.elem_gid.begin());
    }
};

// Exact output captured at 6aefb47. The four cells on each cutting plane
// share point order and face rings, translated by 0.5 in y and/or z.
void check_single_output(const io::ResultsStep& step) {
    using Points = std::array<std::array<double, 3>, 8>;
    const Points left = {{{.125, .5, 0},
                          {.5, .5, 0},
                          {.5, 0, 0},
                          {.125, 0, 0},
                          {.125, 0, .5},
                          {.5, 0, .5},
                          {.5, .5, .5},
                          {.125, .5, .5}}};
    const Points right = {{{1, 0, 0},
                           {1, .5, 0},
                           {1.125, .5, 0},
                           {1.125, 0, 0},
                           {1, 0, .5},
                           {1.125, 0, .5},
                           {1.125, .5, .5},
                           {1, .5, .5}}};
    const std::array<int, 24> left_faces = {0, 1, 2, 3, 4, 5, 6, 7, 3, 2, 5, 4,
                                            7, 6, 1, 0, 2, 1, 6, 5, 7, 0, 3, 4};
    const std::array<int, 24> right_faces = {0, 1, 2, 3, 4, 5, 6, 7, 0, 3, 5, 4,
                                             1, 7, 6, 2, 0, 4, 7, 1, 2, 6, 5, 3};
    std::vector<double> points;
    std::vector<Index> face_conn;
    for (int cell = 0; cell < 8; ++cell) {
        const double y = .5 * ((cell % 4) / 2), z = .5 * (cell % 2);
        for (auto [px, py, pz] : (cell < 4 ? left : right))
            points.insert(points.end(), {px, py + y, pz + z});
        for (int v : (cell < 4 ? left_faces : right_faces))
            face_conn.push_back(cell * 8 + v);
    }
    auto sequence = [](Index count, Index stride) {
        std::vector<Index> values;
        for (Index i = 0; i < count; ++i)
            values.push_back(i * stride);
        return values;
    };
    const auto& cut = step.cut;
    require(cut.points == points, "cut.points changed");
    require(cut.T == std::vector<double>(64, 0.), "cut.T changed");
    require(cut.U == std::vector<double>(192, 0.), "cut.U changed");
    require(cut.volume ==
                std::vector<double>{.09375, .09375, .09375, .09375, .03125, .03125, .03125, .03125},
            "cut.volume changed");
    require(cut.connectivity == sequence(64, 1), "cut.connectivity changed");
    require(cut.offsets == sequence(9, 8), "cut.offsets changed");
    require(cut.face_conn == face_conn, "cut.face_conn changed");
    require(cut.face_off == sequence(49, 4), "cut.face_off changed");
    require(cut.poly_faces == sequence(48, 1), "cut.poly_faces changed");
    require(cut.poly_off == sequence(9, 6), "cut.poly_off changed");
    require(cut.hex.empty(), "cut.hex changed");
    require(cut.elem_gid == std::vector<Index>{0, 1, 4, 5, 8, 9, 10, 11}, "cut.elem_gid changed");
    const std::vector<Local> layers{1, 2, 1, 2, 1, 2, 1, 2};
    require(cut.layer_id == layers, "cut.layer_id changed");
    require(cut.material_id == layers, "cut.material_id changed");
    require(cut.cell_kind == std::vector<Local>(8, 1), "cut.cell_kind changed");
    require(step.embedded_boundary.layer_id ==
                std::vector<Local>{1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2},
            "embedded_boundary.layer_id changed");
}

// Copy each selected cell with its own point IDs and face rings, including when
// two entries in the requested order refer to the same source cell.
CutPolyhedra select_cells(const CutPolyhedra& src, const std::vector<std::size_t>& order) {
    CutPolyhedra dst;
    for (auto cell : order) {
        std::map<Index, Index> point_ids;
        for (Index i = src.poly_off[cell]; i < src.poly_off[cell + 1]; ++i) {
            const auto face = src.poly_faces[i];
            dst.poly_faces.push_back(dst.face_off.size() - 1);
            for (Index j = src.face_off[face]; j < src.face_off[face + 1]; ++j) {
                const auto old = src.face_conn[j];
                auto [it, inserted] = point_ids.emplace(old, dst.point_parent_elem.size());
                if (inserted) {
                    for (int d = 0; d < 3; ++d) {
                        dst.points.push_back(src.points[3 * old + d]);
                        dst.point_xi.push_back(src.point_xi[3 * old + d]);
                    }
                    dst.point_parent_elem.push_back(src.point_parent_elem[old]);
                }
                dst.face_conn.push_back(it->second);
            }
            dst.face_off.push_back(dst.face_conn.size());
        }
        dst.poly_off.push_back(dst.poly_faces.size());
        dst.parent_elem_gid.push_back(src.parent_elem_gid[cell]);
        dst.body.push_back(src.body[cell]);
        dst.layer_id.push_back(src.layer_id[cell]);
        dst.material_id.push_back(src.material_id[cell]);
        dst.cell_kind.push_back(src.cell_kind[cell]);
        dst.volume.push_back(src.volume[cell]);
        dst.volfrac_ref.push_back(src.volfrac_ref[cell]);
    }
    return dst;
}

void prepend_clone(Fixture& fixture) {
    auto& p = *fixture.cut.polyhedra;
    const auto gid = fixture.mesh.elem_gid.at(fixture.cut.eb_parent_elem.front());
    const auto it = std::find(p.parent_elem_gid.begin(), p.parent_elem_gid.end(), gid);
    require(it != p.parent_elem_gid.end(), "fixture EB parent has no polyhedron to clone");
    std::vector<std::size_t> order(p.volume.size() + 1);
    order.front() = it - p.parent_elem_gid.begin();
    std::iota(order.begin() + 1, order.end(), std::size_t{0});
    p = select_cells(p, order);
}

template <class Action>
void expect_error(Action action, std::initializer_list<std::string_view> fragments) {
    try {
        action();
    } catch (const std::runtime_error& error) {
        for (auto fragment : fragments)
            require(std::string_view(error.what()).find(fragment) != std::string_view::npos,
                    "wrong diagnostic, missing '" + std::string(fragment) + "' in '" +
                        error.what() + "'");
        return;
    } catch (const std::out_of_range& error) {
        throw std::runtime_error(std::string("wrong exception type, got std::out_of_range: ") +
                                 error.what());
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string("wrong exception type: ") + error.what());
    }
    throw std::runtime_error("missing exception, expected std::runtime_error");
}

int run_group(std::string_view group) {
    int failures = 0;
    auto run = [&](const char* name, auto test) {
        try {
            Fixture fixture;
            if (group == "eb_layer") {
                require(fixture.cut.polyhedra && !fixture.cut.polyhedra->volume.empty(),
                        "fixture needs exported polyhedra");
                const auto vertices = fixture.cut.eb_parent_elem.size();
                require(vertices > 0 && vertices % 3 == 0, "fixture needs complete EB triangles");
            } else {
                require(!fixture.cut.gp_face_gid.empty(), "fixture needs ghost faces");
            }
            test(fixture);
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "FAIL " << name << ": " << error.what() << '\n';
        }
    };

    if (group == "eb_layer") {
        run("eb_layer_single", [](Fixture& f) {
            const auto step = f.output();
            check_single_output(step);
            const auto& parents = f.cut.eb_parent_elem;
            const auto& layers = step.embedded_boundary.layer_id;
            require(layers.size() == parents.size() / 3, "expected one layer per EB triangle");
            for (std::size_t i = 0; i < layers.size(); ++i)
                require(layers[i] == f.attribute_layer(parents[3 * i]),
                        "incorrect EB triangle layer");
        });
        run("eb_layer_multi_conflict", [](Fixture& f) {
            prepend_clone(f);
            ++f.cut.polyhedra->layer_id.front();
            expect_error([&] { f.output(); },
                         {"cut_output: parent gid ", "has polyhedra in layers"});
        });
        run("eb_layer_multi_same", [](Fixture& f) {
            const auto single = f.output().embedded_boundary.layer_id;
            prepend_clone(f);
            require(f.output().embedded_boundary.layer_id == single, "duplicate changed EB layers");
        });
        run("eb_layer_no_match", [](Fixture& f) {
            const auto gid = f.mesh.elem_gid.at(f.cut.eb_parent_elem.front());
            auto& p = *f.cut.polyhedra;
            std::vector<std::size_t> keep;
            for (std::size_t i = 0; i < p.volume.size(); ++i)
                if (p.parent_elem_gid[i] != gid)
                    keep.push_back(i);
            require(keep.size() < p.volume.size(), "fixture has no polyhedron to remove");
            p = select_cells(p, keep);
            expect_error([&] { f.output(); },
                         {"cut_output: EB triangle parent gid ", "has no polyhedron"});
        });
        run("eb_layer_attr_mismatch", [](Fixture& f) {
            const auto parent = f.cut.eb_parent_elem.front();
            const auto gid = f.mesh.elem_gid.at(parent);
            const auto wrong = f.attribute_layer(parent) + 1;
            auto& p = *f.cut.polyhedra;
            for (std::size_t i = 0; i < p.volume.size(); ++i)
                if (p.parent_elem_gid[i] == gid)
                    p.layer_id[i] = wrong;
            expect_error([&] { f.output(); },
                         {"cut_output: EB triangle parent gid ", "!= attribute layer"});
        });
    } else {
        run("ghost_ok", [](Fixture& f) {
            f.construct_penalty();
            auto& m = f.mesh;
            for (std::size_t e = 0; e < m.elem_gid.size(); ++e) {
                const double x0 = m.vertices[3 * m.hex[8 * e]];
                const double x1 = m.vertices[3 * m.hex[8 * e + 6]];
                m.partitioning[e] = (x0 + x1) / 2 >= .75;
            }
            auto& owners = f.cut.gp_owner_rank;
            for (std::size_t i = 0; i < f.cut.gp_face_gid.size(); ++i)
                owners.at(i) = f.first_endpoint_partition(i);
            for (Local rank : {0, 1})
                require(std::find(owners.begin(), owners.end(), rank) != owners.end(),
                        "split fixture needs ghost faces owned by both ranks");
            f.construct_penalty();
        });
        run("ghost_missing_elem", [](Fixture& f) {
            const auto& gids = f.mesh.elem_gid;
            f.cut.gp_elem_gid[1].at(0) = *std::max_element(gids.begin(), gids.end()) + 1000;
            expect_error(
                [&] { f.construct_penalty(); },
                {"GhostPenalty: ghost face ", "not on this process", "PETSC_COMM_SELF only"});
        });
        run("ghost_owner_mismatch", [](Fixture& f) {
            f.cut.gp_owner_rank.at(0) = f.first_endpoint_partition(0) + 1;
            expect_error([&] { f.construct_penalty(); },
                         {"GhostPenalty: ghost face ", "owner_rank", "!= partitioning"});
        });
        run("ghost_size_mismatch", [](Fixture& f) {
            require(!f.cut.gp_owner_rank.empty(), "fixture needs an owner to remove");
            f.cut.gp_owner_rank.pop_back();
            expect_error([&] { f.construct_penalty(); },
                         {"GhostPenalty: ghost face arrays size mismatch"});
        });
    }
    return failures == 0 ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2 ||
        (std::string_view(argv[1]) != "eb_layer" && std::string_view(argv[1]) != "ghost")) {
        std::cerr << "expected group eb_layer or ghost\n";
        return 2;
    }
    MPI_Init(&argc, &argv);
    const int status = run_group(argv[1]);
    MPI_Finalize();
    return status;
}
