if(NOT CHIPCAE_ENABLE_PYTHON)
    return()
endif()

nanobind_add_module(chipcae_py NB_STATIC src/module.cpp)
target_link_libraries(chipcae_py PRIVATE chipcae::core chipcae::mesh)
chipcae_set_warnings(chipcae_py)
