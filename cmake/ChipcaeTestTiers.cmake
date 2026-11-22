# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
include_guard(GLOBAL)

# Ownership source of truth: MODULE|source prefix. Longest prefix wins, including
# file-stem prefixes such as test_import. Keep module codes here, not team names.
set(_CHIPCAE_TEST_OWNER_PREFIXES
    "M1|geometry/" "M1|kernel/src/import_"
    "M1|tests/unit/test_ipc2581" "M1|tests/unit/test_odbpp"
    "M1|tests/unit/test_step" "M1|tests/unit/test_assembly"
    "M1|tests/unit/test_repair_policy" "M1|tests/unit/test_import"
    "M1|tests/unit/test_ecad" "M1|tests/unit/test_kicad"
    "M1|tests/integration/step_import/"
    "M1|tests/integration/import_cli/" # Lead override: import CLI belongs to M1.
    "M1|tests/integration/import_assembly/"
    "M1|tests/unit/assembly/" "M1|tests/unit/ecad_diff/"
    "M2|mesh/" "M2|preview/" "M2|tests/integration/mesh_p1/"
    "M2|tests/unit/region_ls/" "M2|tests/fixtures/mesh/"
    "M3|apps/chipcae-solve/" "M3|solvers/fem/" "M3|solvers/fvm/" "M3|tests/solve/"
    "M4|material/" "M4|tests/unit/test_material" "M4|tests/unit/test_cte"
    "M4|tests/unit/cte_contract/" "M4|tests/unit/copper_fraction/"
    "M5|io/" "M5|tests/io/" "M5|tests/unit/result_summary/" "M5|tests/unit/report_fields/"
    "M6|protocol/" "M6|jobmgr/" "M6|core/" "M6|coupling/" "M6|kernel/"
    "M6|python/" "M6|tests/protocol/"
    "M7|desktop/" "M7|gui/"
    "INT|tests/integration/" "INT|tests/vv/" "INT|tools/vv/" "INT|examples/"
    "ARCH|schema/" "ARCH|tests/contract/"
    "INFRA|cmake/" "INFRA|tests/auto_selftest/" "INFRA|tests/install/"
    "INFRA|CMakeLists.txt" "INFRA|tests/unit/bench_measure/"
    # Additional assignments after inspecting the exercised APIs/tools:
    "INT|tests/unit/qoi_mirror/" # tools/vv/warpage_qoi mirror reconstruction.
    "M4|tests/unit/temp_edges/" # material::TemperatureTable boundary behavior.
    "INT|tests/unit/sample_docs/" # Public example/case documentation contracts.
    "M2|tests/integration/tracemap/" # mesh copper trace map and FVM face coupling.
    "M1|tests/integration/part_slice/" # geometry::slice_prismatic and OCCT sections.
    "INFRA|tests/tier_labels/"
)

# Manual registrations in tests/CMakeLists.txt run sources in several modules.
# These rules reflect the sources/commands there; they apply only in that dir.
set(_CHIPCAE_TEST_MANUAL_OWNERS
    "M6|^(core.*|event_log|par.*|control_file)$" # core/, protocol/, par/ sources.
    "M2|^(mesh|column_hierarchy|copper_map.*)$" # mesh/background and copper map.
    "M1|^(import_thresholds|repair_policy.*|step_.*|odbpp_.*|ipc.*|ecad_.*|diagnostics_header_standalone)$"
    "M5|^(vtkhdf_results_.*|cut_results_.*)$" # tests/test_{vtkhdf,cut}_results.py.
    "ARCH|^(contract_.*|case_run_schema_fixtures)$" # schema/ and tests/contract/.
)

function(_chipcae_test_owner source directory name output)
    set(_owner "")
    set(_longest -1)
    foreach(_rule IN LISTS _CHIPCAE_TEST_OWNER_PREFIXES)
        string(REPLACE "|" ";" _parts "${_rule}")
        list(GET _parts 0 _module)
        list(GET _parts 1 _prefix)
        string(FIND "${source}" "${_prefix}" _index)
        string(LENGTH "${_prefix}" _length)
        if(_index EQUAL 0 AND _length GREATER _longest)
            set(_owner "${_module}")
            set(_longest "${_length}")
        endif()
    endforeach()
    if(directory STREQUAL "${PROJECT_SOURCE_DIR}/tests" AND source STREQUAL "tests/")
        foreach(_rule IN LISTS _CHIPCAE_TEST_MANUAL_OWNERS)
            # Split at the first | only; regex alternations also contain |.
            string(REGEX MATCH "^([^|]+)\\|(.*)$" _match "${_rule}")
            set(_module "${CMAKE_MATCH_1}")
            set(_regex "${CMAKE_MATCH_2}")
            if(name MATCHES "${_regex}")
                set(_owner "${_module}")
                break()
            endif()
        endforeach()
    endif()
    set(${output} "${_owner}" PARENT_SCOPE)
endfunction()

function(_chipcae_test_tier name source labels timeout output)
    # Explicit tier labels are handled by the caller before inference. Never
    # infer highmem: it must remain identical to the measured resource-lock set.
    if("performance" IN_LIST labels OR "benchmark" IN_LIST labels
            OR name MATCHES "(^|_)(perf|performance|benchmark|throughput|timing)(_|$)")
        set(_tier perf)
    elseif("install" IN_LIST labels OR "nightly" IN_LIST labels
            OR source MATCHES "^tests/install/"
            OR source MATCHES "^tests/integration/(real_case_e2e|real_case_crosscheck|demo_gate|f1130_uncut_cli|fvm_case_e2e|odbpp_cli_e2e|import_solve_chain|step_parts_e2e)/"
            OR source MATCHES "^tests/vv/calculix_demo_gate/"
            OR name MATCHES "e2e|demo_?gate|real_?case|crosscheck")
        set(_tier acceptance)
    elseif(labels MATCHES "(^|;)(numerical|mpi|par|integration|vv|mfem|gmg|cut|hdf5|fvm)(;|$)"
            OR source MATCHES "^tests/(integration|vv)/"
            OR timeout GREATER_EQUAL 120)
        # MPI launchers (including Python wrappers) register an MPI label,
        # a module directory or timeout. A name containing no_mpi is not MPI.
        set(_tier module)
    else()
        set(_tier unit)
    endif()
    set(${output} "${_tier}" PARENT_SCOPE)
endfunction()

function(_chipcae_test_directories directory output)
    set(_directories "${directory}")
    get_property(_children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_child IN LISTS _children)
        _chipcae_test_directories("${_child}" _descendants)
        list(APPEND _directories ${_descendants})
    endforeach()
    set(${output} "${_directories}" PARENT_SCOPE)
endfunction()

function(chipcae_apply_test_tiers)
    if(CMAKE_VERSION VERSION_LESS 3.28)
        message(WARNING "CTest tier/owner assignment needs CMake >= 3.28; skipped. Run ctest_tier_labels to audit labels.")
        return()
    endif()
    set(_tiers unit module highmem perf acceptance)
    set(_owners M1 M2 M3 M4 M5 M6 M7 INT ARCH INFRA)
    foreach(_key IN LISTS _tiers _owners)
        set(_count_${_key} 0)
    endforeach()
    set(_errors "")
    _chipcae_test_directories("${PROJECT_SOURCE_DIR}" _directories)
    foreach(_directory IN LISTS _directories)
        get_property(_tests DIRECTORY "${_directory}" PROPERTY TESTS)
        foreach(_test IN LISTS _tests)
            get_property(_labels TEST "${_test}" DIRECTORY "${_directory}" PROPERTY LABELS)
            get_property(_timeout TEST "${_test}" DIRECTORY "${_directory}" PROPERTY TIMEOUT)
            get_property(_source GLOBAL PROPERTY "CHIPCAE_TEST_SOURCE_${_test}")
            if(_source)
                file(RELATIVE_PATH _source "${PROJECT_SOURCE_DIR}" "${_source}")
            else()
                file(RELATIVE_PATH _source "${PROJECT_SOURCE_DIR}" "${_directory}")
                string(APPEND _source "/")
            endif()
            set(_test_tiers "")
            set(_test_owners "")
            foreach(_label IN LISTS _labels)
                if(_label IN_LIST _tiers)
                    list(APPEND _test_tiers "${_label}")
                endif()
                if(_label MATCHES "^owner:")
                    list(APPEND _test_owners "${_label}")
                endif()
            endforeach()
            if(NOT _test_tiers)
                _chipcae_test_tier("${_test}" "${_source}" "${_labels}" "${_timeout}" _tier)
                list(APPEND _labels "${_tier}")
                list(APPEND _test_tiers "${_tier}")
            endif()
            if(NOT _test_owners)
                _chipcae_test_owner("${_source}" "${_directory}" "${_test}" _owner)
                if(_owner)
                    list(APPEND _labels "owner:${_owner}")
                    list(APPEND _test_owners "owner:${_owner}")
                endif()
            endif()
            set_property(TEST "${_test}" DIRECTORY "${_directory}" PROPERTY LABELS "${_labels}")
            list(LENGTH _test_tiers _tier_count)
            list(LENGTH _test_owners _owner_count)
            if(NOT _tier_count EQUAL 1 OR NOT _owner_count EQUAL 1)
                list(APPEND _errors "${_test} (${_source}): tiers=[${_test_tiers}], owners=[${_test_owners}]")
                continue()
            endif()
            list(GET _test_tiers 0 _tier)
            list(GET _test_owners 0 _owner)
            string(REGEX REPLACE "^owner:" "" _owner "${_owner}")
            if(NOT _owner IN_LIST _owners)
                list(APPEND _errors "${_test}: unknown owner:${_owner}")
                continue()
            endif()
            math(EXPR _count_${_tier} "${_count_${_tier}} + 1")
            math(EXPR _count_${_owner} "${_count_${_owner}} + 1")
        endforeach()
    endforeach()
    if(_errors)
        list(JOIN _errors "\n  " _details)
        message(FATAL_ERROR "Invalid CTest tier/owner labels:\n  ${_details}")
    endif()
    set(_summary "CTest labels:")
    foreach(_key IN LISTS _tiers _owners)
        string(APPEND _summary " ${_key}=${_count_${_key}}")
    endforeach()
    message(STATUS "${_summary}")
endfunction()
