set(CPACK_GENERATOR TGZ)
set(CPACK_PACKAGE_NAME chipcae)
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
string(TOLOWER "${CMAKE_SYSTEM_NAME}" _chipcae_os)
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _chipcae_arch)
set(CPACK_PACKAGE_FILE_NAME "chipcae-${PROJECT_VERSION}-${_chipcae_os}-${_chipcae_arch}")
# Dependencies are not copied or bundled; package the project install layout.
set(CPACK_INSTALL_CMAKE_PROJECTS "${PROJECT_BINARY_DIR};${PROJECT_NAME};ALL;/")
include(CPack)
