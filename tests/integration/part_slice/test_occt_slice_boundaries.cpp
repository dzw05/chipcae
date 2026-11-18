#include "occt_fixture.hpp"

#include <TopExp.hxx>
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

        // In-memory geometry uses the stricter synthetic comparison tolerance.
        const auto memory = [&](const TopoDS_Shape& shape) {
            auto result = input;
            auto data = std::make_shared<detail::StepShape>();
            data->shape = shape;
            TopExp::MapShapes(shape, TopAbs_FACE, data->faces);
            data->names.assign(data->faces.Extent(), input.body_names[0]);
            result.source = data;
            return result;
        };
        const auto adjacent = [&](double shift) {
            BRep_Builder builder;
            TopoDS_Compound shape;
            builder.MakeCompound(shape);
            builder.Add(shape, BRepPrimAPI_MakeBox(0.04, 0.03, 0.00025).Shape());
            builder.Add(
                shape, BRepPrimAPI_MakeBox(gp_Pnt(shift, 0, 0.00025), 0.04, 0.03, 0.00025).Shape());
            return memory(shape);
        };
        require(slice(adjacent(5e-10)).slabs.size() == 2,
                "synthetic sections differing by 0.5 nm remain distinct");
        const auto merged = single(slice(adjacent(5e-13)));
        near(merged.z0, 0, 1e-12, "merged bottom");
        near(merged.z1, 0.0005, 1e-12, "sub-picometre synthetic contours merge");

        // Two otherwise valid loops have endpoints within the weld tolerance.
        // Every ordering must reject the ambiguous degree, including early closure.
        for (bool reverse : {false, true}) {
            BRep_Builder builder;
            TopoDS_Compound shape;
            builder.MakeCompound(shape);
            const auto a = BRepPrimAPI_MakeBox(0.04, 0.03, 0.0005).Shape();
            const auto b =
                BRepPrimAPI_MakeBox(gp_Pnt(0.04 + 1e-10, 0.03, 0), 0.04, 0.03, 0.0005).Shape();
            builder.Add(shape, reverse ? b : a);
            builder.Add(shape, reverse ? a : b);
            const auto ambiguous = memory(shape);
            const auto r = slice(ambiguous);
            rejection(r, "section contour failed");
            same(r, slice(ambiguous));
        }
        auto mismatch = input;
        mismatch.body_names = {"unavailable"};
        rejection(slice(mismatch), "empty shape");
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
