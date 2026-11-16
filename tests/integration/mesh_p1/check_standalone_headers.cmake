# Compile every public C++ header as its own translation unit.
if(NOT COMPILER OR NOT ROOT)
  message(FATAL_ERROR "COMPILER and ROOT must be set")
endif()
set(INCLUDE_DIRS
  "${ROOT}/core/include"
  "${ROOT}/schema/include"
  "${ROOT}/mesh/background/include"
  "${ROOT}/mesh/cut/include")
file(GLOB_RECURSE headers
  "${ROOT}/core/include/chipcae/*.hpp"
  "${ROOT}/schema/include/chipcae/*.hpp"
  "${ROOT}/mesh/background/include/chipcae/*.hpp"
  "${ROOT}/mesh/cut/include/chipcae/*.hpp")
list(LENGTH headers count)
if(count EQUAL 0)
  message(FATAL_ERROR "no public headers found")
endif()
set(failed "")
foreach(header IN LISTS headers)
  file(RELATIVE_PATH rel "${ROOT}" "${header}")
  set(cmd ${COMPILER} -std=c++20 -fsyntax-only -xc++)
  if(DEFS)
    foreach(d IN LISTS DEFS)
      list(APPEND cmd -D${d})
    endforeach()
  endif()
  foreach(dir IN LISTS INCLUDE_DIRS)
    list(APPEND cmd -I${dir})
  endforeach()
  if(INCLUDES)
    foreach(dir IN LISTS INCLUDES)
      list(APPEND cmd -I${dir})
    endforeach()
  endif()
  if(FLAGS)
    separate_arguments(flag_args NATIVE_COMMAND "${FLAGS}")
    list(APPEND cmd ${flag_args})
  endif()
  list(APPEND cmd ${header})
  execute_process(COMMAND ${cmd} RESULT_VARIABLE rc ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    string(APPEND failed "\n${rel}:\n${err}")
  endif()
endforeach()
if(failed)
  message(FATAL_ERROR "public header standalone compile failed:${failed}")
endif()
message(STATUS "Checked ${count} public headers for standalone inclusion")
