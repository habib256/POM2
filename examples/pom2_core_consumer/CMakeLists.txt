cmake_minimum_required(VERSION 3.16)
project(pom2_core_consumer LANGUAGES CXX)

find_package(pom2_core CONFIG REQUIRED)

add_executable(pom2_core_consumer main.cpp)
target_compile_features(pom2_core_consumer PRIVATE cxx_std_17)
target_link_libraries(pom2_core_consumer PRIVATE POM2::core)
