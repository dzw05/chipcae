#include "chipcae/core/version.hpp"

#include <pybind11/pybind11.h>
#include <string>

PYBIND11_MODULE(chipcae_py, module) {
    module.doc() = "Python bindings for chipcae";
    module.attr("__version__") = std::string(chipcae::version_string());
}
