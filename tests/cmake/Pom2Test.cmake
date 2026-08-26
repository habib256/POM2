# Reusable constructor for deterministic, renderer-free test executables.
# Keep platform libraries on pom2_core_test's PUBLIC link interface; individual
# tests describe only their own sources and exceptional dependencies.

include_guard(GLOBAL)

function(pom2_add_headless_test target)
    set(options PORTABLE)
    set(oneValueArgs NAME TIMEOUT WORKING_DIRECTORY)
    set(multiValueArgs
        SOURCES INCLUDE_DIRECTORIES LINK_LIBRARIES COMPILE_DEFINITIONS)
    cmake_parse_arguments(POM2_TEST
        "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT POM2_TEST_SOURCES)
        message(FATAL_ERROR "${target}: pom2_add_headless_test needs SOURCES")
    endif()
    if(NOT POM2_TEST_NAME)
        set(POM2_TEST_NAME "${target}")
    endif()
    if(NOT POM2_TEST_TIMEOUT)
        set(POM2_TEST_TIMEOUT 10)
    endif()

    add_executable(${target} ${POM2_TEST_SOURCES})
    target_include_directories(${target} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${POM2_TEST_INCLUDE_DIRECTORIES})
    if(POM2_TEST_LINK_LIBRARIES)
        target_link_libraries(${target} PRIVATE ${POM2_TEST_LINK_LIBRARIES})
    endif()
    if(POM2_TEST_COMPILE_DEFINITIONS)
        target_compile_definitions(${target} PRIVATE
            ${POM2_TEST_COMPILE_DEFINITIONS})
    endif()

    if(POM2_TEST_WORKING_DIRECTORY)
        add_test(NAME ${POM2_TEST_NAME} COMMAND ${target}
                 WORKING_DIRECTORY "${POM2_TEST_WORKING_DIRECTORY}")
    else()
        add_test(NAME ${POM2_TEST_NAME} COMMAND ${target})
    endif()
    set_tests_properties(${POM2_TEST_NAME} PROPERTIES
        TIMEOUT ${POM2_TEST_TIMEOUT})
    if(POM2_TEST_PORTABLE)
        set_property(TEST ${POM2_TEST_NAME} APPEND PROPERTY
                     LABELS portable-headless)
    endif()
endfunction()
