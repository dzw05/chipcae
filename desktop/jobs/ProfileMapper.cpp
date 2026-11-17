#include "ProfileMapper.hpp"

#include <QJsonArray>
#include <cmath>
#include <stdexcept>
namespace chipcae::desktop {
jobs::RunProfile ProfileMapper::map(const QJsonObject& p, bool cudaAvailable) {
    auto integer = [&](const char* key, int fallback) {
        double value = p.value(key).toDouble(fallback);
        if (!std::isfinite(value) || value < 0 || value > 2147483647 || std::floor(value) != value)
            throw std::runtime_error("Invalid profile integer");
        return static_cast<int>(value);
    };
    int ranks = integer("ranks", 0), threads = integer("threads_per_rank", 1);
    int gpus = integer("gpus", 0);
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
