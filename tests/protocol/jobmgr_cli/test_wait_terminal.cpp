#include "test_support.hpp"

#if CHIPCAE_JOBMGR_LOCAL
using namespace jobmgr_cli_test;

int main() {
    Suite suite;
    suite.test(
        "submit --wait completes the noisy solver and persists success across processes", [] {
            Fixture fixture;
            auto arguments = fixture.submit_args();
            arguments.insert(arguments.end(), {"--wait", "--timeout-ms", "60000"});
            const auto submission = cli(arguments);
            const auto submitted = text_message<jobs::Job>(submission);
            require(chipcae::jobmgr::is_uuid(submitted.run_id()), "submit run_id must be a UUID");
            require(submitted.client_request_id() == request_id,
                    "request-id must be preserved exactly");
            completed(submitted, submitted.run_id());
            require(submission.err.find("synthetic solver progress: 100%") != std::string::npos,
                    "solver progress must be redirected to stderr");
            const auto got =
                text_message<jobs::Job>(fixture.command({"get", "--run-id", submitted.run_id()}));
            completed(got, submitted.run_id());
            const auto waited = text_message<jobs::Job>(
                fixture.command({"wait", "--run-id", submitted.run_id(), "--timeout-ms", "0"}));
            completed(waited, submitted.run_id());
            // The successful solver flushed its noise before its KernelExit. Read again
            // after completion to catch late output inherited from the submit invocation.
            const auto final_submission = text_message<jobs::Job>(submission.refresh());
            require(final_submission.run_id() == submitted.run_id(), "submit output changed");
            const auto cancelled = text_message<jobs::Job>(
                fixture.command({"cancel", "--run-id", submitted.run_id(), "--grace-ms", "1234"}));
            completed(cancelled, submitted.run_id());
        });
    suite.test("submit without --wait cannot own the slow solver across CLI processes", [] {
        Fixture fixture;
        const auto submitted = text_message<jobs::Job>(
            cli(fixture.submit_args(), {{"CHIPCAE_CLI_MOCK_SLEEP_MS", "30000"}}));
        require(submitted.state() == jobs::JOB_STATE_RUNNING, "submit must return a running Job");
        const auto got =
            text_message<jobs::Job>(fixture.command({"get", "--run-id", submitted.run_id()}));
        require(got.run_id() == submitted.run_id(), "get returned a different run_id");
        std::cout << "Job state=" << jobs::JobState_Name(got.state())
                  << " failure=" << jobs::FailureCategory_Name(got.failure().category()) << '\n';
        require(got.state() == jobs::JOB_STATE_FAILED &&
                    got.failure().category() == jobs::FAILURE_CATEGORY_JOB_MANAGER,
                "recovery must fail a Job without process ownership");
    });
    for (const auto& options : {std::vector<std::string>{},
                                {"--timeout-ms", "0"},
                                {"--timeout-ms", "-1"},
                                {"--timeout-ms", "-9223372036854775808"}}) {
        suite.test("submit --wait uses nonpositive timeout as a single poll", [&] {
            Fixture fixture;
            auto arguments = fixture.submit_args();
            arguments.push_back("--wait");
            arguments.insert(arguments.end(), options.begin(), options.end());
            const auto job =
                text_message<jobs::Job>(cli(arguments, {{"CHIPCAE_CLI_MOCK_SLEEP_MS", "30000"}}));
            require(job.state() == jobs::JOB_STATE_RUNNING,
                    "nonpositive submit --wait must return before the slow solver finishes");
        });
    }
    return suite.result();
}
#else
int main() {
    return 77;
}
#endif
