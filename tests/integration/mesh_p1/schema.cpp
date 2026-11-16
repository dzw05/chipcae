#include "../../../schema/src/sha256.hpp"
#include "chipcae/schema/stack.hpp"

#include <fstream>
#include <iostream>
#include <jsoncons/json.hpp>
#include <stdexcept>
using namespace chipcae::mesh;
void expect(bool ok) {
    if (!ok)
        throw std::runtime_error("schema expectation failed");
}
int main() {
    auto root = std::filesystem::path(K1_ROOT);
    auto schema = root / "schema/stack-0.1.schema.json";
    for (auto name : {"fcbga_demo", "interposer_2p5d"}) {
        auto r =
            load_stack(root / (std::string("schema/examples/") + name + ".stack.json"), schema);
        if (!r)
            std::cerr << r.error().path << ": " << r.error().reason << '\n';
        expect(bool(r));
    }
    expect(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::ifstream in(root / "schema/examples/fcbga_demo.stack.json");
    auto base = jsoncons::json::parse(in);
    const auto path = std::filesystem::current_path() / "negative.stack.json";
    for (int i = 0; i < 11; ++i) {
        auto j = base;
        if (i < 2) {
            j["external_refs"] = jsoncons::json::parse(
                R"([{"path":"https://bad","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}])");
            if (i == 1) {
                j["external_refs"][0]["path"] = "external.bin";
                j["external_refs"][0].erase("sha256");
            }
        }
        if (i == 2)
            j["activation_groups"][0]["members"][0] = "group:assembly";
        if (i == 3)
            j["layers"][1]["name"] = "substrate";
        if (i == 4)
            j["activation_groups"][0]["members"][0] = "layer:absent";
        if (i == 5)
            j["layers"][0]["material"] = "SAC305";
        if (i == 6)
            j["layers"][2]["regions"][0]["footprint"]["box"][2] = 1;
        if (i == 7 || i == 8)
            j["external_refs"] = jsoncons::json::parse(
                i == 7
                    ? R"([{"path":"../escape","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}])"
                    : R"([{"path":"missing","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}])");
        if (i == 9)
            j["symmetry"] = "quarter";
        if (i == 10)
            j["symmetry"] = "invalid";
        {
            std::ofstream out(path);
            out << j;
        }
        auto result = load_stack(path, schema);
        expect(!result);
        expect(!result.error().reason.empty());
        const std::string paths[] = {"external_refs",
                                     "external_refs",
                                     "activation_groups",
                                     "layers",
                                     "activation_groups",
                                     "material",
                                     "footprint",
                                     "external_refs",
                                     "external_refs",
                                     "symmetry",
                                     "symmetry"};
        expect(result.error().path.find(paths[i]) != std::string::npos);
    }
    auto positive = base;
    positive["layers"][0]["material"] = "SAC305_ball_eq";
    {
        std::ofstream out(path);
        out << positive;
    }
    expect(bool(load_stack(path, schema)));
    auto external = std::filesystem::current_path() / "external.bin";
    {
        std::ofstream bytes(external);
        bytes << "abc";
    }
    base["external_refs"] = jsoncons::json::parse(
        R"([{"path":"external.bin","sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}])");
    {
        std::ofstream out(path);
        out << base;
    }
    expect(bool(load_stack(path, schema)));
    {
        std::ofstream bytes(external);
        bytes << "abd";
    }
    auto mismatch = load_stack(path, schema);
    expect(!mismatch && mismatch.error().reason == "digest mismatch");
    std::filesystem::remove(external);
    std::filesystem::remove(path);
}
