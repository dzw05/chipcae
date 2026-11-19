#include "test_support.hpp"

#if CHIPCAE_JOBMGR_LOCAL
#include "manager_support.hpp"

using namespace jobmgr_test;

int main() {
    Suite suite;
    using Edit = std::function<void(Json&)>;
    const auto reject = [&](std::string name, const Edit& edit, std::string message) {
        suite.test(name, [&] {
            Fixture f;
            auto profile = f.profile();
            edit(profile);
            f.profile(profile);
            auto manager = f.manager();
            const auto job = f.submit(*manager);
            failure(job, jobs::FAILURE_CATEGORY_CASE_INVALID, message);
            require(f.fake->starts.empty(), "invalid profile launched a process");
            persisted(*manager, job);
        });
    };
    for (const auto key : {"ranks", "gpus"})
        reject(
            std::string("required integer ") + key,
            [=](Json& p) { p.erase(key); },
            std::string("Missing profile field ") + key);
    for (const auto key : {"ranks", "threads_per_rank", "gpus"}) {
        const std::vector<std::pair<std::string, Json>> invalid{{"negative", Json(-1)},
                                                                {"overflow", Json(2147483648LL)},
                                                                {"fractional", Json(1.5)},
                                                                {"string", Json("2")},
                                                                {"boolean", Json(true)},
                                                                {"null", Json::null()}};
        for (const auto& [kind, value] : invalid)
            reject(
                std::string(key) + " rejects " + kind,
                [=](Json& p) { p[key] = value; },
                std::string("Invalid profile integer ") + key);
    }
    const std::vector<std::pair<std::string, Edit>> unsupported{
        {"wrong schema", [](Json& p) { p["schema"] = "chipcae.run_profile/9.0"; }},
        {"missing schema", [](Json& p) { p.erase("schema"); }},
        {"zero ranks", [](Json& p) { p["ranks"] = 0; }},
        {"zero threads", [](Json& p) { p["threads_per_rank"] = 0; }},
        {"unknown launcher", [](Json& p) { p["launcher"] = "other"; }},
        {"direct multiple ranks",
         [](Json& p) {
             p["launcher"] = "direct";
             p["mpi_args"] = Json::array();
         }},
        {"direct mpi arguments",
         [](Json& p) {
             p["launcher"] = "direct";
             p["ranks"] = 1;
         }},
        {"unknown backend", [](Json& p) { p["backend"] = "other"; }},
        {"missing backend", [](Json& p) { p.erase("backend"); }},
        {"cpu with gpu", [](Json& p) { p["gpus"] = 1; }},
        {"cuda without capability", [](Json& p) {
             p["backend"] = "cuda";
             p["gpus"] = 1;
         }}};
    for (const auto& [name, edit] : unsupported)
        reject(name, edit, "Unsupported run profile/backend capability");
    reject(
        "mpi_args not array",
        [](Json& p) { p["mpi_args"] = "--bind-to"; },
        "mpi_args must be an array");
    reject(
        "mpi_args non-string element",
        [](Json& p) { p["mpi_args"] = Json::parse(R"(["ok",7])"); },
        "mpi_args must contain strings");
    suite.test("cuda still requires at least one GPU when capability exists", [] {
        Fixture f;
        f.config.cuda_available = true;
        auto p = f.profile();
        p["backend"] = "cuda";
        p["gpus"] = 0;
        f.profile(p);
        auto manager = f.manager();
        const auto job = f.submit(*manager);
        failure(
            job, jobs::FAILURE_CATEGORY_CASE_INVALID, "Unsupported run profile/backend capability");
        persisted(*manager, job);
    });
    suite.test("integer validation precedes capability and mpi_args checks", [] {
        Fixture f;
        auto p = f.profile();
        p.erase("ranks");
        p["schema"] = "bad";
        p["mpi_args"] = 7;
        f.profile(p);
        auto manager = f.manager();
        failure(
            f.submit(*manager), jobs::FAILURE_CATEGORY_CASE_INVALID, "Missing profile field ranks");
    });
    suite.test("direct capability validation precedes malformed mpi_args", [] {
        Fixture f;
        auto p = f.profile();
        p["launcher"] = "direct";
        p["mpi_args"] = 7;
        f.profile(p);
        auto manager = f.manager();
        failure(f.submit(*manager),
                jobs::FAILURE_CATEGORY_CASE_INVALID,
                "Unsupported run profile/backend capability");
    });
    suite.test("MPI full argv order, mapped fields and OMP override", [] {
        Fixture f;
        auto p = f.profile();
        p["mpi_args"] =
            Json::parse(R"(["--bind-to","core","--tag-output","argument with spaces"])");
        f.profile(p);
        auto manager = f.manager();
        const auto job = f.running(*manager);
        require(job.profile().SerializeAsString() == mapped_profile().SerializeAsString(),
                "MPI mapped protobuf fields");
        require(f.fake->starts.size() == 1, "one MPI start");
        const auto& cmd = f.fake->starts.front().second;
        const fs::path run(job.result_dir());
        require(cmd.program == "mpi-test", "configured MPI program");
        require(cmd.arguments == std::vector<std::string>{"-n",
                                                          "4",
                                                          "--bind-to",
                                                          "core",
                                                          "--tag-output",
                                                          "argument with spaces",
                                                          "solver-test",
                                                          "--case",
                                                          (run / "inputs/case.json").string(),
                                                          "--profile",
                                                          (run / "run.json").string(),
                                                          "--out",
                                                          run.string()},
                "complete MPI argv");
        require(cmd.env_overrides == std::map<std::string, std::string>{{"OMP_NUM_THREADS", "2"}},
                "OMP override must be supplied to spawner");
    });
    suite.test("direct full argv and CPU mapping", [] {
        Fixture f;
        auto p = f.profile();
        p["launcher"] = "direct";
        p["ranks"] = 1;
        p["threads_per_rank"] = 3;
        p.erase("mpi_args");
        f.profile(p);
        auto manager = f.manager();
        const auto job = f.running(*manager);
        require(job.profile().SerializeAsString() == mapped_profile(1, 3).SerializeAsString(),
                "direct mapped fields");
        const auto& cmd = f.fake->starts.at(0).second;
        const fs::path run(job.result_dir());
        require(cmd.program == "solver-test", "configured direct solver");
        require(cmd.arguments == std::vector<std::string>{"--case",
                                                          (run / "inputs/case.json").string(),
                                                          "--profile",
                                                          (run / "run.json").string(),
                                                          "--out",
                                                          run.string()},
                "complete direct argv");
        require(cmd.env_overrides.at("OMP_NUM_THREADS") == "3", "direct OMP override");
    });
    suite.test("default launcher is MPI and threads defaults to one", [] {
        Fixture f;
        auto p = f.profile();
        p.erase("launcher");
        p.erase("threads_per_rank");
        p.erase("mpi_args");
        f.profile(p);
        auto manager = f.manager();
        const auto job = f.running(*manager);
        require(job.profile().SerializeAsString() == mapped_profile(4, 1).SerializeAsString(),
                "default mapped fields");
        const fs::path run(job.result_dir());
        const auto& cmd = f.fake->starts.at(0).second;
        require(cmd.program == "mpi-test" &&
                    cmd.arguments == std::vector<std::string>{"-n",
                                                              "4",
                                                              "solver-test",
                                                              "--case",
                                                              (run / "inputs/case.json").string(),
                                                              "--profile",
                                                              (run / "run.json").string(),
                                                              "--out",
                                                              run.string()},
                "default MPI argv");
        require(cmd.env_overrides.at("OMP_NUM_THREADS") == "1", "default OMP override");
    });
    suite.test("supported CUDA maps GPU count", [] {
        Fixture f;
        f.config.cuda_available = true;
        auto p = f.profile();
        p["backend"] = "cuda";
        p["gpus"] = 2;
        f.profile(p);
        auto manager = f.manager();
        const auto job = f.running(*manager);
        require(job.profile().SerializeAsString() == mapped_profile(4, 2, 2).SerializeAsString(),
                "CUDA mapped fields");
    });
    return suite.result();
}
#else
int main() {
    std::cout << "SKIP: LocalJobManager not built\n";
    return 77;
}
#endif
