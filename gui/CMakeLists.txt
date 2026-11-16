if(NOT CHIPCAE_ENABLE_QT)
    message(FATAL_ERROR "CHIPCAE_BUILD_GUI requires CHIPCAE_ENABLE_QT / Qt6")
endif()

add_executable(chipcae-gui src/main.cpp)
target_link_libraries(chipcae-gui PRIVATE chipcae::core Qt6::Core Qt6::Widgets)
chipcae_set_warnings(chipcae-gui)
