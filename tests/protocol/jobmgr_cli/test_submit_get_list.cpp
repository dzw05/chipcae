#include "test_support.hpp"

#if CHIPCAE_JOBMGR_LOCAL
using namespace jobmgr_cli_test;

int main() {
    Suite suite;
    suite.test("submit retry, get and list across CLI processes", [] {
        Fixture fixture;
        const auto first = fixture.submit();
        const auto repeated = fixture.submit();
        require(repeated.run_id() == first.run_id(), "retry created a different run_id");
        std::size_t directories = 0;
        for (const auto& entry : fs::directory_iterator(fixture.root))
            if (entry.is_directory())
                ++directories;
        require(directories == 1, "retry must leave exactly one run directory");
        const auto got =
            text_message<jobs::Job>(fixture.command({"get", "--run-id", first.run_id()}));
        require(got.run_id() == first.run_id(), "get returned a different run_id");
        const auto listed = text_message<jobs::ListJobsResponse>(fixture.command({"list"}));
        const auto count =
            std::count_if(listed.jobs().begin(), listed.jobs().end(), [&](const auto& job) {
                return job.run_id() == first.run_id();
            });
        require(count == 1, "list must contain submitted run exactly once");
    });
    suite.test("empty list is valid protobuf text", [] {
        Fixture fixture;
        const auto listed = text_message<jobs::ListJobsResponse>(fixture.command({"list"}));
        require(listed.jobs_size() == 0, "new root list must be empty");
    });
    suite.test("empty poll has exact machine readable output", [] {
        Fixture fixture;
        const auto result = fixture.command({"poll"});
        exit_code(result, 0);
        require(result.out == "consumed=0\n", "unexpected poll stdout: " + result.out);
    });
    return suite.result();
}
#else
int main() {
    return 77;
}
#endif
