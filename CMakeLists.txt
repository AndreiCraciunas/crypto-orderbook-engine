cmake_minimum_required(VERSION 3.20)
project(MarketDataHandler VERSION 1.0.0 LANGUAGES CXX)

# C++ Standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native -mtune=native")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")

# Link-time optimization
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT error)
if(ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

# Find packages
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# Try to find Boost - make it optional since websocketpp can use standalone ASIO
find_package(Boost 1.75 COMPONENTS system)
if(NOT Boost_FOUND)
    message(STATUS "Boost not found with components, trying header-only...")
    find_package(Boost 1.75 REQUIRED)
endif()

# Include FetchContent for dependencies
include(FetchContent)

# Fetch simdjson
FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG v3.10.1
)
FetchContent_MakeAvailable(simdjson)

# Fetch websocketpp (header-only, skip its CMakeLists.txt)
FetchContent_Declare(
    websocketpp
    GIT_REPOSITORY https://github.com/zaphoyd/websocketpp.git
    GIT_TAG 0.8.2
)
FetchContent_GetProperties(websocketpp)
if(NOT websocketpp_POPULATED)
    FetchContent_Populate(websocketpp)
    # Don't add subdirectory to avoid websocketpp's CMakeLists.txt
endif()

# Fetch spdlog
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1
)
FetchContent_MakeAvailable(spdlog)

# Fetch yaml-cpp for config
FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG 0.8.0
)
FetchContent_MakeAvailable(yaml-cpp)

# Fetch Google Test
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Fetch Google Benchmark
FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.0
)
set(BENCHMARK_ENABLE_TESTING OFF)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF)
set(HAVE_STD_REGEX OFF)
set(HAVE_POSIX_REGEX OFF)
set(HAVE_GNU_POSIX_REGEX OFF)
FetchContent_MakeAvailable(benchmark)

# Main library
add_library(market_data_core
    src/core/order_book.cpp
    src/core/memory_pool.cpp
    src/core/order_book_manager.cpp
    src/utils/lock_free_queue.cpp
    src/utils/time_utils.cpp
    src/metrics/metrics_collector.cpp
    src/network/frontend_websocket_server.cpp
    src/network/frontend_rest_server.cpp
)

target_include_directories(market_data_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}
    ${websocketpp_SOURCE_DIR}
)

target_link_libraries(market_data_core PUBLIC
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    simdjson::simdjson
    spdlog::spdlog
    yaml-cpp::yaml-cpp
)

# Add Windows socket libraries for REST server and WebSocket (Boost.Beast)
if(WIN32)
    target_link_libraries(market_data_core PUBLIC ws2_32 mswsock)
endif()

# Link Boost if available with components, otherwise just headers
if(TARGET Boost::system)
    target_link_libraries(market_data_core PUBLIC Boost::system)
endif()
if(TARGET Boost::thread)
    target_link_libraries(market_data_core PUBLIC Boost::thread)
endif()
# Always add Boost include directories
target_include_directories(market_data_core PUBLIC ${Boost_INCLUDE_DIRS})

# Main executable (demo)
add_executable(market_data_handler
    src/main.cpp
)

target_link_libraries(market_data_handler PRIVATE
    market_data_core
)

# Server executable (frontend integration)
add_executable(market_data_server
    src/server_main.cpp
)

target_link_libraries(market_data_server PRIVATE
    market_data_core
)

# Tests
enable_testing()

add_executable(unit_tests
    tests/unit/test_order_book.cpp
    tests/unit/test_lock_free_queue.cpp
    tests/unit/test_memory_pool.cpp
)

target_link_libraries(unit_tests PRIVATE
    market_data_core
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME unit_tests COMMAND unit_tests)

# Benchmarks
add_executable(benchmarks
    tests/benchmarks/bench_order_book.cpp
    tests/benchmarks/bench_serialization.cpp
)

target_link_libraries(benchmarks PRIVATE
    market_data_core
    benchmark::benchmark
    benchmark::benchmark_main
)

# Installation
install(TARGETS market_data_handler
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/
    DESTINATION include
)

# Documentation (optional)
option(BUILD_DOCUMENTATION "Build API documentation with Doxygen" OFF)

if(BUILD_DOCUMENTATION)
    find_package(Doxygen)
    if(DOXYGEN_FOUND)
        # Set Doxygen input and output files
        set(DOXYGEN_IN ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile)
        set(DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)

        # Copy Doxyfile to build directory
        configure_file(${DOXYGEN_IN} ${DOXYGEN_OUT} @ONLY)

        # Add documentation target
        add_custom_target(docs
            COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Generating API documentation with Doxygen"
            VERBATIM
        )

        message(STATUS "Doxygen found. Run 'make docs' to generate documentation.")
    else()
        message(WARNING "Doxygen not found. Documentation will not be built.")
    endif()
endif()
