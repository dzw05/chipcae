#include "support.hpp"

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
    });
}
