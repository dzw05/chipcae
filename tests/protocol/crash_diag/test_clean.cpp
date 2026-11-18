#include "support.hpp"

using namespace crashdiag_test;

namespace {
void previous_handler(int) {}
std::size_t descriptor_count() {
    return static_cast<std::size_t>(
        std::distance(fs::directory_iterator("/proc/self/fd"), fs::directory_iterator{}));
}

struct SavedActions {
    const std::array<int, 7> signals{SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE, SIGTERM, SIGINT};
    std::array<struct sigaction, 7> previous{};
    SavedActions() {
        struct sigaction action{};
        action.sa_handler = previous_handler;
        ::sigemptyset(&action.sa_mask);
        ::sigaddset(&action.sa_mask, SIGUSR1);
        action.sa_flags = SA_RESTART;
        for (std::size_t i = 0; i < signals.size(); ++i)
            check(::sigaction(signals[i], &action, &previous[i]) == 0, "save signal action");
    }
    ~SavedActions() {
        CrashDiag::finalize();
        for (std::size_t i = 0; i < signals.size(); ++i)
            ::sigaction(signals[i], &previous[i], nullptr);
    }
    void expect_restored() const {
        for (const int sig : signals) {
            struct sigaction action{};
            check(::sigaction(sig, nullptr, &action) == 0, "query restored signal action");
            check(action.sa_handler == previous_handler, "previous signal handler not restored");
            check((action.sa_flags & SA_RESTART) != 0, "previous signal flags not restored");
            check(::sigismember(&action.sa_mask, SIGUSR1) == 1,
                  "previous signal mask not restored");
        }
    }
};
} // namespace

int main() {
    return run([] {
        CrashDiag::finalize();
        CrashDiag::finalize();
        CrashDiag::set_window(99, 5.0);
        check(!CrashDiag::installed(), "uninstalled setter activated diagnostics");
        expect_throw<std::logic_error>([] { CrashDiag::on_error({}); },
                                       "on_error before install must throw logic_error");
        TempDir tmp;
        const auto dir = tmp.path / "not-created-yet";
        expect_throw<std::invalid_argument>([&] { CrashDiag::install(dir, -1); },
                                            "negative rank must throw invalid_argument");
        check(!CrashDiag::installed(), "negative rank activated diagnostics");
        check(!fs::exists(dir), "negative rank created diagnostic directory");
        SavedActions actions;
        CrashDiag::install(dir, 0);
        check(CrashDiag::installed(), "install must activate diagnostics");
        check(fs::exists(diagnostic(dir, 0, ".signal")), "install must preopen signal file");
        empty_signal(dir, 0);
        for (const int sig : {SIGTERM, SIGINT}) {
            struct sigaction action{};
            check(::sigaction(sig, nullptr, &action) == 0 && action.sa_handler == previous_handler,
                  "install changed graceful shutdown handler");
        }
        CrashDiag::set_window(7, 0.35);
        CrashDiag::finalize();
        check(!CrashDiag::installed(), "finalize must clear installed state");
        check(!fs::exists(diagnostic(dir, 0, ".json")), "clean finalize created JSON");
        check(!fs::exists(diagnostic(dir, 0, ".signal")), "clean finalize left signal file");
        check(!fs::exists(dir / "diag") || fs::is_empty(dir / "diag"), "clean diag not empty");
        actions.expect_restored();

        const auto other = tmp.path / "other";
        CrashDiag::install(dir, 2);
        CrashDiag::set_window(123, 8.0);
        CrashDiag::install(other, 9);
        check(!fs::exists(diagnostic(dir, 2, ".signal")), "reinstall left old signal file");
        CrashDiag::on_error({});
        expect_json(parse(diagnostic(other, 9, ".json")), 9, CrashError{}, 0, 0.0);
        CrashDiag::finalize();
        actions.expect_restored();

        const auto count = descriptor_count();
        for (int i = 0; i < 64; ++i) {
            CrashDiag::install(dir, 0);
            CrashDiag::install(other, 1);
            CrashDiag::finalize();
        }
        check(descriptor_count() == count, "reinstall/finalize leaked descriptors");
        actions.expect_restored();
        // File-system setup failure must not publish a partially installed state.
        const auto blocked = tmp.path / "regular-file";
        std::ofstream(blocked) << "fixture";
        expect_throw<std::exception>([&] { CrashDiag::install(blocked, 0); },
                                     "install under a regular file must fail");
        check(!CrashDiag::installed(), "failed install left installed state");
        check(descriptor_count() == count, "failed install leaked descriptors");
        actions.expect_restored();

        CrashDiag::install(dir, 5);
        std::ofstream(diagnostic(dir, 5, ".signal"), std::ios::app) << "existing evidence\n";
        CrashDiag::finalize();
        check(read(diagnostic(dir, 5, ".signal")) == "existing evidence\n",
              "finalize removed nonempty signal evidence");
    });
}
