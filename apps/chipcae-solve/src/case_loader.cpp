#include "chipcae/solve/case_loader.hpp"

#include "sha256.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>
#include <regex>
#include <set>
#include <sstream>
#include <yaml-cpp/yaml.h>
namespace chipcae::solve {
namespace {
using Json = jsoncons::json;
[[noreturn]] void invalid(const std::string& message) {
    throw InputError(ErrorCode::INPUT_INVALID, message);
}
std::string bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw InputError(ErrorCode::INPUT_REFERENCE_MISSING,
                         "cannot read reference: " + path.string());
    std::ostringstream out;
    out << in.rdbuf();
    if (in.bad())
        throw InputError(ErrorCode::INPUT_REFERENCE_MISSING,
                         "cannot read reference: " + path.string());
    return out.str();
}
Json yaml_json(const YAML::Node& node, int depth = 0) {
    if (depth > 64)
        invalid("YAML nesting exceeds 64 levels");
    if (node.IsNull())
        return Json::null();
    if (node.IsSequence()) {
        Json a = Json::array();
        for (const auto& value : node)
            a.push_back(yaml_json(value, depth + 1));
        return a;
    }
    if (node.IsMap()) {
        Json obj;
        for (const auto& value : node) {
            auto key = value.first.as<std::string>();
            if (obj.contains(key))
                invalid("duplicate YAML key: " + key);
            obj[key] = yaml_json(value.second, depth + 1);
        }
        return obj;
    }
    auto value = node.Scalar();
    if (node.Tag() == "!" || node.Tag() == "tag:yaml.org,2002:str")
        return Json(value);
    bool boolean;
    std::int64_t signed_value;
    std::uint64_t unsigned_value;
    double real_value;
    if (YAML::convert<bool>::decode(node, boolean))
        return Json(boolean);
    if (YAML::convert<std::int64_t>::decode(node, signed_value))
        return Json(signed_value);
    if (YAML::convert<std::uint64_t>::decode(node, unsigned_value))
        return Json(unsigned_value);
    if (YAML::convert<double>::decode(node, real_value))
        return Json(real_value);
    return Json(value);
}
Json read(const std::filesystem::path& path) {
    auto content = bytes(path);
    if (path.extension() == ".yaml" || path.extension() == ".yml")
        return yaml_json(YAML::Load(content));
    return Json::parse(content);
}
std::filesystem::path reference(const Json& ref, const std::filesystem::path& base) {
    if (!ref.contains("ref") || !ref.at("ref").is_string() ||
        ref.at("ref").as<std::string>().empty())
        throw InputError(ErrorCode::INPUT_REFERENCE_MISSING, "missing ref");
    std::filesystem::path relative = ref.at("ref").as<std::string>();
    if (relative.is_absolute())
        invalid("reference must be relative");
    auto path = (base / relative).lexically_normal();
    auto content = bytes(path);
    if (ref.contains("sha256")) {
        auto hash = ref.at("sha256").as<std::string>();
        if (!std::regex_match(hash, std::regex("[a-fA-F0-9]{64}")))
            invalid("invalid sha256");
        std::transform(hash.begin(), hash.end(), hash.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (hash != mesh::sha256(content))
            throw InputError(ErrorCode::INPUT_HASH_MISMATCH,
                             "Referenced file hash mismatch: " + path.string());
    }
    return path;
}
double finite(const Json& obj, const char* key, bool positive = false) {
    if (!obj.at(key).is_number())
        invalid(std::string("number required: ") + key);
    auto value = obj.at(key).as<double>();
    if (!std::isfinite(value) || (positive && value <= 0))
        invalid(std::string("invalid value: ") + key);
    return value;
}
int integer(const Json& obj, const char* key, int minimum) {
    const auto& value = obj.at(key);
    if (!value.is_integer<int>())
        invalid(std::string("integer required: ") + key);
    auto n = value.as<int>();
    if (n < minimum)
        invalid(std::string("invalid value: ") + key);
    return n;
}
} // namespace
InterpolatedProperty
interpolate_property(const PropertyTable& table, double temperature, const std::string& context) {
    const auto& t = table.temperatures;
    const auto& v = table.values;
    auto fail = [&](const std::string& message) { invalid(context + ": " + message); };
    if (t.empty() || t.size() != v.size())
        fail("temperature/value table sizes must match and be nonempty");
    if (!std::isfinite(temperature))
        fail("nonfinite requested temperature");
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (!std::isfinite(t[i]) || !std::isfinite(v[i]))
            fail("nonfinite temperature or property value");
        if (i && t[i] <= t[i - 1])
            fail("temperatures must be strictly increasing");
    }
    if (t.size() == 1)
        return {v.front(), false};
    if (temperature < t.front())
        return {v.front(), true};
    if (temperature > t.back())
        return {v.back(), true};
    auto upper = std::lower_bound(t.begin(), t.end(), temperature);
    auto i = static_cast<std::size_t>(upper - t.begin());
    if (*upper == temperature)
        return {v[i], false};
    return {v[i - 1] + (temperature - t[i - 1]) * (v[i] - v[i - 1]) / (t[i] - t[i - 1]), false};
}
RunInputs load_inputs(const std::filesystem::path& case_path,
                      const std::filesystem::path& profile_path,
                      int ranks) {
    try {
        RunInputs out;
        out.case_path = std::filesystem::absolute(case_path);
        out.profile_path = std::filesystem::absolute(profile_path);
        auto c = read(out.case_path);
        auto profile = read(out.profile_path);
        if (c.at("schema").as<std::string>() != "chipcae.case/1.0")
            invalid("unsupported case schema");
        if (profile.at("schema").as<std::string>() != "chipcae.run_profile/1.0" ||
            integer(profile, "ranks", 1) != ranks)
            invalid("invalid profile: schema or ranks do not match MPI size");
        if (!profile.at("backend").is_string() ||
            (profile.at("backend").as<std::string>() != "cpu" &&
             profile.at("backend").as<std::string>() != "cuda"))
            invalid("invalid profile backend");
        int gpus = integer(profile, "gpus", 0);
        if (profile.contains("threads_per_rank"))
            (void)integer(profile, "threads_per_rank", 1);
        if (profile.contains("launcher")) {
            auto launcher = profile.at("launcher").as<std::string>();
            if (launcher != "direct" && launcher != "mpirun")
                invalid("invalid profile launcher");
        }
        if (profile.contains("mpi_args")) {
            if (!profile.at("mpi_args").is_array())
                invalid("mpi_args must be a string array");
            for (const auto& arg : profile.at("mpi_args").array_range())
                if (!arg.is_string())
                    invalid("mpi_args must be a string array");
        }
        if (profile.contains("output_interval")) {
            const auto& interval = profile.at("output_interval");
            if (!interval.is_object() || interval.contains("steps") == interval.contains("time"))
                invalid("output_interval requires steps or time");
            for (const auto& item : interval.object_range())
                if (item.key() != "steps" && item.key() != "time" && !item.key().starts_with("x-"))
                    invalid("invalid output_interval field");
            if (interval.contains("steps"))
                (void)integer(interval, "steps", 1);
            else
                (void)finite(interval, "time", true);
        }
        for (const auto& item : profile.object_range()) {
            const std::set<std::string> known{"schema",
                                              "ranks",
                                              "backend",
                                              "gpus",
                                              "threads_per_rank",
                                              "launcher",
                                              "mpi_args",
                                              "output_interval"};
            if (!known.contains(std::string(item.key())) && !item.key().starts_with("x-"))
                invalid("unknown profile field");
        }
        if (profile.at("backend").as<std::string>() != "cpu" || gpus != 0)
            throw InputError(ErrorCode::NOT_IMPLEMENTED, "O2 supports CPU profiles only");
        out.stack_path = reference(c.at("stack"), out.case_path.parent_path());
        out.material_path = reference(c.at("materials"), out.case_path.parent_path());
        auto stack_json = read(out.stack_path);
        if (stack_json.contains("external_refs"))
            for (const auto& ref : stack_json.at("external_refs").array_range()) {
                Json file_ref = ref;
                file_ref["ref"] = ref.at("path");
                (void)reference(file_ref, out.stack_path.parent_path());
            }
        auto stack = mesh::load_stack(out.stack_path, CHIPCAE_STACK_SCHEMA);
        if (!stack)
            invalid(stack.error().path + ": " + stack.error().reason);
        out.stack = std::move(stack.value());
        for (const auto& layer : out.stack.layers)
            if (std::any_of(layer.regions.begin(),
                            layer.regions.end(),
                            [](const auto& region) { return !region.footprint.polygon.empty(); }) ||
                (layer.copper && layer.copper->polygons))
                throw InputError(ErrorCode::CUT_UNSUPPORTED, "cut not supported in O2");
        if (!out.stack.outline.polygon.empty())
            throw InputError(ErrorCode::CUT_UNSUPPORTED, "cut not supported in O2");
        const auto& m = c.at("mesh");
        if (m.at("type").as<std::string>() != "aligned_cartesian_cut")
            invalid("invalid mesh type");
        out.grid.ranks = ranks;
        if (m.contains("nx"))
            out.grid.nx = integer(m, "nx", 1);
        if (m.contains("ny"))
            out.grid.ny = integer(m, "ny", 1);
        if (m.contains("max_ratio")) {
            out.grid.max_ratio = finite(m, "max_ratio", true);
            if (out.grid.max_ratio <= 1)
                invalid("max_ratio must exceed 1");
        }
        if (m.contains("cell_budget"))
            out.grid.cell_budget = integer(m, "cell_budget", 1);
        if (m.contains("levels"))
            out.grid.levels = integer(m, "levels", 0);
        auto minimum = m.contains("min_elems_per_layer") ? integer(m, "min_elems_per_layer", 1) : 2;
        for (auto& layer : out.stack.layers)
            layer.n_elem_min = std::max(layer.n_elem_min, minimum);
        std::set<std::string> ids;
        auto check_id = [&](const Json& obj) {
            if (!obj.contains("id"))
                return std::string{};
            auto id = obj.at("id").as<std::string>();
            if (!std::regex_match(id, std::regex("[A-Za-z0-9_.-]{1,64}")) || !ids.insert(id).second)
                invalid("invalid or duplicate id: " + id);
            return id;
        };
        auto check_target = [&](const std::string& target) {
            if (target == "boundary:embedded")
                throw InputError(ErrorCode::CUT_UNSUPPORTED, "cut not supported in O2");
            if (std::regex_match(target, std::regex("face:(xmin|xmax|ymin|ymax|zmin|zmax)")))
                return;
            if (target.starts_with("layer:"))
                for (const auto& layer : out.stack.layers)
                    if (layer.name == target.substr(6))
                        return;
            invalid("invalid target: " + target);
        };
        bool mech = false, temp_from = false;
        for (const auto& physics : c.at("physics").array_range()) {
            auto id = physics.at("id").as<std::string>();
            if (id == "thermal" && !out.thermal &&
                physics.at("solver").as<std::string>() == "fem.conduction" &&
                physics.at("mode").as<std::string>() == "steady")
                out.thermal = true;
            else if (id == "mech" && !mech &&
                     physics.at("solver").as<std::string>() == "fem.solid") {
                mech = true;
                if (physics.contains("temp_from")) {
                    if (physics.at("temp_from").as<std::string>() != "thermal")
                        invalid("invalid temp_from");
                    temp_from = true;
                    out.T_ref = finite(physics, "T_ref");
                }
            } else
                invalid("invalid or duplicate physics");
        }
        int uniform = 0, thermal_loads = 0, anchors = 0;
        for (const auto& obj : c.at("loads").array_range()) {
            Load load;
            load.id = check_id(obj);
            load.type = obj.at("type").as<std::string>();
            if (load.type == "uniform_temperature") {
                ++uniform;
                load.T_ref = finite(obj, "T_ref");
                load.T = finite(obj, "T_target");
                out.T_ref = load.T_ref;
            } else {
                ++thermal_loads;
                load.target = obj.at("target").as<std::string>();
                check_target(load.target);
                if (load.type == "fixed_temperature") {
                    load.T = finite(obj, "T");
                    ++anchors;
                } else if (load.type == "convection") {
                    load.h = finite(obj, "h", true);
                    load.T_inf = finite(obj, "T_inf");
                    ++anchors;
                } else if (load.type == "heat_source") {
                    if (!load.target.starts_with("layer:") ||
                        obj.contains("power") == obj.contains("q_vol"))
                        invalid("invalid heat source");
                    load.total_power = obj.contains("power");
                    load.source = finite(obj, load.total_power ? "power" : "q_vol");
                } else
                    invalid("invalid load type");
            }
            out.loads.push_back(load);
        }
        if (!mech || (out.thermal ? (uniform != 0 || !temp_from || anchors == 0)
                                  : (uniform != 1 || temp_from || thermal_loads != 0)))
            invalid("inconsistent temperature sources");
        const auto& constraints = c.at("constraints");
        if (constraints.contains("rigid") == constraints.contains("fixed"))
            invalid("choose rigid or fixed constraints");
        if (constraints.contains("rigid")) {
            if (constraints.at("rigid").as<std::string>() != "3-2-1")
                invalid("invalid rigid constraint");
            (void)check_id(constraints);
        } else {
            for (const auto& fixed : constraints.at("fixed").array_range()) {
                (void)check_id(fixed);
                auto target = fixed.at("target").as<std::string>();
                check_target(target);
                out.fixed.push_back(target);
            }
            if (out.fixed.empty())
                invalid("empty fixed constraints");
        }
        if (c.contains("process"))
            invalid("process is not supported in case/1.0");
        if (c.contains("solver") && c.at("solver").contains("linear")) {
            const auto& linear = c.at("solver").at("linear");
            if (linear.contains("rtol"))
                out.linear.rtol = finite(linear, "rtol", true);
            if (linear.contains("max_iter"))
                out.linear.max_iter = integer(linear, "max_iter", 1);
            if (linear.contains("preconditioner"))
                out.linear.preconditioner = linear.at("preconditioner").as<std::string>();
            if (out.linear.preconditioner != "direct" && out.linear.preconditioner != "column_gmg")
                invalid("invalid preconditioner");
        }
        const auto& output = c.at("output");
        if (output.at("format").as<std::string>() != "vtkhdf")
            invalid("invalid output format");
        out.fields = {"T", "U", "layer_id", "material_id", "cell_kind", "elem_gid"};
        if (output.contains("fields")) {
            std::set<std::string> selected;
            for (const auto& field : output.at("fields").array_range()) {
                auto name = field.as<std::string>();
                if (std::find(out.fields.begin(), out.fields.end(), name) == out.fields.end() ||
                    !selected.insert(name).second)
                    invalid("invalid output field");
            }
            out.fields.assign(selected.begin(), selected.end());
        }
        auto library = read(out.material_path);
        if (library.at("schema").as<std::string>() != "chipcae.material_library/1" ||
            !library.at("materials").is_array() || library.at("materials").empty())
            invalid("invalid material library");
        for (const auto& item : library.object_range())
            if (item.key() != "schema" && item.key() != "materials" &&
                !item.key().starts_with("x-"))
                invalid("unknown material library field");
        auto material_schema = read(CHIPCAE_MATERIAL_SCHEMA);
        // material.v1's optional frame reference is unresolved in the baseline schema.
        // O2 uses isotropic materials, so reject a frame instead of ignoring it.
        material_schema.at("properties").erase("material_frame");
        auto validator = jsoncons::jsonschema::make_json_schema(material_schema);
        for (const auto& card : library.at("materials").array_range()) {
            validator.validate(card, [&](const auto& error) {
                invalid("material " +
                        (card.contains("id") && card.at("id").is_string()
                             ? card.at("id").as<std::string>()
                             : "<unknown>") +
                        " " + error.instance_location().string() + ": " + error.message());
                return jsoncons::jsonschema::walk_result::abort;
            });
            if (card.contains("material_frame") ||
                card.at("model").as<std::string>() != "thermo_elastic" ||
                card.at("elastic").at("symmetry").as<std::string>() != "isotropic" ||
                card.at("thermal_expansion").at("symmetry").as<std::string>() != "isotropic")
                invalid("O2 requires isotropic thermo_elastic materials");
            Material material;
            material.id = card.at("id").as<std::string>();
            auto column = [&](const Json& table, const char* name) {
                const auto& temperatures = table.at("temperatures");
                const auto& values = table.at("columns").at(name);
                PropertyTable result;
                for (const auto& t : temperatures.array_range())
                    result.temperatures.push_back(t.as<double>());
                for (const auto& v : values.array_range())
                    result.values.push_back(v.as<double>());
                (void)interpolate_property(
                    result, out.T_ref, "material " + material.id + "/" + name);
                return result;
            };
            material.E_table = column(card.at("elastic").at("table"), "E");
            material.nu_table = column(card.at("elastic").at("table"), "nu");
            material.alpha_table = column(card.at("thermal_expansion").at("table"), "alpha");
            for (double E : material.E_table.values)
                if (!(E > 0))
                    invalid("material " + material.id + "/E: must be positive");
            for (double nu : material.nu_table.values)
                if (!(nu > -1 && nu < 0.5))
                    invalid("material " + material.id + "/nu: must be between -1 and 0.5");
            if (out.thermal)
                material.k = finite(card, "conductivity", true);
            if (!out.materials.emplace(material.id, material).second)
                throw InputError(ErrorCode::MATERIAL_ID_DUPLICATE, "duplicate material id");
        }
        auto mapped = [&](const std::string& id) {
            if (!out.materials.contains(id))
                throw InputError(ErrorCode::MATERIAL_MAPPING_MISSING,
                                 "material mapping missing: " + id);
        };
        for (const auto& layer : out.stack.layers) {
            mapped(layer.material);
            for (const auto& region : layer.regions)
                mapped(region.material);
            if (layer.copper)
                mapped(layer.copper->metal);
        }
        return out;
    } catch (const InputError&) {
        throw;
    } catch (const std::exception& error) {
        throw InputError(ErrorCode::INPUT_INVALID, std::string("invalid input: ") + error.what());
    }
}
} // namespace chipcae::solve
