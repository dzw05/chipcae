# Standalone acceptance suite: configure this directory directly, not the whole tree.
cmake_minimum_required(VERSION 3.20)
project(chipcae_material_gate_acceptance LANGUAGES NONE)

enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

foreach(directory cte_contract temp_edges)
    foreach(mode missing-undefined missing-off missing-on target-off target-on)
        add_test(NAME material_gate_${directory}_${mode}
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/acceptance.py"
                ${mode} --directory ${directory})
        set_tests_properties(material_gate_${directory}_${mode} PROPERTIES LABELS "material;tierA")
    endforeach()
endforeach()

# MAT_GATE_CMAKE_ARGS is a JSON array of the runbook's dependency/compiler arguments.
# MAT_GATE_KOKKOS_ROOT supplies an existing installation for the enabled configuration.
# Optional MAT_GATE_BUILD_ROOT keeps generated whole-tree builds in a chosen scratch directory.
# No timeout: a nested configure may legitimately wait for a shared build slot.
foreach(mode tree-skip tree-kokkos)
    add_test(NAME material_gate_${mode}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/acceptance.py" ${mode})
    set_tests_properties(material_gate_${mode} PROPERTIES LABELS "material;integration")
endforeach()
