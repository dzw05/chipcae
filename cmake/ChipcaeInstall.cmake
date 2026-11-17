include(GNUInstallDirs)
option(CHIPCAE_INSTALL_EXAMPLES "Install the fcbga_demo example inputs" ON)

# Kernel and jobmgr are skeleton developer entry points, not distribution tools.
install(TARGETS chipcae-solve chipcae-preview chipcae-import
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
file(GLOB _chipcae_schemas CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/schema/*.schema.json"
    "${PROJECT_SOURCE_DIR}/material/schema/*.schema.json")
install(FILES ${_chipcae_schemas} DESTINATION ${CMAKE_INSTALL_DATADIR}/chipcae/schema)
install(FILES ${PROJECT_SOURCE_DIR}/protocol/errors.yaml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/chipcae/protocol)
if(CHIPCAE_INSTALL_EXAMPLES)
    install(FILES
        ${PROJECT_SOURCE_DIR}/schema/examples/fcbga_demo.case.json
        ${PROJECT_SOURCE_DIR}/schema/examples/fcbga_demo.case.yaml
        ${PROJECT_SOURCE_DIR}/schema/examples/fcbga_demo.stack.json
        ${PROJECT_SOURCE_DIR}/schema/examples/fcbga_demo.materials.json
        ${PROJECT_SOURCE_DIR}/schema/examples/local.run-profile.json
        DESTINATION ${CMAKE_INSTALL_DATADIR}/chipcae/examples)
endif()
install(FILES ${PROJECT_SOURCE_DIR}/LICENSE ${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES
    ${PROJECT_SOURCE_DIR}/material/LICENSE-NOTES.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

foreach(target chipcae-solve chipcae-preview chipcae-import)
    set_target_properties(${target} PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)
endforeach()

if(APPLE)
    foreach(target chipcae-solve chipcae-preview chipcae-import)
        set_target_properties(${target} PROPERTIES MACOSX_RPATH ON
            INSTALL_RPATH "@loader_path/../lib" INSTALL_RPATH_USE_LINK_PATH TRUE)
    endforeach()
endif()
