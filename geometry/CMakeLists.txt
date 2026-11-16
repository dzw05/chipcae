add_library(chipcae_geometry
    src/geometry.cpp
)
add_library(chipcae::geometry ALIAS chipcae_geometry)

target_include_directories(chipcae_geometry
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(chipcae_geometry PUBLIC chipcae::core)
chipcae_set_warnings(chipcae_geometry)

if(CHIPCAE_ENABLE_OCCT)
    target_link_libraries(chipcae_geometry PRIVATE TKernel TKBRep TKSTEP TKSTL)
    target_compile_definitions(chipcae_geometry PRIVATE CHIPCAE_ENABLE_OCCT=1)
endif()
