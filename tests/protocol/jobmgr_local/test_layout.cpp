#include "test_support.hpp"

#if CHIPCAE_JOBMGR_LOCAL
#include "manager_support.hpp"

#include <algorithm>
#include <cctype>
#include <tuple>

using namespace jobmgr_test;

int main() {
    Suite suite;
    suite.test("Submit persists complete run layout and original bytes", [] {
        Fixture f;
        // Exercise absolute normalization without changing the CTest working directory.
        f.config.run_root = fs::relative(f.config.run_root, fs::current_path());
        auto manager = f.manager();
        auto request = f.request();
        *request.mutable_profile() = mapped_profile();
        request.mutable_case_file()->set_sha256(
            "6933690CA67EBED3B7A7989056D8C26F167D1FF2D3D6058306E41094B8FB47AC");
        auto job = manager->Submit(request, f.case_path, f.profile_path);
        require(job.state() == jobs::JOB_STATE_RUNNING, "Submit must transition to RUNNING");
        require(chipcae::jobmgr::is_uuid(job.run_id()) && job.run_id().size() == 36,
                "run_id is an unbraced UUID");
        require(job.client_request_id() == request.client_request_id(), "request ID preserved");
        require(job.name() == request.name() && job.attempt() == 1, "name/attempt metadata");
        const auto user = std::getenv("USER");
        require(job.owner() == (user ? user : ""), "owner from USER");
        require(job.labels().at("project") == "demo", "original labels");
        require(job.labels().at("desktop.solver") == "chipcae-solve", "solver label");
        require(job.native().scheduler() == jobs::SCHEDULER_TYPE_LOCAL, "native scheduler");
        require(job.native().native_job_id() == "4242", "PID mapped to native_job_id");
        require(job.profile().SerializeAsString() == mapped_profile().SerializeAsString(),
                "mapped profile fields");
        require(job.submitted_at().seconds() > 0 && job.started_at().seconds() > 0,
                "submission/start timestamps");
        require(stamp(job.started_at()) >= stamp(job.submitted_at()), "timestamp order");
        require(!job.has_ended_at(), "RUNNING must not have ended_at");
        const fs::path run(job.result_dir());
        require(run.is_absolute() &&
                    fs::equivalent(run, fs::absolute(f.config.run_root) / job.run_id()),
                "absolute result directory");
        require(job.result_dir_local(), "local result directory flag");
        for (const auto file : {"job.pb",
                                "submit.pb",
                                "control.jsonl",
                                "run.json",
                                "inputs/case.json",
                                "snapshot-hashes.json"})
            require(fs::is_regular_file(run / file), std::string("missing ") + file);
        require(read(run / "control.jsonl").empty(), "initial journal must be zero bytes");
        persisted(*manager, job);
        require(read(run / "submit.pb") == request.SerializeAsString(), "original submit bytes");
        require(parse_pb<jobs::SubmitRequest>(run / "submit.pb").SerializeAsString() ==
                    request.SerializeAsString(),
                "submit protobuf parse");
        const auto hashes = Json::parse(read(run / "snapshot-hashes.json"));
        const std::vector<std::tuple<fs::path, fs::path, std::string>> snapshots{
            {f.case_path,
             run / "inputs/case.json",
             "6933690ca67ebed3b7a7989056d8c26f167d1ff2d3d6058306e41094b8fb47ac"},
            {f.profile_path,
             run / "run.json",
             "a4b6666faa98beea4a288e1cbcbd7b44c52dfd5267dd9b528ecf905035841cad"},
            {f.source / "fcbga_demo.stack.json",
             run / "inputs/fcbga_demo.stack.json",
             "db79e596ad6a1260378e9ce58b16ddf418645dd785f99dbc96d23f6d85d711d2"},
            {f.source / "fcbga_demo.materials.json",
             run / "inputs/fcbga_demo.materials.json",
             "a41bcdf01dcacc8f22c03f3d7642b1cf95657dcac88ca773b22f344b5254dd04"}};
        require(hashes.size() == snapshots.size(), "every snapshot has exactly one hash");
        for (const auto& [source, target, hash] : snapshots) {
            require(read(source) == read(target), "snapshot raw bytes preserved");
            require(hashes.at(target.string()).as<std::string>() == hash, "snapshot hash mismatch");
        }
        require(job.case_sha256() ==
                    "6933690ca67ebed3b7a7989056d8c26f167d1ff2d3d6058306e41094b8fb47ac",
                "case hash uses raw snapshot bytes");
        require(f.fake->starts.size() == 1 &&
                    f.fake->starts.front().second.env_overrides.at("OMP_NUM_THREADS") == "2",
                "OMP override passed to launcher");
    });
    suite.test("nested refs, cycles, binary attachments, uppercase hashes and x extensions", [] {
        Fixture f;
        auto value = f.case_json();
        value["x-ignored"] = Json::parse(R"({"ref":"/unsafe","path":"../escape","sha256":"bad"})");
        f.case_json(value);
        // Materials has no schema content check here; references still recurse through arrays.
        write(f.source / "fcbga_demo.materials.json",
              R"({"assets":[{"ref":"nested/one.json"},{"ref":"nested/one.json"}]})");
        write(f.source / "nested/one.json", R"({"ref":"two.json"})");
        write(f.source / "nested/two.json", R"({"ref":"one.json"})");
        write(f.source / "binary.bin", "abc");
        write(f.source / "opaque.bin", std::string("\0\xff{broken JSON", 14));
        write(
            f.source / "nested/blob.json",
            R"({"path":"raw.bin","sha256":"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"})");
        write(f.source / "nested/raw.bin", "abc");
        auto request = f.request();
        request.add_inputs()->set_logical_name("nested/blob.json");
        request.add_inputs()->set_logical_name("opaque.bin");
        auto* binary = request.add_inputs();
        binary->set_logical_name("binary.bin");
        binary->set_sha256("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
        auto manager = f.manager();
        const auto job = manager->Submit(request, f.case_path, f.profile_path);
        require(job.state() == jobs::JOB_STATE_RUNNING, "nested snapshot Submit must be RUNNING");
        const fs::path run(job.result_dir());
        const auto hashes = Json::parse(read(run / "snapshot-hashes.json"));
        require(hashes.size() == 10, "visited destinations copied and hashed exactly once");
        for (const auto name : {"nested/one.json",
                                "nested/two.json",
                                "nested/blob.json",
                                "nested/raw.bin",
                                "binary.bin",
                                "opaque.bin"}) {
            const auto target = run / "inputs" / name;
            require(read(target) == read(f.source / name), "recursive snapshot raw bytes");
            require(hashes.at(target.string()).as<std::string>() ==
                        chipcae::jobmgr::sha256_hex(read(f.source / name)),
                    "recursive hash");
        }
        require(hashes.at((run / "inputs/nested/raw.bin").string()).as<std::string>() ==
                    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                "uppercase expected hash accepts lowercase SHA");
        persisted(*manager, job);
    });
    suite.test("STARTING and submitted metadata are durable before start", [] {
        Fixture f;
        bool observed = false;
        f.fake->on_start = [&](const std::string& id, const auto&) {
            const auto disk = parse_pb<jobs::Job>(f.config.run_root / id / "job.pb");
            require(disk.state() == jobs::JOB_STATE_STARTING, "start saw non-STARTING disk state");
            require(disk.started_at().seconds() > 0, "STARTING timestamp persisted");
            require(fs::is_regular_file(f.config.run_root / id / "submit.pb"),
                    "submit bytes must exist before launch");
            observed = true;
        };
        auto manager = f.manager();
        const auto job = f.running(*manager);
        require(observed, "launcher never called");
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
