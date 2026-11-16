#include "chipcae/material/schema.hpp"

#include "chipcae/material/thermo_elastic.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
namespace chipcae::material {
namespace {
struct Json {
    enum Kind { Null, Number, String, Array, Object, Boolean } kind = Null;
    Real number = 0;
    std::string string;
    std::string path = "material";
    std::vector<Json> array;
    std::map<std::string, Json> object;
    const Json& at(const std::string& key) const {
        auto it = object.find(key);
        if (kind != Object || it == object.end())
            throw std::runtime_error(path + "." + key + ": required field is missing");
        return it->second;
    }
    bool has(const std::string& key) const { return object.contains(key); }
    Real num() const {
        if (kind == Number)
            return number;
        if (kind == String) {
            static const std::regex strict_number(
                R"(^[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?$)");
            if (std::regex_match(string, strict_number)) {
                char* end = nullptr;
                const Real value = std::strtod(string.c_str(), &end);
                if (end == string.c_str() + string.size() && std::isfinite(value))
                    return value;
            }
        }
        throw std::runtime_error(path + ": expected finite number");
    }
    std::string str() const {
        if (kind != String)
            throw std::runtime_error(path + ": expected string");
        return string;
    }
};
struct Reader {
    std::string_view s;
    std::size_t pos = 0;
    void ws() {
        while (pos < s.size() &&
               (s[pos] == ' ' || s[pos] == '\n' || s[pos] == '\r' || s[pos] == '\t'))
            ++pos;
    }
    bool eat(char c) {
        ws();
        if (pos < s.size() && s[pos] == c) {
            ++pos;
            return true;
        }
        return false;
    }
    void expect(char c) {
        if (!eat(c))
            throw std::runtime_error("malformed JSON at byte " + std::to_string(pos));
    }
    std::string string() {
        expect('"');
        std::string v;
        while (pos < s.size()) {
            char c = s[pos++];
            if (c == '"')
                return v;
            if (static_cast<unsigned char>(c) < 32)
                throw std::runtime_error("JSON control character");
            if (c == '\\') {
                if (pos == s.size())
                    break;
                c = s[pos++];
                switch (c) {
                case '"':
                case '\\':
                case '/':
                    break;
                case 'n':
                    c = '\n';
                    break;
                case 'r':
                    c = '\r';
                    break;
                case 't':
                    c = '\t';
                    break;
                case 'b':
                    c = '\b';
                    break;
                case 'f':
                    c = '\f';
                    break;
                default:
                    throw std::runtime_error("unsupported JSON escape");
                }
            }
            v += c;
        }
        throw std::runtime_error("unterminated JSON string");
    }
    Json value(int depth = 0) {
        if (depth > 64)
            throw std::runtime_error("JSON nesting limit");
        ws();
        if (pos == s.size())
            throw std::runtime_error("unexpected JSON end");
        Json j;
        if (s[pos] == '{') {
            ++pos;
            j.kind = Json::Object;
            if (eat('}'))
                return j;
            do {
                auto key = string();
                expect(':');
                if (!j.object.emplace(key, value(depth + 1)).second)
                    throw std::runtime_error(key + ": duplicate key");
            } while (eat(','));
            expect('}');
        } else if (s[pos] == '[') {
            ++pos;
            j.kind = Json::Array;
            if (eat(']'))
                return j;
            do {
                j.array.push_back(value(depth + 1));
            } while (eat(','));
            expect(']');
        } else if (s[pos] == '"') {
            j.kind = Json::String;
            j.string = string();
        } else if (s.substr(pos, 4) == "null") {
            pos += 4;
        } else if (s.substr(pos, 4) == "true") {
            pos += 4;
            j.kind = Json::Boolean;
        } else if (s.substr(pos, 5) == "false") {
            pos += 5;
            j.kind = Json::Boolean;
        } else {
            auto start = pos;
            if (s[pos] == '-')
                ++pos;
            if (pos == s.size())
                throw std::runtime_error("bad JSON number");
            if (s[pos] == '0')
                ++pos;
            else {
                if (s[pos] < '1' || s[pos] > '9')
                    throw std::runtime_error("bad JSON number");
                while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
                    ++pos;
            }
            if (pos < s.size() && s[pos] == '.') {
                ++pos;
                auto begin = pos;
                while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
                    ++pos;
                if (pos == begin)
                    throw std::runtime_error("bad fraction");
            }
            if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
                ++pos;
                if (pos < s.size() && (s[pos] == '+' || s[pos] == '-'))
                    ++pos;
                auto begin = pos;
                while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
                    ++pos;
                if (pos == begin)
                    throw std::runtime_error("bad exponent");
            }
            auto result = std::from_chars(s.data() + start, s.data() + pos, j.number);
            if (result.ec != std::errc{} || !std::isfinite(j.number))
                throw std::runtime_error("invalid finite JSON number");
            j.kind = Json::Number;
        }
        return j;
    }
    Json read() {
        auto j = value();
        ws();
        if (pos != s.size() || j.kind != Json::Object)
            throw std::runtime_error("expected single JSON object");
        return j;
    }
};
std::vector<Real> numbers(const Json& j) {
    if (j.kind != Json::Array)
        throw std::runtime_error("expected array");
    std::vector<Real> a;
    for (const auto& v : j.array)
        a.push_back(v.num());
    return a;
}
TemperatureTable table(const Json& j, const std::vector<std::string>& names, bool cte = false) {
    auto interpolation = j.at("interpolation").str();
    if (interpolation != "linear")
        throw std::runtime_error(
            "interpolation: v0 only supports linear (pchip NotImplemented; constant forbidden)");
    auto extrap = j.at("extrapolation").str();
    if (extrap != "error" && extrap != "clamp")
        throw std::runtime_error("extrapolation: invalid value");
    std::vector<std::vector<Real>> columns;
    for (const auto& name : names)
        columns.push_back(numbers(j.at("columns").at(name)));
    try {
        TemperatureTable t(numbers(j.at("temperatures")),
                           columns,
                           Interpolation::Linear,
                           extrap == "error" ? Extrapolation::Error : Extrapolation::Clamp);
        if (!cte && !t.strictly_increasing())
            throw std::runtime_error("temperatures: duplicate modulus knots are forbidden");
        return t;
    } catch (const std::exception& e) {
        throw std::runtime_error(j.path + ": " + e.what());
    }
}
void set_paths(Json& j, const std::string& path = "material") {
    j.path = path;
    for (auto& [k, v] : j.object)
        set_paths(v, path + "." + k);
    for (std::size_t i = 0; i < j.array.size(); ++i)
        set_paths(j.array[i], path + "[" + std::to_string(i) + "]");
}
MaterialParams decode(Json j, bool customer_delivery = false) {
    set_paths(j);
    MaterialParams p;
    p.id = j.at("id").str();
    if (j.at("schema_version").str() != "1" || j.at("unit_system").str() != "SI")
        throw std::runtime_error("schema_version/unit_system: expected 1/SI");
    const auto& provenance = j.at("provenance");
    auto source = provenance.at("source_type").str(),
         confidence = provenance.at("confidence").str();
    if (source != "datasheet" && source != "measurement" && source != "literature" &&
        source != "fitted" && source != "illustrative")
        throw std::runtime_error("provenance.source_type: invalid");
    if (confidence != "high" && confidence != "medium" && confidence != "low" &&
        confidence != "illustrative")
        throw std::runtime_error("provenance.confidence: invalid");
    if (customer_delivery && (confidence == "illustrative" || source == "illustrative"))
        throw std::runtime_error(
            "provenance.confidence: illustrative cards forbidden in customer delivery");
    p.density = j.at("density").num();
    if (!(p.density > 0))
        throw std::runtime_error("density: must be positive");
    auto model = j.at("model").str();
    if (model != "thermo_elastic" && model != "prony_viscoelastic")
        throw std::runtime_error("model: unsupported");
    p.kind = model == "thermo_elastic" ? ModelKind::ThermoElastic : ModelKind::PronyViscoelastic;
    p.model_id = model + "@1";
    const auto& th = j.at("thermal_expansion");
    auto type = th.at("cte_type").str();
    if (type != "tangent" && type != "secant")
        throw std::runtime_error("thermal_expansion.cte_type: invalid");
    p.thermal.cte_type = type == "tangent" ? CteType::Tangent : CteType::Secant;
    if (type == "secant")
        p.thermal.T_ref = th.at("reference_temperature").num();
    p.thermal.T_sf = th.at("stress_free_temperature").num();
    auto sym = th.at("symmetry").str();
    std::vector<std::string> names;
    if (sym == "isotropic") {
        names = {"alpha"};
        p.thermal.symmetry = ThermalSymmetry::Isotropic;
    } else if (sym == "transversely_isotropic") {
        names = {"alpha_p", "alpha_t"};
        p.thermal.symmetry = ThermalSymmetry::TransverselyIsotropic;
    } else if (sym == "orthotropic") {
        names = {"alpha1", "alpha2", "alpha3"};
        p.thermal.symmetry = ThermalSymmetry::Orthotropic;
    } else
        throw std::runtime_error("thermal_expansion.symmetry: invalid");
    p.thermal.n_components = names.size();
    p.cte_table = table(th.at("table"), names, true);
    for (Real T : p.cte_table.temperatures())
        if (T < 0) {
            p.warnings.push_back(
                "thermal_expansion.table.temperatures: negative Kelvin; check units");
            break;
        }
    if (p.kind == ModelKind::ThermoElastic) {
        const auto& el = j.at("elastic");
        auto symmetry = el.at("symmetry").str();
        if (symmetry == "isotropic") {
            p.elastic_symmetry = ElasticSymmetry::Isotropic;
            names = {"E", "nu"};
        } else if (symmetry == "transversely_isotropic") {
            p.elastic_symmetry = ElasticSymmetry::TransverselyIsotropic;
            names = {"E_p", "E_t", "nu_p", "nu_pt", "G_pt"};
        } else if (symmetry == "orthotropic") {
            p.elastic_symmetry = ElasticSymmetry::Orthotropic;
            names = {"E1", "E2", "E3", "nu12", "nu13", "nu23", "G12", "G13", "G23"};
        } else
            throw std::runtime_error("elastic.symmetry: invalid");
        p.elastic_table = table(el.at("table"), names);
        for (Real T : p.elastic_table.temperatures()) {
            Real E;
            p.elastic_table.evaluate(0, T, E);
            if (E < 1e3) {
                p.warnings.push_back("elastic.table: modulus below 1e3 Pa; check units");
                break;
            }
        }
        ThermoElastic probe(p.elastic_symmetry, p.elastic_table);
        auto& ts = p.elastic_table.temperatures();
        for (std::size_t i = 0; i < ts.size(); ++i)
            for (int sample = 0; sample <= 8; ++sample) {
                Real T = ts[i];
                if (i + 1 < ts.size())
                    T += (ts[i + 1] - ts[i]) * sample / 9;
                PointInput in;
                in.T_np1 = T;
                PointOutput out;
                if (probe.update({}, in, {nullptr, 1}, {nullptr, 1}, out) != StatusCode::Ok)
                    throw std::runtime_error("elastic.table: non-SPD stiffness at T=" +
                                             std::to_string(T));
            }
    } else {
        if (j.has("elastic"))
            throw std::runtime_error("elastic: viscoelasticity must not also define a "
                                     "temperature-dependent elastic/shear table");
        const auto& v = j.at("viscoelastic");
        if (v.has("fit") && v.at("fit").has("valid_temperature_range")) {
            auto range = numbers(v.at("fit").at("valid_temperature_range"));
            if (range.size() != 2 || !(range[0] > 0 && range[1] > range[0]))
                throw std::runtime_error("viscoelastic.fit.valid_temperature_range: increasing "
                                         "positive bounds required");
            p.fit_temperature = {
                true, p.cte_table.extrapolation() == Extrapolation::Clamp, range[0], range[1]};
        }
        if (v.at("symmetry").str() != "isotropic")
            throw std::runtime_error("viscoelastic.symmetry: orthotropic NotImplemented");
        auto mode = v.at("volumetric_mode").str();
        if (mode != "elastic_bulk" && mode != "constant_poisson_from_E")
            throw std::runtime_error("viscoelastic.volumetric_mode: invalid");
        p.prony.mode = mode == "elastic_bulk" ? VolumetricMode::ElasticBulk
                                              : VolumetricMode::ConstantPoissonFromE;
        const auto& series = v.at(mode == "elastic_bulk" ? "shear" : "tensile");
        p.prony.infinity = series.at("infinity").num();
        if (p.prony.infinity < 0)
            throw std::runtime_error("viscoelastic.infinity: negative");
        if (series.at("terms").kind != Json::Array || series.at("terms").array.empty())
            throw std::runtime_error("viscoelastic.terms: nonempty array required");
        for (const auto& t : series.at("terms").array) {
            Real g = t.at("modulus").num(), tau = t.at("tau").num();
            if (g < 0 || tau <= 0)
                throw std::runtime_error(
                    "viscoelastic.terms: nonnegative modulus and positive tau required");
            if (!p.prony.terms.empty() && tau <= p.prony.terms.back().tau)
                throw std::runtime_error("viscoelastic.terms.tau: must be strictly increasing");
            p.prony.terms.push_back({g, tau});
        }
        if (mode == "elastic_bulk") {
            p.bulk_table = table(v.at("bulk_modulus"), {"K"});
            for (Real T : p.bulk_table.temperatures()) {
                Real K;
                p.bulk_table.evaluate(0, T, K);
                if (!(K > 0))
                    throw std::runtime_error("viscoelastic.bulk_modulus.K: must be positive");
            }
        } else {
            p.prony.poisson = v.at("poisson_ratio").num();
            if (!(p.prony.poisson > -1 && p.prony.poisson < 0.5))
                throw std::runtime_error("viscoelastic.poisson_ratio: out of range");
            if (p.prony.poisson > 0.45)
                p.warnings.push_back("poisson_ratio > 0.45");
            p.bulk_table =
                TemperatureTable({1}, {{1}}, Interpolation::Linear, Extrapolation::Clamp);
        }
        if (v.has("shear_modulus"))
            throw std::runtime_error(
                "viscoelastic.shear_modulus: temperature-dependent shear forbidden");
        const auto& glass = j.at("glass_transition");
        p.shift.T_g = glass.at("T_g").num();
        glass.at("method").str();
        p.shift.T_ref = v.at("reference_temperature").num();
        const auto& shift = v.at("shift");
        auto st = shift.at("type").str();
        if (st == "WLF")
            p.shift.type = ShiftType::WLF;
        else if (st == "Arrhenius")
            p.shift.type = ShiftType::Arrhenius;
        else if (st == "table")
            p.shift.type = ShiftType::Table;
        else if (st == "piecewise_wlf_arrhenius")
            p.shift.type = ShiftType::PiecewiseWlfArrhenius;
        else
            throw std::runtime_error("viscoelastic.shift.type: invalid");
        if (st == "WLF" || st == "piecewise_wlf_arrhenius") {
            p.shift.C1 = shift.at("C1").num();
            p.shift.C2 = shift.at("C2").num();
            Real low = st == "WLF" ? p.cte_table.temperatures().front() : p.shift.T_g;
            if (p.shift.C2 + low - p.shift.T_ref <= 0)
                throw std::runtime_error("shift.C2: WLF singularity in valid range");
        }
        if (st == "Arrhenius" || st == "piecewise_wlf_arrhenius") {
            p.shift.E_a = shift.at("activation_energy").num();
            if (p.shift.E_a <= 0)
                throw std::runtime_error("shift.activation_energy: must be positive");
        }
        if (st == "table") {
            p.shift.table_T = numbers(shift.at("temperatures"));
            p.shift.table_log10 = numbers(shift.at("log10_aT"));
            TemperatureTable check(p.shift.table_T, {p.shift.table_log10});
            if (!check.strictly_increasing())
                throw std::runtime_error("shift.temperatures: duplicate knots");
        }
        if (p.shift.T_g < p.cte_table.temperatures().front() ||
            p.shift.T_g > p.cte_table.temperatures().back())
            throw std::runtime_error("glass_transition.T_g: outside valid temperature range");
    }
    if (j.has("activation"))
        p.has_activation = true;
    if (j.has("chemical_shrinkage")) {
        p.has_shrinkage = true;
        const auto& ch = j.at("chemical_shrinkage");
        if (ch.has("gel_conversion"))
            throw std::runtime_error("chemical_shrinkage.gel_conversion: forbidden");
        if (ch.at("model").str() != "equivalent_isotropic")
            throw std::runtime_error("chemical_shrinkage.model: unsupported");
        const auto& sh = ch.at("post_gel_shrinkage");
        if (sh.has("gel_conversion"))
            throw std::runtime_error(
                "chemical_shrinkage.post_gel_shrinkage.gel_conversion: forbidden");
        auto measure = sh.at("measure").str();
        if (measure != "linear" && measure != "volumetric")
            throw std::runtime_error("chemical_shrinkage.post_gel_shrinkage.measure: invalid");
        Real effective = sh.at("value").num() * (measure == "linear" ? 3 : 1);
        if (!(effective >= 0 && effective < 1))
            throw std::runtime_error(
                "chemical_shrinkage.post_gel_shrinkage.value: effective volume must be in [0,1)");
        const auto& schedule = j.at("chemical_shrinkage").at("schedule");
        if (schedule.has("cure_step_id"))
            throw std::runtime_error(
                "chemical_shrinkage.schedule: obsolete scalar binding is forbidden");
        const auto& ids = schedule.at("cure_step_ids");
        if (ids.kind != Json::Array || ids.array.size() != 1)
            throw std::runtime_error(
                "chemical_shrinkage.schedule.cure_step_ids: P0 requires exactly one id");
        if (schedule.has("gel_conversion"))
            throw std::runtime_error("chemical_shrinkage.schedule.gel_conversion: forbidden");
        auto id = ids.array[0].str();
        if (id.empty())
            throw std::runtime_error("chemical_shrinkage.schedule.cure_step_ids: empty id");
        p.cure_step_ids.push_back(id);
        auto profile = schedule.has("profile") ? schedule.at("profile").str() : "linear_ramp";
        std::vector<Real> times, fractions;
        if (profile == "table") {
            times = numbers(schedule.at("fractions").at("t_norm"));
            fractions = numbers(schedule.at("fractions").at("fraction"));
        } else if (profile != "linear_ramp")
            throw std::runtime_error("chemical_shrinkage.schedule.profile: unsupported");
        auto step = process_step_id(id);
        bool ordered = false;
        if (j.has("process_step_ids")) {
            bool found = false;
            unsigned index = 0;
            for (const auto& candidate : j.at("process_step_ids").array) {
                if (candidate.str() == id) {
                    if (found)
                        throw std::runtime_error("process_step_ids: duplicate cure step");
                    found = true;
                    step = index;
                }
                ++index;
            }
            if (!found)
                throw std::runtime_error(
                    "chemical_shrinkage.schedule.cure_step_ids: unknown process step");
            ordered = true;
        }
        p.shrinkage = ShrinkageSchedule(effective,
                                        step,
                                        schedule.has("t_start") ? schedule.at("t_start").num() : 0,
                                        schedule.has("t_end") ? schedule.at("t_end").num() : 1,
                                        times,
                                        fractions,
                                        ordered);
    }
    return p;
}
} // namespace
ValidationResult validate_material_json(std::string_view json, bool customer_delivery) {
    ValidationResult r;
    try {
        decode(Reader{json}.read(), customer_delivery);
    } catch (const std::exception& e) {
        r.ok = false;
        std::string message = e.what();
        auto colon = message.find(':');
        r.issues.push_back(
            {colon == std::string::npos ? "material" : message.substr(0, colon), message});
    }
    return r;
}
MaterialParams parse_material_json(std::string_view json, bool customer_delivery) {
    auto p = decode(Reader{json}.read(), customer_delivery);
    p.parameter_hash = 14695981039346656037ull;
    for (unsigned char c : json) {
        p.parameter_hash ^= c;
        p.parameter_hash *= 1099511628211ull;
    }
    return p;
}
ValidationResult validate_material_file(const std::string& path, bool customer_delivery) {
    std::ifstream f(path);
    if (!f)
        return {false, {{path, "cannot open file"}}};
    std::ostringstream s;
    s << f.rdbuf();
    return validate_material_json(s.str(), customer_delivery);
}
} // namespace chipcae::material
