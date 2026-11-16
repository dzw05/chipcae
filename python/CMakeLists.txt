if(NOT CHIPCAE_ENABLE_PYTHON)
    return()
endif()

pybind11_add_module(chipcae_py src/module.cpp)
target_link_libraries(chipcae_py PRIVATE chipcae::core chipcae::mesh)
chipcae_set_warnings(chipcae_py)
