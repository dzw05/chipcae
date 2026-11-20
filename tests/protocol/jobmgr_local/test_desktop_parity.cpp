#include "test_support.hpp"

#if CHIPCAE_JOBMGR_DESKTOP_PARITY
#include "ProfileMapper.hpp"
#include "manager_support.hpp"
#include "profile_map.hpp"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QLockFile>

using namespace jobmgr_test;
namespace detail = chipcae::jobmgr::detail;
namespace desktop = chipcae::desktop;

namespace {
QString utf8(const std::string& value) {
    return QString::fromUtf8(value.data(), value.size());
}
QJsonObject qt_profile(const Json& profile) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(profile.to_string())).object();
}
std::string rejection(const std::function<void()>& action) {
    try {
        action();
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    throw std::runtime_error("invalid profile was accepted");
}
void compare(const Json& p, bool cuda) {
    TempDirectory temp;
    LocalConfig config{
        temp.path / "runs with spaces", "solver with spaces", "mpi with spaces", cuda};
    desktop::LauncherConfig qt_config;
    qt_config.runRoot = utf8(config.run_root.string());
    qt_config.solver = utf8(config.solver);
    qt_config.mpi = utf8(config.mpi);
    qt_config.cudaAvailable = cuda;
    const auto qp = qt_profile(p);
    const auto mapped = detail::map_profile(p, cuda);
    const auto qt_mapped = desktop::ProfileMapper::map(qp, cuda);
    require(mapped.SerializeAsString() == qt_mapped.SerializeAsString(),
            "RunProfile bytes differ from desktop");
    const auto case_path = temp.path / "输入 case with spaces.json";
    const auto profile_path = temp.path / "run profile.json";
    const auto command =
        detail::profile_command(p, config, case_path, profile_path, config.run_root);
    const auto qt_command = desktop::ProfileMapper::command(qp,
                                                            qt_config,
                                                            utf8(case_path.string()),
                                                            utf8(profile_path.string()),
                                                            utf8(config.run_root.string()));
    require(command.program == qt_command.program.toUtf8().toStdString(),
            "program differs from desktop");
    require(command.arguments.size() == static_cast<std::size_t>(qt_command.arguments.size()),
            "argument count differs from desktop");
    for (std::size_t i = 0; i < command.arguments.size(); ++i)
        require(command.arguments[i] ==
                    qt_command.arguments.at(static_cast<qsizetype>(i)).toUtf8().toStdString(),
                "UTF-8 argv differs at index " + std::to_string(i));
    require(command.env_overrides.size() == 1 && command.env_overrides.at("OMP_NUM_THREADS") ==
                                                     std::to_string(mapped.cpus_per_rank()),
            "environment must contain exactly the mapped OMP override");
    require(qt_mapped.env().size() == 1 && qt_mapped.env().at("OMP_NUM_THREADS") ==
                                               command.env_overrides.at("OMP_NUM_THREADS"),
            "environment differs from desktop mapping");
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    Suite suite;
    const auto base = Json::parse(R"({"schema":"chipcae.run_profile/1.0","ranks":1,
        "backend":"cpu","gpus":0,"launcher":"direct"})");
    for (const bool mpi : {false, true})
        for (const bool explicit_threads : {false, true})
            for (const bool cuda : {false, true}) {
                const auto name = std::string(mpi ? "MPI rank 4" : "direct rank 1") +
                                  (explicit_threads ? " threads 3" : " default threads") +
                                  (cuda ? " CUDA" : " CPU");
                suite.test(name, [&] {
                    auto p = base;
                    if (mpi) {
                        p["launcher"] = "mpirun";
                        p["ranks"] = 4;
                        p["mpi_args"] = Json::parse(R"(["--bind-to","core"])");
                    }
                    if (explicit_threads)
                        p["threads_per_rank"] = 3;
                    if (cuda) {
                        p["backend"] = "cuda";
                        p["gpus"] = 1;
                    }
                    compare(p, cuda);
                });
            }
    const auto invalid = [&](std::string name, const std::function<void(Json&)>& change) {
        suite.test(name, [&] {
            auto p = base;
            change(p);
            const auto ours = rejection([&] { detail::map_profile(p, false); });
            const auto theirs =
                rejection([&] { desktop::ProfileMapper::map(qt_profile(p), false); });
            require(ours == theirs, "profile rejection text differs: " + ours + " / " + theirs);
        });
    };
    invalid("reject CPU with GPU", [](Json& p) { p["gpus"] = 1; });
    invalid("reject unavailable CUDA", [](Json& p) {
        p["backend"] = "cuda";
        p["gpus"] = 1;
    });
    invalid("reject direct rank 2", [](Json& p) { p["ranks"] = 2; });
    invalid("reject direct MPI arguments",
            [](Json& p) { p["mpi_args"] = Json::parse(R"(["--bind-to","core"])"); });
    invalid("reject rank zero", [](Json& p) { p["ranks"] = 0; });
    invalid("reject wrong schema", [](Json& p) { p["schema"] = "wrong"; });
    suite.test("manager lock excludes QLockFile and release allows it", [] {
        Fixture f;
        QLockFile qt_lock(utf8((f.config.run_root / "launcher.lock").string()));
        {
            LocalJobManager manager(f.config);
            require(!qt_lock.tryLock(0), "QLockFile acquired manager-owned root");
        }
        require(qt_lock.tryLock(0), "manager did not release launcher.lock");
        qt_lock.unlock();
        LocalJobManager manager(f.config);
    });
    suite.test("QLockFile excludes manager and release allows it", [] {
        Fixture f;
        fs::create_directories(f.config.run_root);
        QLockFile qt_lock(utf8((f.config.run_root / "launcher.lock").string()));
        require(qt_lock.tryLock(0), "cannot acquire QLockFile fixture");
        throws_message([&] { LocalJobManager manager(f.config); },
                       "Local job root already has a control writer");
        qt_lock.unlock();
        {
            LocalJobManager manager(f.config);
            require(!qt_lock.tryLock(0), "manager did not acquire released root");
        }
        require(qt_lock.tryLock(0), "second manager did not release root");
    });
    return suite.result();
}
#else
int main() {
    std::cout << "SKIP: desktop Qt sources unavailable\n";
    return 77;
}
#endif
