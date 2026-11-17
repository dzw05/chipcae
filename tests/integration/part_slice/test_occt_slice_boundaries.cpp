#include "../../../geometry/src/step_shape.hpp"
#include "occt_fixture.hpp"
using namespace slice_test;
int main() {
    return run([] {
        const auto input = fixture(BRepPrimAPI_MakeBox(40, 30, 0.5).Shape());
        // Body lookup must survive unavailable global per-face attribution.
        auto unattributed = input;
        auto source = std::make_shared<detail::StepShape>(*input.source);
        source->names.assign(source->names.size(), "");
        source->faces.Clear();
        unattributed.source = source;
        same(slice(input), slice(unattributed));

        // Exercise imported-scale adjacent sections differing by 0.5 nm.
        BRep_Builder builder;
        TopoDS_Compound adjacent;
        builder.MakeCompound(adjacent);
        builder.Add(adjacent, BRepPrimAPI_MakeBox(0.04, 0.03, 0.00025).Shape());
        builder.Add(adjacent,
                    BRepPrimAPI_MakeBox(gp_Pnt(5e-10, 0, 0.00025), 0.04, 0.03, 0.00025).Shape());
        source->bodies.at(input.body_names[0]) = adjacent;
        const auto merged = single(slice(unattributed));
        near(merged.z0, 0, 1e-12, "merged bottom");
        near(merged.z1, 0.0005, 1e-12, "nanometre-scale contours merge");
        int failures = 0;
        for (const auto& name :
             {std::string{}, std::string("missing"), input.body_names[0] + "X"}) {
            failures += run([&] {
                const auto r = slice_prismatic(input, name, options());
                rejection(r, "body not found");
                same(r, slice_prismatic(input, name, options()));
            });
        }
        failures += run([&] {
            auto renamed = input;
            renamed.body_names = {"CaseSensitive"};
            const auto r = slice_prismatic(renamed, "casesensitive", options());
            rejection(r, "body not found");
            same(r, slice_prismatic(renamed, "casesensitive", options()));
        });
        failures += run([&] {
            StepImportResult empty;
            empty.body_names = {"empty"};
            const auto r = slice_prismatic(empty, "empty", options());
            rejection(r, "empty shape");
            same(r, slice_prismatic(empty, "empty", options()));
        });
        require(failures == 0, "all empty, unknown, case-sensitive and missing-shape cases");
    });
}
