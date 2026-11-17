#include "occt_fixture.hpp"
using namespace slice_test;
int main() {
    return run([] {
        const auto input = fixture(BRepPrimAPI_MakeBox(40, 30, 0.5).Shape());
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
            rejection(slice_prismatic(renamed, "casesensitive", options()), "body not found");
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
