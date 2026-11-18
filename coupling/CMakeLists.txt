add_library(chipcae_coupling
    src/coupler.cpp
    src/field_mapper.cpp
)
add_library(chipcae::coupling ALIAS chipcae_coupling)

target_include_directories(chipcae_coupling
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(chipcae_coupling PUBLIC chipcae::core)
chipcae_set_warnings(chipcae_coupling)

if(CHIPCAE_ENABLE_PRECICE)
    target_sources(chipcae_coupling PRIVATE src/precice_adapter.cpp)
endif()

target_sources(chipcae_coupling PRIVATE src/step_loop.cpp)

target_sources(chipcae_coupling PRIVATE src/control_channel.cpp)
target_link_libraries(chipcae_coupling PUBLIC chipcae::io)
target_sources(chipcae_coupling PRIVATE src/run_control.cpp)
target_sources(chipcae_coupling PRIVATE src/residual_decimator.cpp)
