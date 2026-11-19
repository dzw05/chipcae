#include "chipcae/schema/stack.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <jsoncons/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;
using Json = jsoncons::json;
using chipcae::mesh::StackModel;
constexpr auto stack_name = "copper_polygons.stack.json";
constexpr auto polyset_name = "copper_polygons.polyset.json";

struct Workspace {
    fs::path original = fs::current_path();
    fs::path root;

    Workspace() {
        const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned i = 0; i < 100; ++i) {
            auto candidate = fs::temp_directory_path() /
                             ("stack-relpath-" + std::to_string(seed) + "-" + std::to_string(i));
            if (fs::create_directory(candidate)) {
                root = fs::canonical(candidate);
                return;
            }
        }
        throw std::runtime_error("cannot create unique temporary directory");
    }
    ~Workspace() noexcept {
        std::error_code ec;
        fs::current_path(original, ec);
        fs::remove_all(root, ec);
    }
};

void require(bool ok, const std::string& message) {
    if (!ok)
        throw std::runtime_error(message);
}

void write_json(const fs::path& path, const Json& json) {
    std::ofstream out(path);
    out << json;
    out.close();
    require(bool(out), "cannot write fixture: " + path.string());
}

StackModel load(const fs::path& instance, const fs::path& schema) {
    auto result = chipcae::mesh::load_stack(instance, schema);
    require(bool(result), result ? "" : result.error().path + ": " + result.error().reason);
    return result.value();
}

void equal(const StackModel& a, const StackModel& b) {
    require(a.layers.size() == b.layers.size(), "layers.size() differs");
    for (std::size_t i = 0; i < a.layers.size(); ++i) {
        require(a.layers[i].name == b.layers[i].name,
                "layer " + std::to_string(i) + " name differs");
        require(a.layers[i].regions.size() == b.layers[i].regions.size(),
                "layer " + std::to_string(i) + " regions.size() differs");
    }
}

void expect_error(const fs::path& instance,
                  const fs::path& schema,
                  const std::string& path,
                  const std::string& reason) {
    const auto result = chipcae::mesh::load_stack(instance, schema);
    require(!result, "expected error, got success");
    require(result.error().path == path && result.error().reason == reason,
            "expected " + path + ": " + reason + "; got " + result.error().path + ": " +
                result.error().reason);
}

struct Runner {
    unsigned failures = 0;
    unsigned cases = 0;
    template <class F>
    void run(const std::string& name, F action) {
        ++cases;
        try {
            action();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        }
    }
};
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected repository root argument");
        const auto repo = fs::absolute(argv[1]);
        const auto schema = repo / "schema/stack-0.2.schema.json";
        const auto examples = repo / "schema/examples";
        std::ifstream source(examples / stack_name);
        const auto original = Json::parse(source);
        Workspace workspace;
        Runner runner;
        const auto normal = workspace.root / "normal";
        fs::create_directory(normal);
        fs::copy_file(examples / stack_name, normal / stack_name);
        fs::copy_file(examples / polyset_name, normal / polyset_name);
        fs::current_path(normal);
        std::optional<StackModel> bare, dot, absolute;
        runner.run("relpath_bare", [&] { bare = load(stack_name, schema); });
        runner.run("relpath_bare_repeat", [&] {
            // Two consecutive calls in this process must each succeed.
            const auto first = load(stack_name, schema);
            const auto second = load(stack_name, schema);
            equal(first, second);
        });
        runner.run("relpath_dot", [&] { dot = load(fs::path(".") / stack_name, schema); });
        runner.run("relpath_abs", [&] { absolute = load(normal / stack_name, schema); });
        runner.run("relpath_equivalent", [&] {
            require(bare && dot && absolute, "one or more invocation forms did not load");
            equal(*bare, *dot);
            equal(*bare, *absolute);
        });

        const auto permissive = workspace.root / "runtime.schema.json";
        write_json(permissive, Json::object());
        for (const std::string kind : {"dotdot", "abs", "symlink"}) {
            const auto parent = workspace.root / kind;
            const auto case_dir = parent / "case";
            fs::create_directories(case_dir);
            fs::copy_file(examples / polyset_name, parent / polyset_name);
            auto fixture = original;
            std::string reference;
            if (kind == "dotdot")
                reference = "../" + std::string(polyset_name);
            else if (kind == "abs")
                reference = (parent / polyset_name).string();
            else {
                reference = polyset_name;
                fs::create_symlink(parent / polyset_name, case_dir / polyset_name);
            }
            fixture["external_refs"][0]["path"] = reference;
            fixture["layers"][0]["copper"]["polygons"]["file"] = reference;
            write_json(case_dir / stack_name, fixture);
            fs::current_path(case_dir);
            for (const std::string form : {"bare", "dot", "abs"}) {
                const fs::path instance = form == "bare"  ? fs::path(stack_name)
                                          : form == "dot" ? fs::path(".") / stack_name
                                                          : case_dir / stack_name;
                runner.run("relpath_escape_" + kind + "_" + form, [&] {
                    expect_error(
                        instance, permissive, "$/external_refs/path", "path escapes case root");
                });
            }
        }
        fs::current_path(normal);
        for (const bool missing : {true, false}) {
            runner.run(missing ? "external_refs_missing" : "external_refs_empty", [&] {
                auto fixture = original;
                fixture["layers"][0]["copper"].erase("polygons");
                fixture["layers"][0]["copper"]["fraction"] = 0.5;
                if (missing)
                    fixture.erase("external_refs");
                else
                    fixture["external_refs"] = Json::array();
                write_json(normal / "no-refs.json", fixture);
                load("no-refs.json", schema);
            });
        }
        runner.run("instance_empty",
                   [&] { expect_error({}, schema, "$", "cannot open instance or schema"); });
        std::cout << runner.cases << " cases, " << runner.failures << " failures\n";
        return runner.failures == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "FAIL fixture_setup: " << e.what() << '\n';
        return 1;
    }
}
