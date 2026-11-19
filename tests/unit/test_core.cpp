#include "chipcae/core/field.hpp"
#include "chipcae/core/log.hpp"
#include "chipcae/core/parallel.hpp"
#include "chipcae/core/parameter.hpp"
#include "chipcae/core/version.hpp"
#include "chipcae/coupling/field_mapper.hpp"
#include "chipcae/io/manifest.hpp"
#include "chipcae/jobmgr/local_paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

int failed = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failed;
    }
}

} // namespace

int main() {
    expect(chipcae::version_string() == "0.1.0", "version_string should be 0.1.0");
    expect(chipcae::version_major == 0, "version_major");
    expect(chipcae::version_minor == 1, "version_minor");

    chipcae::Field field("temperature", 4, 1);
    expect(field.name() == "temperature", "field name");
    expect(field.size() == 4, "field size");
    expect(field.components() == 1, "field components");
    expect(field.value_count() == 4, "field value count");
    field.values()[0] = 350.0;
    expect(field.values()[0] == 350.0, "field value assignment");

    chipcae::Field displaced("displacement", 4, 3);
    expect(displaced.value_count() == 12, "vector field value count");

    chipcae::coupling::FieldMapper mapper;
    chipcae::Field copy("temperature_copy", 4, 1);
    mapper.map(field, copy);
    expect(copy.values()[0] == 350.0, "field mapper copies values");

    chipcae::ParameterSet parameters;
    parameters.declare({"nx", chipcae::ParameterType::integer, "8", "cells in x"});
    expect(parameters.get("nx") == "8", "parameter default");
    parameters.set("nx", "16");
    expect(parameters.get("nx") == "16", "parameter override");
    expect(parameters.get("missing") == std::nullopt, "missing parameter");

    const auto comm = chipcae::Communicator::world();
    expect(comm.rank() == 0, "serial communicator rank");
    expect(comm.size() == 1, "serial communicator size");
    expect(comm.is_serial(), "serial communicator flag");

    chipcae::set_log_level(chipcae::LogLevel::error);
    chipcae::log(chipcae::LogLevel::info, "this message should be filtered");

    const auto run_id = chipcae::jobmgr::make_run_id();
    expect(chipcae::jobmgr::is_uuid(run_id), "generated run id is valid");
    expect(!chipcae::jobmgr::is_uuid(""), "empty run id is invalid");

    const auto manifest_path =
        std::filesystem::temp_directory_path() / "chipcae_manifest_test.json";
    chipcae::io::ResultManifest manifest;
    manifest.add({"temperature", "temperature.vtkhdf", "field"});
    manifest.write(manifest_path);
    expect(std::filesystem::exists(manifest_path), "manifest file written");
    std::ifstream manifest_input(manifest_path);
    const std::string manifest_text((std::istreambuf_iterator<char>(manifest_input)), {});
    expect(manifest_text.find("\"manifest_version\": 1") != std::string::npos,
           "manifest contains version");
    expect(manifest_text.find("temperature.vtkhdf") != std::string::npos,
           "manifest contains added entry");
    manifest_input.close();
    std::filesystem::remove(manifest_path);

    if (failed != 0) {
        std::cerr << failed << " assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
