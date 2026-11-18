#include "support.hpp"

// Include the implementation to pause acquisition at its private atomic guard,
// without adding test hooks to the production handler.
#include "../../../core/src/crash_diag.cpp"

using namespace crashdiag_test;

int main() {
    return run([] {
        TempDir tmp;
        CrashDiag::install(tmp.path, 0);
        CrashDiag::set_window(9, 1.25);

        // The handler has announced itself but has not yet loaded/pinned a slot.
        // Ordinary code must not recycle snapshots during this acquisition gap.
        check(!chipcae::core::handling.test_and_set(), "handler guard was already set");
        for (const std::uint64_t window : {10000000000000000000ULL, 100ULL, 1000ULL, 10000ULL})
            CrashDiag::set_window(window, 2.5);
        const CrashError error{};
        CrashDiag::on_error(error);
        expect_json(parse(diagnostic(tmp.path, 0, ".json")), 0, error, 9, 1.25);
        empty_signal(tmp.path, 0);
    });
}
