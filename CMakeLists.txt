cmake_minimum_required(VERSION 3.21)

# Floating-point std::to_chars, which writes every number in a response, needs macOS 13.3 or later.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS version")
endif()

project(NanoTrade VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(NANOTRADE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(NANOTRADE_SANITIZERS "" CACHE STRING "Sanitizers for every target, such as address,undefined")

# Settings every target shares.
add_library(nanotrade_options INTERFACE)
target_compile_options(nanotrade_options INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /utf-8 /EHsc>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Wshadow>)
# A fused multiply-add rounds differently from a multiply and an add, and the golden responses must come out the same
# on every platform and in WebAssembly, so contraction is off. MSVC doesn't contract under its default /fp:precise.
target_compile_options(nanotrade_options INTERFACE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>)
if(NANOTRADE_WARNINGS_AS_ERRORS)
    target_compile_options(nanotrade_options INTERFACE $<IF:$<CXX_COMPILER_ID:MSVC>,/WX,-Werror>)
endif()
if(NANOTRADE_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR "NANOTRADE_SANITIZERS needs GCC or Clang")
    endif()
    target_compile_options(nanotrade_options INTERFACE
        -fsanitize=${NANOTRADE_SANITIZERS} -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
    target_link_options(nanotrade_options INTERFACE -fsanitize=${NANOTRADE_SANITIZERS})
endif()

find_package(Threads REQUIRED)

add_library(nanotrade_core
    src/engine.cpp
    src/expression.cpp
    src/indicators.cpp
    src/json.cpp
    src/math.cpp
    src/metrics.cpp
    src/series.cpp
    src/strategy.cpp
    src/synthetic.cpp
    src/text.cpp
    src/time.cpp
)
target_include_directories(nanotrade_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(nanotrade_core PUBLIC Threads::Threads PRIVATE nanotrade_options)

include(CTest)

if(BUILD_TESTING)
    file(GLOB NANOTRADE_TESTS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/*_tests.cpp)
    foreach(test_source IN LISTS NANOTRADE_TESTS)
        get_filename_component(test_name ${test_source} NAME_WE)
        add_executable(nanotrade_${test_name} ${test_source})
        target_include_directories(nanotrade_${test_name} PRIVATE ${PROJECT_SOURCE_DIR}/tests)
        target_link_libraries(nanotrade_${test_name} PRIVATE nanotrade_core nanotrade_options)
        target_compile_definitions(nanotrade_${test_name} PRIVATE NANOTRADE_DATA_DIR="${PROJECT_SOURCE_DIR}/data")
        add_test(NAME ${test_name} COMMAND nanotrade_${test_name})
    endforeach()

endif()
