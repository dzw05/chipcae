#include "support.hpp"

#include <locale>

using namespace crashdiag_test;

namespace {
struct DecimalComma : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '.'; }
    std::string do_grouping() const override { return "\3"; }
};
struct LocaleGuard {
    std::locale previous;
    LocaleGuard() { std::locale::global(std::locale(previous, new DecimalComma)); }
    ~LocaleGuard() { std::locale::global(previous); }
};
} // namespace

int main() {
    return run([] {
        TempDir tmp;
        CrashError error{"FAILURE_CATEGORY_IO", "INPUT_INVALID", 14, "core", "disk full"};
        CrashDiag::install(tmp.path, 3);
        check(CrashDiag::installed(), "install must activate crash diagnostics");
        CrashDiag::set_window(7, 0.35);
        CrashDiag::on_error(error);
        const auto path = diagnostic(tmp.path, 3, ".json");
        expect_json(parse(path), 3, error, 7, 0.35);
        empty_signal(tmp.path, 3);

        error = {"FAILURE_CATEGORY_INTERNAL", "INTERNAL", 1, "coupling", "second error"};
        CrashDiag::set_window(19, 1.2345678901234567);
        CrashDiag::on_error(error);
        expect_json(parse(path), 3, error, 19, 1.2345678901234567);
        const auto previous = read(path);
        for (const auto* category :
             {"INTERNAL", "failure_category_internal", "", "FAILURE_CATEGORY_X"}) {
            auto invalid = error;
            invalid.category = category;
            expect_throw<std::invalid_argument>([&] { CrashDiag::on_error(invalid); },
                                                "invalid category must throw invalid_argument");
            check(read(path) == previous, "invalid category changed previous JSON");
        }
        CrashDiag::finalize();
        check(read(path) == previous, "finalize removed or changed JSON evidence");
        check(!fs::exists(diagnostic(tmp.path, 3, ".signal")),
              "empty signal file survived finalize");

        CrashDiag::install(tmp.path, 0);
        LocaleGuard locale; // JSON punctuation and numbers must ignore the process locale.
        const std::array<const char*, 11> categories{"FAILURE_CATEGORY_UNSPECIFIED",
                                                     "FAILURE_CATEGORY_CASE_INVALID",
                                                     "FAILURE_CATEGORY_DIVERGENCE",
                                                     "FAILURE_CATEGORY_IO",
                                                     "FAILURE_CATEGORY_MPI",
                                                     "FAILURE_CATEGORY_SCHEDULER",
                                                     "FAILURE_CATEGORY_MEMORY",
                                                     "FAILURE_CATEGORY_GPU",
                                                     "FAILURE_CATEGORY_LAUNCH",
                                                     "FAILURE_CATEGORY_INTERNAL",
                                                     "FAILURE_CATEGORY_JOB_MANAGER"};
        for (const auto* category : categories) {
            error = {category, "", std::numeric_limits<std::uint32_t>::max(), "", ""};
            CrashDiag::on_error(error);
            expect_json(parse(diagnostic(tmp.path, 0, ".json")), 0, error, 0, 0.0);
        }
        check(CrashError{}.category == "FAILURE_CATEGORY_INTERNAL", "wrong default category");
        check(CrashError{}.error_code == 0, "wrong default numeric code");
        check(CrashError{}.code.empty() && CrashError{}.module.empty() &&
                  CrashError{}.message.empty(),
              "wrong default string fields");
        for (const double time : {0.1,
                                  -0.0,
                                  std::numeric_limits<double>::denorm_min(),
                                  std::numeric_limits<double>::max()}) {
            CrashDiag::set_window(std::numeric_limits<std::uint64_t>::max(), time);
            CrashDiag::on_error(error);
            expect_json(parse(diagnostic(tmp.path, 0, ".json")),
                        0,
                        error,
                        std::numeric_limits<std::uint64_t>::max(),
                        time);
        }
        // A failed sibling-temp/rename operation must leave the destination untouched.
        CrashDiag::finalize();
        fs::remove(diagnostic(tmp.path, 0, ".json"));
        fs::create_directory(diagnostic(tmp.path, 0, ".json"));
        std::ofstream(diagnostic(tmp.path, 0, ".json") / "keep") << "prior evidence";
        CrashDiag::install(tmp.path, 0);
        expect_throw<std::exception>([&] { CrashDiag::on_error(error); },
                                     "rename over a directory must report failure");
        check(read(diagnostic(tmp.path, 0, ".json") / "keep") == "prior evidence",
              "failed publication destroyed destination");
        CrashDiag::finalize();
        check(std::distance(fs::directory_iterator(tmp.path / "diag"), fs::directory_iterator{}) ==
                  2,
              "temporary JSON files survived a failed publication");
    });
}
