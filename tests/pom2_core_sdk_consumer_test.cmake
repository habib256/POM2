if(NOT DEFINED POM2_BINARY_DIR OR NOT DEFINED POM2_SOURCE_DIR)
    message(FATAL_ERROR "POM2_BINARY_DIR and POM2_SOURCE_DIR are required")
endif()

if(NOT POM2_TEST_CONFIG)
    set(POM2_TEST_CONFIG Release)
endif()

set(_root "${POM2_BINARY_DIR}/sdk-consumer-test")
set(_prefix "${_root}/prefix")
set(_build "${_root}/build")
file(REMOVE_RECURSE "${_root}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${POM2_BINARY_DIR}"
            --prefix "${_prefix}" --component pom2_core_sdk
            --config "${POM2_TEST_CONFIG}"
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_output
    ERROR_VARIABLE _install_error)
if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR
        "SDK install failed (${_install_result})\n${_install_output}\n${_install_error}")
endif()

set(_configure_command
    "${CMAKE_COMMAND}"
    -S "${POM2_SOURCE_DIR}/examples/pom2_core_consumer"
    -B "${_build}"
    "-DCMAKE_PREFIX_PATH=${_prefix}")
if(POM2_TEST_GENERATOR)
    list(APPEND _configure_command -G "${POM2_TEST_GENERATOR}")
endif()
if(POM2_TEST_GENERATOR_PLATFORM)
    list(APPEND _configure_command -A "${POM2_TEST_GENERATOR_PLATFORM}")
endif()
if(POM2_TEST_GENERATOR_TOOLSET)
    list(APPEND _configure_command -T "${POM2_TEST_GENERATOR_TOOLSET}")
endif()
if(NOT POM2_TEST_GENERATOR MATCHES "Visual Studio|Xcode|Multi-Config")
    list(APPEND _configure_command "-DCMAKE_BUILD_TYPE=${POM2_TEST_CONFIG}")
endif()

execute_process(
    COMMAND ${_configure_command}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
        "SDK consumer configure failed (${_configure_result})\n"
        "${_configure_output}\n${_configure_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_build}"
            --config "${POM2_TEST_CONFIG}"
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_error)
if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR
        "SDK consumer build failed (${_build_result})\n${_build_output}\n${_build_error}")
endif()

set(_executable "${_build}/pom2_core_consumer${POM2_TEST_EXECUTABLE_SUFFIX}")
if(NOT EXISTS "${_executable}")
    set(_executable
        "${_build}/${POM2_TEST_CONFIG}/pom2_core_consumer${POM2_TEST_EXECUTABLE_SUFFIX}")
endif()
if(NOT EXISTS "${_executable}")
    message(FATAL_ERROR "SDK consumer executable was not produced")
endif()

execute_process(
    COMMAND "${_executable}"
    RESULT_VARIABLE _run_result
    OUTPUT_VARIABLE _run_output
    ERROR_VARIABLE _run_error)
if(NOT _run_result EQUAL 0)
    message(FATAL_ERROR
        "SDK consumer run failed (${_run_result})\n${_run_output}\n${_run_error}")
endif()
message(STATUS "SDK consumer: ${_run_output}")
