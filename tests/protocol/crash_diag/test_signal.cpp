#include "support.hpp"

#include <atomic>
#include <thread>

using namespace crashdiag_test;

namespace {
void scenario(int sig, const std::vector<std::uint64_t>& windows) {
    TempDir tmp;
    const pid_t child = ::fork();
    if (child == 0) {
        try {
            disable_core_dumps();
            CrashDiag::install(tmp.path, 1);
            for (const auto window : windows)
                CrashDiag::set_window(window, 1.25);
            ::raise(sig);
            ::_exit(90); // A returning handler cannot satisfy fatal termination.
        } catch (...) {
            ::_exit(91); // Setup failure must not masquerade as the requested signal.
        }
    }
    const int status = wait_child(child);
    fatal_status(status, sig);
    signal_evidence(tmp.path, 1, windows.empty() ? 0 : windows.back(), sig);
}
// A handler on the main thread can pin a snapshot while another thread is
// formatting or publishing a different digit count. Every observed line must
// belong entirely to one completed update.
void concurrent_scenario() {
    TempDir tmp;
    const pid_t child = ::fork();
    if (child == 0) {
        try {
            disable_core_dumps();
            CrashDiag::install(tmp.path, 1);
            constexpr std::uint64_t large = 10000000000000000000ULL;
            CrashDiag::set_window(9, 0.0);
            std::atomic<unsigned int> updates{0};
            std::thread writer([&] {
                for (;;) {
                    CrashDiag::set_window(large, 1.0);
                    CrashDiag::set_window(9, 0.0);
                    updates.fetch_add(1);
                }
            });
            while (updates.load() < 1000)
                std::this_thread::yield();
            ::raise(SIGSEGV);
            ::_exit(90);
        } catch (...) {
            ::_exit(91);
        }
    }
    fatal_status(wait_child(child), SIGSEGV);
    const auto bytes = read(diagnostic(tmp.path, 1, ".signal"));
    const auto suffix = " signal=" + std::to_string(SIGSEGV) + "\n";
    check(bytes == "window=9" + suffix || bytes == "window=10000000000000000000" + suffix,
          "cross-thread signal observed a torn or incomplete snapshot");
    check(!fs::exists(diagnostic(tmp.path, 1, ".json")), "signal unexpectedly wrote JSON");
}
} // namespace

int main() {
    return run([] {
        for (const int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE})
            scenario(sig, {4});
        scenario(SIGSEGV, {});
        scenario(SIGABRT, {});
        scenario(SIGSEGV, {8, 9, 10});
        scenario(SIGABRT, {10, 9});
        scenario(SIGSEGV, {99, 100});
        scenario(SIGSEGV, {1000, 999});
        scenario(SIGABRT, {0, std::numeric_limits<std::uint64_t>::max()});
        for (int i = 0; i < 32; ++i)
            concurrent_scenario();
    });
}
