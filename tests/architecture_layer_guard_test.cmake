# Pin the configure-time architecture guard with a deliberately inverted
# dependency. The outer script succeeds only when the child CMake process
# rejects it for the expected reason.

if(NOT DEFINED POM2_LAYER_TEST_DIR OR NOT DEFINED POM2_SOURCE_DIR)
    message(FATAL_ERROR "architecture test arguments are required")
endif()

file(REMOVE_RECURSE "${POM2_LAYER_TEST_DIR}")
file(MAKE_DIRECTORY "${POM2_LAYER_TEST_DIR}/src")
file(WRITE "${POM2_LAYER_TEST_DIR}/src/Low.cpp" "#include \"High.h\"\n")
file(WRITE "${POM2_LAYER_TEST_DIR}/src/High.h" "#pragma once\n")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DPOM2_LAYER_FIXTURE_ROOT=${POM2_LAYER_TEST_DIR}"
        "-DPOM2_ARCH_MODULE=${POM2_SOURCE_DIR}/cmake/Pom2Architecture.cmake"
        -P "${POM2_SOURCE_DIR}/tests/architecture_layer_guard_fixture.cmake"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)

set(combined "${output}\n${error}")
if(result EQUAL 0)
    message(FATAL_ERROR "architecture guard accepted an upward dependency")
endif()
if(NOT combined MATCHES "POM2 architecture violation")
    message(FATAL_ERROR
        "architecture guard failed for an unexpected reason:\n${combined}")
endif()

# A lower layer must not bypass first-party runtime wrappers by reaching for a
# worker-thread or host-network system header directly.
file(WRITE "${POM2_LAYER_TEST_DIR}/src/Low.cpp" "#include <thread>\n")
file(REMOVE "${POM2_LAYER_TEST_DIR}/src/High.h")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DPOM2_LAYER_FIXTURE_ROOT=${POM2_LAYER_TEST_DIR}"
        "-DPOM2_ARCH_MODULE=${POM2_SOURCE_DIR}/cmake/Pom2Architecture.cmake"
        "-DPOM2_LAYER_FIXTURE_KIND=host_api"
        -P "${POM2_SOURCE_DIR}/tests/architecture_layer_guard_fixture.cmake"
    RESULT_VARIABLE host_result
    OUTPUT_VARIABLE host_output
    ERROR_VARIABLE host_error)

set(host_combined "${host_output}\n${host_error}")
if(host_result EQUAL 0)
    message(FATAL_ERROR "architecture guard accepted a device host API")
endif()
if(NOT host_combined MATCHES "host-API violation")
    message(FATAL_ERROR
        "host API guard failed for an unexpected reason:\n${host_combined}")
endif()
