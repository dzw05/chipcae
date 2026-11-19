#include "test_support.hpp"

#if CHIPCAE_JOBMGR_LOCAL
#include "manager_support.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <regex>

using namespace jobmgr_test;

namespace {
std::vector<Json> journal(const fs::path& path) {
    const auto bytes = read(path);
    require(bytes.empty() || bytes.back() == '\n', "journal must end with newline");
    std::istringstream input(bytes);
    std::vector<Json> lines;
    for (std::string line; std::getline(input, line);) {
        require(!line.empty(), "empty journal record");
        auto value = Json::parse(line);
        bool in_string = false;
        bool escaped = false;
        for (const unsigned char c : line) {
            if (in_string) {
                if (escaped)
                    escaped = false;
                else if (c == '\\')
                    escaped = true;
                else if (c == '"')
                    in_string = false;
            } else if (c == '"')
                in_string = true;
            else
                require(!std::isspace(c), "journal record must be compact JSON");
        }
        lines.push_back(std::move(value));
    }
    return lines;
}
void command_fields(const Json& line,
                    std::string_view id,
                    std::string_view seq,
                    std::string_view action,
                    std::size_t fields) {
    require(line.size() == fields, "unexpected journal fields");
    require(line.at("issuedAt").is_string() &&
                std::regex_match(line.at("issuedAt").as<std::string>(),
                                 std::regex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z)")),
            "issuedAt must be UTC ISO8601 with milliseconds");
    for (const auto key : {"cmd_seq", "client_request_id", "action"})
        require(line.at(key).is_string(), std::string(key) + " must be a string");
    require(line.at("cmd_seq").as<std::string>() == seq, "sequence string");
    require(line.at("client_request_id").as<std::string>() == id, "request UUID field");
    require(line.at("action").as<std::string>() == action, "stripped action field");
}
} // namespace

int main() {
    Suite suite;
    suite.test("Submit retry preserves run ID, bytes, directories and start count", [] {
        Fixture f;
        auto manager = f.manager();
        const auto request = f.request();
        const auto job = manager->Submit(request, f.case_path, f.profile_path);
        require(job.state() == jobs::JOB_STATE_RUNNING, "initial Submit must be RUNNING");
        const auto run = fs::path(job.result_dir());
        const auto snapshot = read(run / "inputs/case.json");
        const auto disk = read(run / "job.pb");
        const auto count = directory_count(f.config.run_root);
        write(f.case_path, "invalid changed live input");
        const auto retry = manager->Submit(request, f.case_path, f.profile_path);
        require(serialize_job(retry) == serialize_job(job), "retry returned another Job");
        require(directory_count(f.config.run_root) == count && count == 1,
                "retry created directory");
        require(read(run / "inputs/case.json") == snapshot && read(run / "job.pb") == disk,
                "retry rewrote snapshots or Job");
        require(f.fake->starts.size() == 1, "retry launched another process");
        persisted(*manager, retry);
    });
    suite.test("invalid Submit UUID has no run side effects", [] {
        Fixture f;
        auto manager = f.manager();
        const auto before = directory_count(f.config.run_root);
        for (const auto id : {"", "bad", "00000000-0000-0000-0000-000000000000"}) {
            auto request = f.request();
            request.set_client_request_id(id);
            throws_message([&] { manager->Submit(request, f.case_path, f.profile_path); },
                           "client_request_id must be a UUID");
        }
        require(directory_count(f.config.run_root) == before && manager->List({}).jobs_size() == 0,
                "invalid UUID created run");
        require(f.fake->starts.empty(), "invalid UUID launched process");
    });
    suite.test("accepted Control retry is cached before terminal rejection", [] {
        Fixture f;
        auto manager = f.manager();
        const auto job = f.running(*manager);
        const auto request = f.control(job);
        const auto reply = manager->Control(request);
        require(reply.delivery() == jobs::CONTROL_DELIVERY_QUEUED && reply.cmd_seq() == 1,
                "first control queued with sequence one");
        require(reply.message().empty(), "accepted reply message");
        const auto path = fs::path(job.result_dir()) / "control.jsonl";
        const auto bytes = read(path);
        auto changed = request;
        changed.set_action(jobs::CONTROL_ACTION_STOP);
        require(manager->Control(changed).SerializeAsString() == reply.SerializeAsString(),
                "accepted request ID must return original reply");
        manager->Cancel(cancel_request(job));
        const auto terminal = manager->Get(ref(job));
        persisted(*manager, terminal);
        require(manager->Control(request).SerializeAsString() == reply.SerializeAsString(),
                "accepted retry after terminal must return cached reply");
        require(read(path) == bytes, "Control retry appended another line");
        persisted(*manager, terminal);
    });
    suite.test("control journal has exact field types, sequence and interval encoding", [] {
        Fixture f;
        auto manager = f.manager();
        const auto job = f.running(*manager);
        const auto first = f.control(job);
        auto second = f.control(job, jobs::CONTROL_ACTION_SET_OUTPUT_INTERVAL);
        second.set_output_interval_time(0.25);
        auto third = f.control(job, jobs::CONTROL_ACTION_SET_OUTPUT_INTERVAL);
        third.set_output_interval_steps(9007199254740993ULL);
        require(manager->Control(first).cmd_seq() == 1, "first sequence");
        require(manager->Control(second).cmd_seq() == 2, "second sequence");
        require(manager->Control(third).cmd_seq() == 3, "third sequence");
        const auto lines = journal(fs::path(job.result_dir()) / "control.jsonl");
        require(lines.size() == 3, "one journal line per accepted request");
        command_fields(lines[0], first.client_request_id(), "1", "PAUSE", 4);
        command_fields(lines[1], second.client_request_id(), "2", "SET_OUTPUT_INTERVAL", 5);
        command_fields(lines[2], third.client_request_id(), "3", "SET_OUTPUT_INTERVAL", 5);
        require(lines[1].at("output_interval_time").is_number(), "time interval must be number");
        near(lines[1].at("output_interval_time").as<double>(), 0.25);
        require(lines[2].at("output_interval_steps").is_string() &&
                    lines[2].at("output_interval_steps").as<std::string>() == "9007199254740993",
                "step interval must preserve uint64 as decimal string");
        persisted(*manager, job);
    });
    for (const auto action : {jobs::CONTROL_ACTION_PAUSE,
                              jobs::CONTROL_ACTION_RESUME,
                              jobs::CONTROL_ACTION_STOP,
                              jobs::CONTROL_ACTION_WRITE_OUTPUT_NOW,
                              jobs::CONTROL_ACTION_CHECKPOINT_AND_STOP})
        suite.test("action journal encoding " + jobs::ControlAction_Name(action), [&] {
            Fixture f;
            auto manager = f.manager();
            const auto job = f.running(*manager);
            const auto request = f.control(job, action);
            require(manager->Control(request).delivery() == jobs::CONTROL_DELIVERY_QUEUED,
                    "valid action rejected");
            const auto lines = journal(fs::path(job.result_dir()) / "control.jsonl");
            require(lines.size() == 1, "action must append one line");
            command_fields(lines[0],
                           request.client_request_id(),
                           "1",
                           jobs::ControlAction_Name(action).substr(15),
                           4);
        });
    using Edit = std::function<void(jobs::ControlRequest&)>;
    const std::vector<std::pair<std::string, Edit>> rejections{
        {"unspecified action", [](auto& r) { r.set_action(jobs::CONTROL_ACTION_UNSPECIFIED); }},
        {"negative action", [](auto& r) { r.set_action(static_cast<jobs::ControlAction>(-1)); }},
        {"past last action", [](auto& r) { r.set_action(static_cast<jobs::ControlAction>(7)); }},
        {"interval absent",
         [](auto& r) { r.set_action(jobs::CONTROL_ACTION_SET_OUTPUT_INTERVAL); }},
        {"time on other action", [](auto& r) { r.set_output_interval_time(1.0); }},
        {"steps on other action", [](auto& r) { r.set_output_interval_steps(1); }},
        {"zero time",
         [](auto& r) {
             r.set_action(jobs::CONTROL_ACTION_SET_OUTPUT_INTERVAL);
             r.set_output_interval_time(0);
         }},
        {"negative time",
         [](auto& r) {
             r.set_action(jobs::CONTROL_ACTION_SET_OUTPUT_INTERVAL);
             r.set_output_interval_time(-1);
         }},
        {"NaN time",
         [](auto& r) {
             r.set_action(jobs::CONTROL_ACTION_SET_OUTPUT_INTERVAL);
             r.set_output_interval_time(std::numeric_limits<double>::quiet_NaN());
         }},
        {"infinite time",
         [](auto& r) {
             r.set_action(jobs::CONTROL_ACTION_SET_OUTPUT_INTERVAL);
             r.set_output_interval_time(std::numeric_limits<double>::infinity());
         }},
        {"negative infinite time",
         [](auto& r) {
             r.set_action(jobs::CONTROL_ACTION_SET_OUTPUT_INTERVAL);
             r.set_output_interval_time(-std::numeric_limits<double>::infinity());
         }},
        {"zero steps", [](auto& r) {
             r.set_action(jobs::CONTROL_ACTION_SET_OUTPUT_INTERVAL);
             r.set_output_interval_steps(0);
         }}};
    for (const auto& [name, edit] : rejections)
        suite.test("Control rejects " + name + " without caching", [&] {
            Fixture f;
            auto manager = f.manager();
            const auto job = f.running(*manager);
            auto request = f.control(job);
            edit(request);
            const auto reply = manager->Control(request);
            require(reply.delivery() == jobs::CONTROL_DELIVERY_REJECTED && reply.cmd_seq() == 0,
                    "invalid control must be rejected without sequence");
            require(reply.message() == "State/action/interval does not permit control",
                    "rejection message");
            const auto path = fs::path(job.result_dir()) / "control.jsonl";
            require(read(path).empty(), "rejection appended a line");
            request.clear_arg();
            request.set_action(jobs::CONTROL_ACTION_RESUME);
            const auto accepted = manager->Control(request);
            require(accepted.delivery() == jobs::CONTROL_DELIVERY_QUEUED && accepted.cmd_seq() == 1,
                    "rejected request ID was incorrectly cached");
            require(journal(path).size() == 1, "corrected retry must append once");
        });
    suite.test("Control validates UUID and append failure does not consume sequence or cache", [] {
        Fixture f;
        auto manager = f.manager();
        const auto job = f.running(*manager);
        auto request = f.control(job);
        request.set_client_request_id("bad");
        throws_message([&] { manager->Control(request); }, "client_request_id must be a UUID");
        const auto path = fs::path(job.result_dir()) / "control.jsonl";
        require(read(path).empty(), "invalid UUID appended journal");
        request.set_client_request_id(f.id());
        fs::remove(path);
        fs::create_directory(path); // Fails even for root; chmod does not.
        throws_message([&] { manager->Control(request); }, "Control append failed");
        fs::remove(path);
        write(path, "");
        const auto reply = manager->Control(request);
        require(reply.delivery() == jobs::CONTROL_DELIVERY_QUEUED && reply.cmd_seq() == 1,
                "failed append advanced sequence or cached request");
        require(journal(path).size() == 1, "successful retry must append one record");
    });
    suite.test("List orders, filters and emits token only if another matching item exists", [] {
        Fixture f;
        auto manager = f.manager();
        auto first = f.running(*manager);
        auto second = f.running(*manager);
        auto request = f.request();
        (*request.mutable_labels())["project"] = "other";
        const auto third = manager->Submit(request, f.case_path, f.profile_path);
        require(third.state() == jobs::JOB_STATE_RUNNING, "third Submit RUNNING");
        first = manager->Cancel(cancel_request(first));
        persisted(*manager, first);
        const auto all = manager->List({});
        require(all.jobs_size() == 3, "list all count");
        std::vector<std::string> expected{first.run_id(), second.run_id(), third.run_id()};
        std::sort(expected.begin(), expected.end());
        for (int i = 0; i < 3; ++i)
            require(all.jobs(i).run_id() == expected[static_cast<std::size_t>(i)],
                    "run_id ascending order");
        jobs::ListJobsRequest query;
        query.set_page_size(2);
        auto page = manager->List(query);
        require(page.jobs_size() == 2 && page.next_page_token() == expected[1],
                "extra item needs token");
        query.set_page_token(page.next_page_token());
        page = manager->List(query);
        require(page.jobs_size() == 1 && page.jobs(0).run_id() == expected[2] &&
                    page.next_page_token().empty(),
                "last page");
        query.Clear();
        query.set_label_selector("project=demo");
        query.set_page_size(2);
        page = manager->List(query);
        require(page.jobs_size() == 2 && page.next_page_token().empty(),
                "exactly full filtered page has no token");
        query.add_states(jobs::JOB_STATE_RUNNING);
        page = manager->List(query);
        require(page.jobs_size() == 1 && page.jobs(0).run_id() == second.run_id(),
                "combined state/label filters");
        query.Clear();
        query.add_states(jobs::JOB_STATE_RUNNING);
        query.add_states(jobs::JOB_STATE_CANCELLED);
        require(manager->List(query).jobs_size() == 3, "multiple states are alternatives");
        query.Clear();
        query.set_label_selector("missing=value");
        require(manager->List(query).jobs_size() == 0, "missing label must not match");
        for (const auto selector : {"invalid", "a=b=c"}) {
            query.Clear();
            query.set_label_selector(selector);
            throws_message([&] { manager->List(query); }, "label_selector must be key=value");
        }
    });
    suite.test("List rejects malformed selector on empty roots too", [] {
        Fixture f;
        auto manager = f.manager();
        jobs::ListJobsRequest query;
        query.set_label_selector("a=b=c");
        throws_message([&] { manager->List(query); }, "label_selector must be key=value");
    });
    suite.test("all lookup calls reject unknown run ID", [] {
        Fixture f;
        auto manager = f.manager();
        jobs::Job unknown;
        unknown.set_run_id("11111111-1111-4111-8111-111111111111");
        throws_message([&] { manager->Get(ref(unknown)); }, "Unknown run_id");
        throws_message([&] { manager->Cancel(cancel_request(unknown)); }, "Unknown run_id");
        throws_message([&] { manager->ListOutputs(outputs_request(unknown)); }, "Unknown run_id");
        throws_message([&] { manager->Control(f.control(unknown)); }, "Unknown run_id");
    });
    for (const auto& [seconds, expected] : std::vector<std::pair<std::int64_t, std::int64_t>>{
             {-5, 0},
             {30, 30000},
             {120, 60000},
             {std::numeric_limits<std::int64_t>::max(), 60000},
             {std::numeric_limits<std::int64_t>::min(), 0}})
        suite.test("Cancel clamps grace " + std::to_string(seconds), [&] {
            Fixture f;
            auto manager = f.manager();
            const auto job = f.running(*manager);
            f.fake->on_terminate = [&](const auto&) {
                const auto disk = parse_pb<jobs::Job>(fs::path(job.result_dir()) / "job.pb");
                require(disk.state() == jobs::JOB_STATE_CANCELLED,
                        "Cancel must persist before terminate");
            };
            const auto cancelled = manager->Cancel(cancel_request(job, seconds));
            require(cancelled.state() == jobs::JOB_STATE_CANCELLED &&
                        cancelled.ended_at().seconds() > 0,
                    "Cancel terminal state and timestamp");
            require(stamp(cancelled.ended_at()) >= stamp(job.started_at()),
                    "Cancel timestamp order");
            require(f.fake->terminations.size() == 1 &&
                        f.fake->terminations[0].first == job.run_id() &&
                        f.fake->terminations[0].second.count() == expected,
                    "grace passed to terminate");
            persisted(*manager, cancelled);
            const auto bytes = read(fs::path(job.result_dir()) / "job.pb");
            require(serialize_job(manager->Cancel(cancel_request(job))) == serialize_job(cancelled),
                    "terminal Cancel must return same Job");
            require(f.fake->terminations.size() == 1 &&
                        read(fs::path(job.result_dir()) / "job.pb") == bytes,
                    "terminal Cancel has side effects");
            f.fake->on_terminate = {};
        });
    suite.test("ListOutputs maps manifest order, defaults and generation/window cutoffs", [] {
        Fixture f;
        auto manager = f.manager();
        const auto job = f.running(*manager);
        auto request = outputs_request(job);
        auto response = manager->ListOutputs(request);
        require(response.steps_size() == 0 && response.manifest_path().empty() &&
                    response.manifest_generation() == 0,
                "missing manifest empty response");
        write(
            fs::path(job.result_dir()) / "manifest.json",
            R"({"generation":7,"steps":[{"n":2,"time":0.2,"complete":false,"file":"two.vtkhdf","checksum":"sha-two","blocks":["solid","lid"]},{"n":0,"time":0,"file":"zero.vtkhdf","checksum":"sha-zero","blocks":[]},{"n":1,"time":0.1,"file":"one.vtkhdf","checksum":"sha-one","blocks":["solid"]}]})");
        response = manager->ListOutputs(request);
        require(response.manifest_path() == "manifest.json" &&
                    response.manifest_generation() == 7 && response.steps_size() == 3,
                "manifest metadata and count");
        require(response.steps(0).window() == 2 && response.steps(1).window() == 0 &&
                    response.steps(2).window() == 1,
                "manifest step order preserved");
        near(response.steps(0).time(), 0.2);
        near(response.steps(1).time(), 0);
        near(response.steps(2).time(), 0.1);
        require(!response.steps(0).complete() && response.steps(1).complete() &&
                    response.steps(2).complete(),
                "complete defaults true");
        for (int i = 0; i < 3; ++i)
            require(response.steps(i).files_size() == 1, "one file per manifest step");
        require(response.steps(0).files(0).path() == "two.vtkhdf" &&
                    response.steps(0).files(0).sha256() == "sha-two",
                "file/checksum mapping");
        require(response.steps(1).files(0).path() == "zero.vtkhdf" &&
                    response.steps(1).files(0).sha256() == "sha-zero",
                "second file mapping");
        require(response.steps(2).files(0).path() == "one.vtkhdf" &&
                    response.steps(2).files(0).sha256() == "sha-one",
                "third file mapping");
        require(response.steps(0).blocks_size() == 2 && response.steps(0).blocks(0) == "solid" &&
                    response.steps(0).blocks(1) == "lid" && response.steps(1).blocks_size() == 0 &&
                    response.steps(2).blocks(0) == "solid",
                "block mapping and order");
        request.set_since_generation(7);
        response = manager->ListOutputs(request);
        require(response.steps_size() == 0 && response.manifest_generation() == 7 &&
                    response.manifest_path() == "manifest.json",
                "equal generation metadata only");
        request.set_since_generation(8);
        require(manager->ListOutputs(request).steps_size() == 0, "newer generation cutoff");
        request.set_since_generation(6);
        request.set_since_window(0);
        response = manager->ListOutputs(request);
        require(response.steps_size() == 2 && response.steps(0).window() == 2 &&
                    response.steps(1).window() == 1,
                "window excludes n <= cutoff");
        request.set_since_window(2);
        require(manager->ListOutputs(request).steps_size() == 0, "window past all entries");
        persisted(*manager, job);
    });
    return suite.result();
}
#else
int main() {
    std::cout << "SKIP: LocalJobManager not built\n";
    return 77;
}
#endif
