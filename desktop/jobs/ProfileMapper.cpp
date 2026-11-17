#include "ProfileMapper.hpp"

#include <QJsonArray>
#include <cmath>
#include <stdexcept>
#include <string>
namespace chipcae::desktop {
jobs::RunProfile ProfileMapper::map(const QJsonObject& p, bool cudaAvailable) {
    auto integer = [&](const char* key, const int* fallback) {
        if (!p.contains(key)) {
            if (!fallback)
                throw std::runtime_error(std::string("Missing profile field ") + key);
            return *fallback;
        }
        const auto raw = p.value(key);
        if (!raw.isDouble())
            throw std::runtime_error(std::string("Invalid profile integer ") + key);
        const double value = raw.toDouble();
        if (!std::isfinite(value) || value < 0 || value > 2147483647 || std::floor(value) != value)
            throw std::runtime_error(std::string("Invalid profile integer ") + key);
        return static_cast<int>(value);
    };
    const int defaultThreads = 1;
    int ranks = integer("ranks", nullptr);
    int threads = integer("threads_per_rank", &defaultThreads);
    int gpus = integer("gpus", nullptr);
    auto backend = p.value("backend").toString();
    auto launcher = p.value("launcher").toString("mpirun");
    if (p.value("schema") != "chipcae.run_profile/1.0" || ranks < 1 || threads < 1 ||
        (launcher != "direct" && launcher != "mpirun") ||
        (launcher == "direct" && (ranks != 1 || !p.value("mpi_args").toArray().isEmpty())) ||
        (backend != "cpu" && backend != "cuda") || (backend == "cpu" && gpus != 0) ||
        (backend == "cuda" && (!cudaAvailable || gpus < 1)))
        throw std::runtime_error("Unsupported run profile/backend capability");
    if (p.contains("mpi_args") && !p.value("mpi_args").isArray())
        throw std::runtime_error("mpi_args must be an array");
    for (auto arg : p.value("mpi_args").toArray())
        if (!arg.isString())
            throw std::runtime_error("mpi_args must contain strings");
    jobs::RunProfile result;
    result.set_scheduler(jobs::SCHEDULER_TYPE_LOCAL);
    result.set_nodes(1);
    result.set_ranks(ranks);
    result.set_ranks_per_node(ranks);
    result.set_cpus_per_rank(threads);
    result.set_gpus_per_node(gpus);
    (*result.mutable_env())["OMP_NUM_THREADS"] = std::to_string(threads);
    return result;
}
LaunchCommand ProfileMapper::command(const QJsonObject& p,
                                     const LauncherConfig& c,
                                     const QString& casePath,
                                     const QString& profilePath,
                                     const QString& runDirectory) {
    auto mapped = map(p, c.cudaAvailable);
    LaunchCommand result{c.solver, {}};
    if (p.value("launcher").toString("mpirun") == "mpirun") {
        result.program = c.mpi;
        result.arguments = {"-n", QString::number(mapped.ranks())};
        for (auto arg : p.value("mpi_args").toArray())
            result.arguments.append(arg.toString());
        result.arguments.append(c.solver);
    }
    result.arguments.append({"--case", casePath, "--profile", profilePath, "--out", runDirectory});
    return result;
}
} // namespace chipcae::desktop
