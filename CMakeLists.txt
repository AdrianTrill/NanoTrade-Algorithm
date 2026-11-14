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
    src/api.cpp
    src/engine.cpp
    src/expression.cpp
    src/indicators.cpp
    src/json.cpp
    src/math.cpp
    src/metrics.cpp
    src/research.cpp
    src/series.cpp
    src/strategy.cpp
    src/synthetic.cpp
    src/text.cpp
    src/time.cpp
)
target_include_directories(nanotrade_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(nanotrade_core PUBLIC Threads::Threads PRIVATE nanotrade_options)

add_library(nanotrade_http src/http.cpp)
target_include_directories(nanotrade_http PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(nanotrade_http PUBLIC Threads::Threads PRIVATE nanotrade_options)
if(WIN32)
    target_link_libraries(nanotrade_http PUBLIC ws2_32)
endif()

# Where `nanotrade serve` looks for the lab's files.
set(NANOTRADE_SITE_DIR ${PROJECT_BINARY_DIR}/site)

add_executable(nanotrade tools/nanotrade_main.cpp)
target_link_libraries(nanotrade PRIVATE nanotrade_core nanotrade_http nanotrade_options)
target_compile_definitions(nanotrade PRIVATE
    NANOTRADE_DATA_DIR="${PROJECT_SOURCE_DIR}/data"
    NANOTRADE_SITE_DIR="${NANOTRADE_SITE_DIR}")

include(CTest)

if(BUILD_TESTING)
    file(GLOB NANOTRADE_TESTS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/*_tests.cpp)
    foreach(test_source IN LISTS NANOTRADE_TESTS)
        get_filename_component(test_name ${test_source} NAME_WE)
        add_executable(nanotrade_${test_name} ${test_source})
        target_include_directories(nanotrade_${test_name} PRIVATE ${PROJECT_SOURCE_DIR}/tests)
        target_link_libraries(nanotrade_${test_name} PRIVATE nanotrade_core nanotrade_http nanotrade_options)
        target_compile_definitions(nanotrade_${test_name} PRIVATE NANOTRADE_DATA_DIR="${PROJECT_SOURCE_DIR}/data")
        add_test(NAME ${test_name} COMMAND nanotrade_${test_name})
    endforeach()

    # Every golden request must produce its golden response byte for byte, on every platform.
    add_test(NAME golden_responses COMMAND nanotrade api --check ${PROJECT_SOURCE_DIR}/tests/golden)

    # The command line, end to end.
    add_test(NAME cli_help COMMAND nanotrade --help)
    add_test(NAME cli_run COMMAND nanotrade run --data AAPL --from 2004-01-01 --strategy donchian --allow-short --stop 0.1)
    add_test(NAME cli_sweep COMMAND nanotrade sweep --data GE --strategy sma_cross --x fast=5:50:5 --y slow=60:200:20)
    add_test(NAME cli_walkforward
             COMMAND nanotrade walkforward --data CAT --strategy rsi_reversion --x period=5:25:5 --is-years 5 --oos-years 2)
    add_test(NAME cli_montecarlo COMMAND nanotrade montecarlo --data BA --strategy momentum --paths 200)
    add_test(NAME cli_custom COMMAND nanotrade run --data F --long "rsi(close, 2) < 10 and close > sma(close, 200)"
                                                     --long-exit "close > sma(close, 5)")
    add_test(NAME cli_rejects_bad_rule COMMAND nanotrade run --long "rsi(close, 14) < oversold")
    set_tests_properties(cli_rejects_bad_rule PROPERTIES WILL_FAIL TRUE)
    add_test(NAME cli_synth COMMAND nanotrade synth --seed 3 --bars 300)
endif()
