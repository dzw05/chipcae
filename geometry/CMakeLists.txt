add_library(chipcae_geometry
    src/geometry.cpp
    src/import_thresholds.cpp
    src/import_report.cpp
    src/import_accept.cpp
    src/repair_policy.cpp
)
add_library(chipcae::geometry ALIAS chipcae_geometry)

target_include_directories(chipcae_geometry
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(chipcae_geometry PUBLIC chipcae::core)
chipcae_set_warnings(chipcae_geometry)

if(CHIPCAE_ENABLE_OCCT)
    target_sources(chipcae_geometry PRIVATE src/step_import.cpp)
    target_include_directories(chipcae_geometry SYSTEM PRIVATE ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(chipcae_geometry PRIVATE TKernel TKBRep TKSTEP TKSTL
        TKXSBase TKXCAF TKXDESTEP TKShHealing TKTopAlgo TKBO TKMesh TKBool)
    target_compile_definitions(chipcae_geometry PRIVATE CHIPCAE_ENABLE_OCCT=1)
else()
    target_sources(chipcae_geometry PRIVATE src/step_import_unavailable.cpp)
endif()

# The test target is created later by the top-level tests subdirectory.
if(CHIPCAE_BUILD_TESTS)
    cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL
        target_compile_definitions chipcae_test_repair_policy PRIVATE
        CHIPCAE_ACCEPT_TEST_JSONCONS=$<BOOL:${CHIPCAE_ENABLE_JSONCONS}>)
endif()

if(CHIPCAE_ENABLE_JSONCONS)
    target_link_libraries(chipcae_geometry PRIVATE jsoncons)
    set_source_files_properties(src/import_accept.cpp PROPERTIES
        COMPILE_DEFINITIONS CHIPCAE_ACCEPT_JSONCONS=1)
endif()
