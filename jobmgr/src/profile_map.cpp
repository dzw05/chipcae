#include "profile_map.hpp"

#include <cmath>
#include <stdexcept>

namespace chipcae::jobmgr::detail {
namespace {
// Match QJsonValue::toString/toArray defaults, including for a wrong JSON type.
std::string
string_or(const jsoncons::json& object, const char* key, const std::string& fallback = {}) {
    return object.contains(key) && object.at(key).is_string() ? object.at(key).as<std::string>()
                                                              : fallback;
}
} // namespace
jobs::RunProfile map_profile(const jsoncons::json& p, bool cuda_available) {
    const auto integer = [&](const char* key, const int* fallback) {
        if (!p.contains(key)) {
            if (!fallback)
                throw std::runtime_error(std::string("Missing profile field ") + key);
            return *fallback;
        }
        const auto& raw = p.at(key);
        if (!raw.is_number())
            throw std::runtime_error(std::string("Invalid profile integer ") + key);
        const double value = raw.as<double>();
        if (!std::isfinite(value) || value < 0 || value > 2147483647 || std::floor(value) != value)
            throw std::runtime_error(std::string("Invalid profile integer ") + key);
        return static_cast<int>(value);
    };
    const int default_threads = 1;
    const int ranks = integer("ranks", nullptr);
    const int threads = integer("threads_per_rank", &default_threads);
    const int gpus = integer("gpus", nullptr);
    const auto backend = string_or(p, "backend");
    const auto launcher = string_or(p, "launcher", "mpirun");
    const bool has_args =
        p.contains("mpi_args") && p.at("mpi_args").is_array() && !p.at("mpi_args").empty();
    if (string_or(p, "schema") != "chipcae.run_profile/1.0" || ranks < 1 || threads < 1 ||
        (launcher != "direct" && launcher != "mpirun") ||
        (launcher == "direct" && (ranks != 1 || has_args)) ||
        (backend != "cpu" && backend != "cuda") || (backend == "cpu" && gpus != 0) ||
        (backend == "cuda" && (!cuda_available || gpus < 1)))
        throw std::runtime_error("Unsupported run profile/backend capability");
    if (p.contains("mpi_args") && !p.at("mpi_args").is_array())
        throw std::runtime_error("mpi_args must be an array");
    if (p.contains("mpi_args"))
        for (const auto& arg : p.at("mpi_args").array_range())
            if (!arg.is_string())
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
LaunchCommand profile_command(const jsoncons::json& p,
                              const LocalConfig& c,
                              const std::filesystem::path& case_path,
                              const std::filesystem::path& profile_path,
                              const std::filesystem::path& run_dir) {
    const auto mapped = map_profile(p, c.cuda_available);
    LaunchCommand result;
    result.program = c.solver;
    if (string_or(p, "launcher", "mpirun") == "mpirun") {
        result.program = c.mpi;
        result.arguments = {"-n", std::to_string(mapped.ranks())};
        if (p.contains("mpi_args"))
            for (const auto& arg : p.at("mpi_args").array_range())
                result.arguments.push_back(arg.as<std::string>());
        result.arguments.push_back(c.solver);
    }
    result.arguments.insert(result.arguments.end(),
                            {"--case",
                             case_path.string(),
                             "--profile",
                             profile_path.string(),
                             "--out",
                             run_dir.string()});
    result.env_overrides["OMP_NUM_THREADS"] = std::to_string(mapped.cpus_per_rank());
    return result;
}
} // namespace chipcae::jobmgr::detail
