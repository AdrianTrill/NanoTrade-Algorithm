cmake_minimum_required(VERSION 3.16)

project(NanoTradeAlgorithm VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(NANOTRADE_BUILD_TESTS "Build NanoTrade test target" ON)

add_library(nanotrade
    src/backtest_engine.cpp
    src/downloader.cpp
    src/indicators.cpp
    src/market_data.cpp
    src/reporting.cpp
    src/strategies.cpp
)

target_include_directories(nanotrade PUBLIC include)

if(MSVC)
    target_compile_options(nanotrade PRIVATE /W4 /permissive-)
else()
    target_compile_options(nanotrade PRIVATE -Wall -Wextra -Wpedantic)
endif()

find_package(CURL QUIET)
if(CURL_FOUND)
    target_link_libraries(nanotrade PRIVATE CURL::libcurl)
    target_compile_definitions(nanotrade PRIVATE NANOTRADE_HAVE_CURL=1)
endif()

add_executable(nanotrade_cli app/main.cpp)
target_link_libraries(nanotrade_cli PRIVATE nanotrade)
set_target_properties(nanotrade_cli PROPERTIES OUTPUT_NAME nanotrade)

file(COPY data DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

if(NANOTRADE_BUILD_TESTS)
    enable_testing()

    add_executable(nanotrade_tests tests/test_main.cpp)
    target_link_libraries(nanotrade_tests PRIVATE nanotrade)

    if(MSVC)
        target_compile_options(nanotrade_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(nanotrade_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_test(NAME nanotrade_tests COMMAND nanotrade_tests)
endif()
