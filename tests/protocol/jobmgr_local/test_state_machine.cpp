#include "test_support.hpp"

#if CHIPCAE_JOBMGR_LOCAL
#include "manager_support.hpp"

using namespace jobmgr_test;

int main() {
    Suite suite;
    const auto invalid = [&](std::string name,
                             const std::function<void(Fixture&, jobs::SubmitRequest&)>& edit,
                             std::string message,
                             bool prefix = false) {
        suite.test(name, [&] {
            Fixture f;
            auto request = f.request();
            edit(f, request);
            auto manager = f.manager();
            const auto job = manager->Submit(request, f.case_path, f.profile_path);
            if (message.empty())
                require(!job.failure().message().empty(), "missing input failure has no message");
            failure(job,
                    jobs::FAILURE_CATEGORY_CASE_INVALID,
                    message.empty() ? job.failure().message() : message,
                    -1,
                    0,
                    prefix);
            require(f.fake->starts.empty(), "validation failure launched process");
            require(fs::is_regular_file(fs::path(job.result_dir()) / "control.jsonl"),
                    "failed validation must retain journal");
            const auto list = manager->List({});
            require(list.jobs_size() == 1 && list.jobs(0).run_id() == job.run_id(),
                    "validation failure must remain queryable");
            persisted(*manager, job);
        });
    };
    invalid(
        "missing case is queryable failure",
        [](Fixture& f, auto&) { fs::remove(f.case_path); },
        "");
    invalid(
        "missing profile is queryable failure",
        [](Fixture& f, auto&) { fs::remove(f.profile_path); },
        "");
    invalid(
        "missing referenced input",
        [](Fixture& f, auto&) { fs::remove(f.source / "fcbga_demo.stack.json"); },
        "");
    for (const auto ref : {"/abs", "a/../b", "a\\b", "c:x"})
        invalid(
            std::string("unsafe reference ") + ref,
            [=](Fixture& f, auto&) {
                auto c = f.case_json();
                c["stack"]["ref"] = ref;
                f.case_json(c);
            },
            "Unsafe snapshot reference");
    invalid(
        "missing attachment",
        [](Fixture&, auto& r) { r.add_inputs()->set_logical_name("missing.bin"); },
        "");
    invalid(
        "unsafe hashed path",
        [](Fixture& f, auto&) {
            write(f.source / "fcbga_demo.materials.json",
                  R"({"asset":{"path":"../escape","sha256":"bad"}})");
        },
        "Unsafe snapshot reference");
    invalid(
        "hashed path mismatch",
        [](Fixture& f, auto&) {
            write(
                f.source / "fcbga_demo.materials.json",
                R"({"asset":{"path":"raw.bin","sha256":"0000000000000000000000000000000000000000000000000000000000000000"}})");
            write(f.source / "raw.bin", "abc");
        },
        "INPUT_HASH_MISMATCH");
    invalid(
        "case hash mismatch",
        [](Fixture&, auto& r) { r.mutable_case_file()->set_sha256(std::string(64, '0')); },
        "INPUT_HASH_MISMATCH");
    invalid(
        "reference hash mismatch",
        [](Fixture& f, auto&) {
            auto c = f.case_json();
            c["stack"]["sha256"] = std::string(64, '0');
            f.case_json(c);
        },
        "INPUT_HASH_MISMATCH");
    invalid(
        "attachment hash mismatch",
        [](Fixture& f, auto& r) {
            write(f.source / "blob.bin", "abc");
            auto* input = r.add_inputs();
            input->set_logical_name("blob.bin");
            input->set_sha256(std::string(64, '0'));
        },
        "INPUT_HASH_MISMATCH");
    for (const auto name : {"", "/absolute", "nested/../escape"})
        invalid(
            std::string("unsafe attachment name '") + name + "'",
            [=](Fixture&, auto& r) { r.add_inputs()->set_logical_name(name); },
            "Unsafe input logical_name");
    invalid(
        "case must be object",
        [](Fixture& f, auto&) { write(f.case_path, "[]"); },
        "CASE_INVALID: snapshot validation failed",
        true);
    invalid(
        "case malformed JSON",
        [](Fixture& f, auto&) { write(f.case_path, "{broken"); },
        "CASE_INVALID: snapshot validation failed",
        true);
    invalid(
        "case external parameter schema is enforced",
        [](Fixture& f, auto&) {
            auto c = f.case_json();
            c["solver"] = Json::parse(R"({"linear":{"max_iter":0}})");
            f.case_json(c);
        },
        "CASE_INVALID: snapshot validation failed",
        true);
    invalid(
        "case must satisfy schema",
        [](Fixture& f, auto&) {
            auto c = f.case_json();
            c["mesh"]["nx"] = 0;
            f.case_json(c);
        },
        "CASE_INVALID: snapshot validation failed",
        true);
    invalid(
        "case missing materials reference",
        [](Fixture& f, auto&) {
            auto c = f.case_json();
            c.erase("materials");
            f.case_json(c);
        },
        "CASE_INVALID: snapshot validation failed",
        true);
    invalid(
        "case missing stack reference",
        [](Fixture& f, auto&) {
            auto c = f.case_json();
            c.erase("stack");
            f.case_json(c);
        },
        "CASE_INVALID: snapshot validation failed",
        true);
    invalid(
        "stack must satisfy stack schema",
        [](Fixture& f, auto&) {
            auto stack = Json::parse(read(f.source / "fcbga_demo.stack.json"));
            stack.erase("layers");
            write(f.source / "fcbga_demo.stack.json", stack.to_string());
        },
        "CASE_INVALID: snapshot validation failed",
        true);
    invalid(
        "duplicate IDs across case objects",
        [](Fixture& f, auto&) {
            auto c = f.case_json();
            c["constraints"]["id"] = "mech";
            f.case_json(c);
        },
        "CASE_INVALID: snapshot validation failed",
        true);
    invalid(
        "request profile disagrees with mapped JSON",
        [](Fixture&, auto& r) { *r.mutable_profile() = mapped_profile(3, 2); },
        "RunProfile disagrees with JSON profile");
    suite.test("case external parameter schema resolves locally", [] {
        Fixture f;
        auto c = f.case_json();
        c["solver"] = Json::parse(R"({"linear":{"max_iter":5,"rtol":1e-8}})");
        f.case_json(c);
        auto manager = f.manager();
        f.running(*manager);
    });
    suite.test("x extension IDs do not participate in duplicate checks", [] {
        Fixture f;
        auto c = f.case_json();
        c["x-note"] = Json::parse(R"({"id":"mech","items":[{"id":"mech"}]})");
        f.case_json(c);
        auto manager = f.manager();
        f.running(*manager);
    });
    suite.test("start failure is LAUNCH with launcher error and default exit -1", [] {
        Fixture f;
        f.fake->pid.reset();
        auto manager = f.manager();
        const auto job = f.submit(*manager);
        failure(job, jobs::FAILURE_CATEGORY_LAUNCH, "fake launcher unavailable");
        require(f.fake->starts.size() == 1, "launch failure must attempt start once");
        persisted(*manager, job);
    });
    suite.test("default launcher reports transitional unavailable error", [] {
        Fixture f;
        LocalJobManager manager(f.config);
        const auto job = f.submit(manager);
        failure(job, jobs::FAILURE_CATEGORY_LAUNCH, "process launcher not available in this build");
        persisted(manager, job);
    });
    for (const auto direct : {false, true})
        for (const auto& [code, normal] :
             std::vector<std::pair<int, bool>>{{7, true}, {9, false}, {0, false}})
            suite.test(
                std::string(direct ? "direct" : "MPI") + " abnormal code " + std::to_string(code),
                [&] {
                    Fixture f;
                    if (direct) {
                        auto p = f.profile();
                        p["ranks"] = 1;
                        p["launcher"] = "direct";
                        p.erase("mpi_args");
                        f.profile(p);
                    }
                    auto manager = f.manager();
                    auto job = f.running(*manager);
                    events(job, {chipcae::io::KernelExit{0, 1, "end_reached"}});
                    f.finish(job, code, normal);
                    require(manager->poll() == 1, "one exit consumed");
                    job = manager->Get(ref(job));
                    failure(job,
                            direct ? jobs::FAILURE_CATEGORY_INTERNAL : jobs::FAILURE_CATEGORY_MPI,
                            "Solver/launcher exited abnormally",
                            code,
                            code);
                    persisted(*manager, job);
                });
    suite.test("MPI classification follows actual program equality", [] {
        Fixture f;
        f.config.solver = f.config.mpi;
        auto p = f.profile();
        p["ranks"] = 1;
        p["launcher"] = "direct";
        p.erase("mpi_args");
        f.profile(p);
        auto manager = f.manager();
        const auto job = f.running(*manager);
        f.finish(job, 12);
        manager->poll();
        failure(manager->Get(ref(job)),
                jobs::FAILURE_CATEGORY_MPI,
                "Solver/launcher exited abnormally",
                12,
                12);
    });
    for (const auto kind :
         {"missing", "empty", "no-exit", "truncated", "crc", "magic", "last-exit-failed"})
        suite.test(std::string("zero process exit rejects ") + kind + " events", [&] {
            Fixture f;
            auto manager = f.manager();
            auto job = f.running(*manager);
            const auto path = fs::path(job.result_dir()) / "events.bin";
            const std::string mode(kind);
            if (mode == "empty")
                write(path, "");
            else if (mode == "no-exit")
                events(job, {chipcae::io::Progress{}});
            else if (mode == "last-exit-failed")
                events(job,
                       {chipcae::io::KernelExit{0, 1, "end_reached"},
                        chipcae::io::KernelExit{2, 2, "error"}});
            else if (mode != "missing") {
                events(job, {chipcae::io::KernelExit{0, 1, "end_reached"}});
                std::uint64_t offset = 0;
                require(chipcae::io::EventLogReader(path).read_from(0, &offset).size() == 1 &&
                            offset == fs::file_size(path),
                        "valid pre-corruption event fixture");
                auto bytes = read(path);
                if (mode == "truncated")
                    bytes.pop_back();
                else if (mode == "crc")
                    bytes.back() ^= 1;
                else
                    bytes.front() = 'X';
                write(path, bytes);
            }
            f.finish(job);
            manager->poll();
            job = manager->Get(ref(job));
            failure(job, jobs::FAILURE_CATEGORY_JOB_MANAGER, "Missing successful KernelExit", 0, 0);
            persisted(*manager, job);
        });
    const std::vector<std::pair<std::string, jobs::CompletionKind>> reasons{
        {"checkpoint_and_stop", jobs::COMPLETION_KIND_STOP_AFTER_CHECKPOINT},
        {"user_stop", jobs::COMPLETION_KIND_USER_STOP},
        {"end_reached", jobs::COMPLETION_KIND_END_REACHED},
        {"error", jobs::COMPLETION_KIND_END_REACHED},
        {"", jobs::COMPLETION_KIND_END_REACHED},
        {"future-reason", jobs::COMPLETION_KIND_END_REACHED}};
    for (const auto& [reason, expected] : reasons)
        suite.test("successful KernelExit reason '" + reason + "'", [&] {
            Fixture f;
            auto manager = f.manager();
            auto job = f.running(*manager);
            events(job,
                   {chipcae::io::KernelExit{2, 1, "error"}, chipcae::io::KernelExit{0, 2, reason}});
            f.finish(job);
            manager->poll();
            job = manager->Get(ref(job));
            completed(job, expected);
            persisted(*manager, job);
        });
    for (const auto terminal :
         {jobs::JOB_STATE_COMPLETED, jobs::JOB_STATE_FAILED, jobs::JOB_STATE_CANCELLED})
        suite.test("terminal state remains immutable: " + jobs::JobState_Name(terminal), [&] {
            Fixture f;
            auto manager = f.manager();
            auto job = f.running(*manager);
            if (terminal == jobs::JOB_STATE_CANCELLED)
                job = manager->Cancel(cancel_request(job));
            else {
                events(job, {chipcae::io::KernelExit{0, 1, "end_reached"}});
                f.finish(job, terminal == jobs::JOB_STATE_FAILED ? 5 : 0);
                manager->poll();
                job = manager->Get(ref(job));
            }
            require(job.state() == terminal, "terminal setup");
            persisted(*manager, job);
            const auto bytes = read(fs::path(job.result_dir()) / "job.pb");
            const auto terminated = f.fake->terminations.size();
            f.finish(job, 17, false);
            manager->poll();
            const auto cancelled = manager->Cancel(cancel_request(job));
            const auto reply = manager->Control(f.control(job));
            require(serialize_job(cancelled) == serialize_job(job), "terminal Cancel mutated Job");
            require(reply.delivery() == jobs::CONTROL_DELIVERY_REJECTED,
                    "terminal Control must reject");
            require(reply.message() == "State/action/interval does not permit control",
                    "terminal rejection message");
            require(f.fake->terminations.size() == terminated,
                    "terminal Cancel terminated process");
            require(read(fs::path(job.result_dir()) / "job.pb") == bytes,
                    "terminal disk bytes changed");
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
