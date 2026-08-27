# Configure-time architecture guard for POM2's logical source layers.
#
# The project intentionally still emits one static archive. This checker makes
# the source manifests meaningful meanwhile: every first-party quoted header
# reached from a classified file must itself be classified, and a lower layer
# may not include a higher one.

function(pom2_enforce_source_layers root_dir)
    set(_pom2_layers FOUNDATION MEDIA MACHINE DEVICES RUNTIME FRONTEND)
    set(_pom2_rank_FOUNDATION 0)
    set(_pom2_rank_MEDIA      1)
    set(_pom2_rank_MACHINE    2)
    set(_pom2_rank_DEVICES    3)
    set(_pom2_rank_RUNTIME    4)
    set(_pom2_rank_FRONTEND   5)

    # Basename inventory lets includes such as "HgrPaintModel.h" resolve even
    # when the owning file lives in src/hgrpaint and is found through an include
    # directory rather than relative to the including file.
    file(GLOB_RECURSE _pom2_known_headers
        RELATIVE "${root_dir}"
        "${root_dir}/src/*.h"
        "${root_dir}/include/*.h"
        "${root_dir}/include/*.hpp")
    foreach(_header IN LISTS _pom2_known_headers)
        if(_header MATCHES "(^|/)third_party/")
            continue()
        endif()
        get_filename_component(_name "${_header}" NAME)
        string(MAKE_C_IDENTIFIER "${_name}" _name_key)
        set(_pom2_known_${_name_key} "${_header}")
    endforeach()

    macro(_pom2_register_file layer rank entry)
        if(IS_ABSOLUTE "${entry}")
            set(_abs "${entry}")
        else()
            set(_abs "${root_dir}/${entry}")
        endif()
        if(NOT EXISTS "${_abs}")
            message(FATAL_ERROR
                "POM2 architecture: ${layer} manifest names missing file '${entry}'")
        endif()
        file(RELATIVE_PATH _rel "${root_dir}" "${_abs}")
        string(MAKE_C_IDENTIFIER "${_rel}" _path_key)
        if(DEFINED _pom2_owner_${_path_key}
           AND NOT _pom2_owner_${_path_key} STREQUAL "${layer}")
            message(FATAL_ERROR
                "POM2 architecture: '${_rel}' belongs to both "
                "${_pom2_owner_${_path_key}} and ${layer}")
        endif()
        set(_pom2_owner_${_path_key} "${layer}")
        set(_pom2_file_rank_${_path_key} "${rank}")
        list(APPEND _pom2_classified_files "${_rel}")

        get_filename_component(_name "${_rel}" NAME)
        string(MAKE_C_IDENTIFIER "${_name}" _name_key)
        if(DEFINED _pom2_include_owner_${_name_key}
           AND NOT _pom2_include_owner_${_name_key} STREQUAL "${layer}")
            message(FATAL_ERROR
                "POM2 architecture: duplicate include basename '${_name}' "
                "is owned by multiple layers")
        endif()
        set(_pom2_include_owner_${_name_key} "${layer}")
        set(_pom2_include_rank_${_name_key} "${rank}")
    endmacro()

    foreach(_layer IN LISTS _pom2_layers)
        set(_rank "${_pom2_rank_${_layer}}")
        if(_layer STREQUAL "FOUNDATION")
            set(_source_var "")
            set(_header_var POM2_FOUNDATION_HEADERS)
        else()
            set(_source_var "POM2_${_layer}_SOURCES")
            set(_header_var "POM2_${_layer}_HEADERS")
        endif()

        set(_entries "")
        if(_source_var)
            list(APPEND _entries ${${_source_var}})
        endif()
        list(APPEND _entries ${${_header_var}})
        foreach(_entry IN LISTS _entries)
            _pom2_register_file("${_layer}" "${_rank}" "${_entry}")
            if(_entry MATCHES "\\.cpp$")
                string(REGEX REPLACE "\\.cpp$" ".h" _inferred_header "${_entry}")
                if(EXISTS "${root_dir}/${_inferred_header}")
                    _pom2_register_file("${_layer}" "${_rank}"
                                        "${_inferred_header}")
                endif()
            endif()
        endforeach()
    endforeach()

    list(REMOVE_DUPLICATES _pom2_classified_files)
    foreach(_rel IN LISTS _pom2_classified_files)
        set(_abs "${root_dir}/${_rel}")
        string(MAKE_C_IDENTIFIER "${_rel}" _path_key)
        set(_owner "${_pom2_owner_${_path_key}}")
        set(_owner_rank "${_pom2_file_rank_${_path_key}}")
        file(STRINGS "${_abs}" _include_lines
             REGEX "^[ \t]*#[ \t]*include[ \t]*\"[^\"]+\"")
        foreach(_line IN LISTS _include_lines)
            string(REGEX REPLACE "^[^\"]*\"([^\"]+)\".*$" "\\1"
                   _include "${_line}")
            get_filename_component(_include_name "${_include}" NAME)
            if(_include MATCHES "(^|/)third_party/"
               OR _include_name MATCHES "^stb_.*\\.h$")
                continue()
            endif()
            string(MAKE_C_IDENTIFIER "${_include_name}" _include_key)

            if(NOT DEFINED _pom2_include_owner_${_include_key})
                if(DEFINED _pom2_known_${_include_key})
                    message(FATAL_ERROR
                        "POM2 architecture: '${_rel}' (${_owner}) includes "
                        "unclassified first-party header '${_include}'. Add it "
                        "to a layer header manifest.")
                endif()
                # Generated or external header (ImGui, stb, Version.h, ...).
                continue()
            endif()

            set(_target_owner "${_pom2_include_owner_${_include_key}}")
            set(_target_rank "${_pom2_include_rank_${_include_key}}")
            if(_target_rank GREATER _owner_rank)
                message(FATAL_ERROR
                    "POM2 architecture violation: '${_rel}' (${_owner}) "
                    "includes '${_include}' (${_target_owner}). Dependencies "
                    "may only point toward lower layers: foundation <- media "
                    "<- machine <- devices <- runtime <- frontend.")
            endif()
        endforeach()

        # First-party ownership catches our own runtime wrappers. Also reject
        # direct escapes around those wrappers: deterministic machine/device
        # code may not acquire worker-thread or host-network APIs by including
        # a system header directly.
        if(_owner_rank LESS_EQUAL _pom2_rank_DEVICES)
            file(STRINGS "${_abs}" _system_include_lines
                 REGEX "^[ \t]*#[ \t]*include[ \t]*<[^>]+>")
            foreach(_line IN LISTS _system_include_lines)
                string(REGEX REPLACE "^[^<]*<([^>]+)>.*$" "\\1"
                       _system_include "${_line}")
                if(_system_include MATCHES
                   "^(thread|future|condition_variable|poll\\.h|sys/socket\\.h|arpa/inet\\.h|netinet/.*|winsock2\\.h|ws2tcpip\\.h)$")
                    message(FATAL_ERROR
                        "POM2 architecture host-API violation: '${_rel}' "
                        "(${_owner}) includes <${_system_include}>. Machine "
                        "and device layers must inject runtime transports.")
                endif()
            endforeach()
        endif()
    endforeach()
endfunction()

# Keep the frontend composition root reviewable. This is intentionally a
# family-wide limit: once a physical panel TU approaches the old god-object
# size, it must be split by responsibility instead of growing a new monolith.
function(pom2_enforce_mainwindow_line_limit root_dir max_lines)
    file(GLOB _pom2_mainwindow_tus "${root_dir}/src/MainWindow*.cpp")
    if(NOT _pom2_mainwindow_tus)
        message(FATAL_ERROR "POM2 architecture: no MainWindow translation units found")
    endif()
    foreach(_source IN LISTS _pom2_mainwindow_tus)
        file(READ "${_source}" _content)
        string(LENGTH "${_content}" _bytes_with_newlines)
        string(REPLACE "\n" "" _without_newlines "${_content}")
        string(LENGTH "${_without_newlines}" _bytes_without_newlines)
        math(EXPR _lines
             "${_bytes_with_newlines} - ${_bytes_without_newlines} + 1")
        if(_lines GREATER max_lines)
            get_filename_component(_name "${_source}" NAME)
            message(FATAL_ERROR
                "POM2 architecture: ${_name} has ${_lines} lines; "
                "MainWindow translation units are capped at ${max_lines}. "
                "Extract the growing panel/responsibility into its own .cpp.")
        endif()
    endforeach()
endfunction()
