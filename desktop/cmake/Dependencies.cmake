set(CHIPCAE_SDK_PREFIX "$ENV{CHIPCAE_SDK_PREFIX}" CACHE PATH "Optional desktop SDK prefix")
if(CHIPCAE_SDK_PREFIX)
    list(PREPEND CMAKE_PREFIX_PATH "${CHIPCAE_SDK_PREFIX}")
endif()
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
   NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "15.3.0")
    message(FATAL_ERROR "Desktop SDK requires GNU 15.3.0")
endif()
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("#include <string>
#if !defined(_GLIBCXX_USE_CXX11_ABI) || _GLIBCXX_USE_CXX11_ABI != 1
#error Desktop SDK requires C++11 ABI
#endif
int main() { return 0; }" CHIPCAE_HAS_CXX11_ABI)
if(NOT CHIPCAE_HAS_CXX11_ABI)
    message(FATAL_ERROR "Desktop SDK requires libstdc++ C++11 ABI")
endif()
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets OpenGLWidgets)
find_package(VTK 9.7 REQUIRED COMPONENTS CommonCore CommonDataModel FiltersSources
    RenderingCore RenderingOpenGL2 InteractionStyle GUISupportQt IOHDF IOImage
    ChartsCore ViewsContext2D RenderingContextOpenGL2)
if(NOT VTK_VERSION VERSION_LESS 9.8)
    message(FATAL_ERROR "D01 requires VTK 9.7.x")
endif()
if(NOT VTK_QT_VERSION STREQUAL "6")
    message(FATAL_ERROR "VTK must use Qt6; found '${VTK_QT_VERSION}'")
endif()
# Always use the repository's pinned, relocatable header-only package.
set(jsoncons_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/jsoncons/share/cmake/jsoncons")
find_package(jsoncons 1.4.0 EXACT CONFIG REQUIRED NO_DEFAULT_PATH)
find_package(Protobuf 35.1.0 EXACT CONFIG REQUIRED)
message(STATUS "Desktop SDK: Qt ${Qt6_VERSION}, VTK ${VTK_VERSION}, protobuf ${Protobuf_VERSION}")
