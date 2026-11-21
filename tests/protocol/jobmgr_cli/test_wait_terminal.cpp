#include "test_support.hpp"

#if CHIPCAE_JOBMGR_LOCAL
using namespace jobmgr_cli_test;

int main() {
    Suite suite;
    suite.test("CLI submit then CLI wait completes the noisy successful solver", [] {
        Fixture fixture;
        const auto submission = cli(fixture.submit_args());
        const auto submitted = text_message<jobs::Job>(submission);
        require(chipcae::jobmgr::is_uuid(submitted.run_id()), "submit run_id must be a UUID");
        const auto waited = text_message<jobs::Job>(
            fixture.command({"wait", "--run-id", submitted.run_id(), "--timeout-ms", "60000"}));
        completed(waited, submitted.run_id());
        // The successful solver flushed its noise before its KernelExit. Read again
        // after completion to catch late output inherited from the submit invocation.
        const auto final_submission = text_message<jobs::Job>(submission.refresh());
        require(final_submission.run_id() == submitted.run_id(), "submit output changed");
        const auto cancelled = text_message<jobs::Job>(
            fixture.command({"cancel", "--run-id", submitted.run_id(), "--grace-ms", "1234"}));
        completed(cancelled, submitted.run_id());
    });
    return suite.result();
}
#else
int main() {
    return 77;
}
#endif
