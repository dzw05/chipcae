// chipcae/material/python/bindings.cpp —— nanobind 模块 stub（选项开启时编译）
#include "chipcae/material/schema.hpp"

#include <nanobind/nanobind.h>
#include <string>

namespace nb = nanobind;

NB_MODULE(chipcae_material, m) {
    m.doc() = "chipcae material point library (v0, CHUNK 1 skeleton)";
    m.attr("__version__") = "0.1.0";

    // cte_convert / single-point driver / IMaterial bindings arrive in later chunks.
    m.def("validate_material_json", [](const std::string& json) {
        const auto result = chipcae::material::validate_material_json(json);
        return result.ok;
    });
}
