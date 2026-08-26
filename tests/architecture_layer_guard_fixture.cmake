# Child process used by architecture_layer_guard_test.cmake. This fixture is
# intentionally invalid: a media source includes a runtime header.

if(NOT DEFINED POM2_LAYER_FIXTURE_ROOT OR NOT DEFINED POM2_ARCH_MODULE)
    message(FATAL_ERROR "architecture fixture arguments are required")
endif()

if(POM2_LAYER_FIXTURE_KIND STREQUAL "host_api")
    set(POM2_DEVICES_SOURCES src/Low.cpp)
else()
    set(POM2_MEDIA_SOURCES src/Low.cpp)
    set(POM2_RUNTIME_HEADERS src/High.h)
endif()

include("${POM2_ARCH_MODULE}")
pom2_enforce_source_layers("${POM2_LAYER_FIXTURE_ROOT}")
