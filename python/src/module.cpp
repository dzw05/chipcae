#include "chipcae/core/version.hpp"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <string>

NB_MODULE(chipcae_py, module) {
    module.doc() = "Python bindings for chipcae";
    module.attr("__version__") = std::string(chipcae::version_string());
}
