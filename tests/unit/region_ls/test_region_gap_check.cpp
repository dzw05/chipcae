#include "support.hpp"
using namespace sp5;
int main() {
    return run([] {
        // Frozen API has no error return: compare the exception's leading error-name token exactly.
        auto check = [](StackModel s, bool reject, const char* first, const char* second) {
            auto m = samples(32);
            bodies(m, s);
            bool caught = false;
            try {
                (void)region_level_sets(s, m, {});
            } catch (const std::exception& e) {
                caught = true;
                std::string message = e.what();
                auto end = message.find_first_of(": \t\n");
                require(message.substr(0, end) == "CUT_UNSUPPORTED",
                        "exact error code must be CUT_UNSUPPORTED");
                require(message.find(first) != std::string::npos &&
                            message.find(second) != std::string::npos,
                        "gap diagnostic must name offending regions or outline");
            }
            require(caught == reject, "gap acceptance threshold");
        };
        for (bool same_material : {false, true})
            for (double gap : {.375, .625}) {
                auto s = stack();
                auto a = square("a", 2, 2, 3, 3), b = square("b", 3 + gap, 2, 4 + gap, 3);
                b.material = same_material ? a.material : "other";
                s.layers[0].regions = {a, b};
                check(s, gap == .375, "a", "b");
            }
        auto s = stack();
        s.outline.polygon = {{0, 0}, {8, 0}, {8, 8}, {0, 8}};
        s.layers[0].regions = {square("edge", .375, 2, 2, 4)};
        check(s, true, "edge", "outline");
        s = stack();
        s.layers[0].material = "background";
        s.layers[0].regions = {square("in_solid", 2, 2, 6, 6)};
        check(s, true, "", "");
    });
}
