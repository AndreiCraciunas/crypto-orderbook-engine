# CMakeLists.txt for crypto-orderbook-engine tests

cmake_minimum_required(VERSION 3.15)

# Find Google Test
find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIRS})

# Include project headers
include_directories(${PROJECT_SOURCE_DIR}/include)

# Common libraries for all tests
set(COMMON_LIBS
    ${GTEST_BOTH_LIBRARIES}
    pthread
    spdlog::spdlog
)

# Platform-specific libraries
if(UNIX AND NOT APPLE)
    list(APPEND COMMON_LIBS numa yaml-cpp)
endif()

# ============================================================================
# Core Component Tests
# ============================================================================

add_executable(test_order_book test_order_book.cpp)
target_link_libraries(test_order_book ${COMMON_LIBS})
add_test(NAME OrderBookTests COMMAND test_order_book)

add_executable(test_aggregated_order_book test_aggregated_order_book.cpp)
target_link_libraries(test_aggregated_order_book ${COMMON_LIBS})
add_test(NAME AggregatedOrderBookTests COMMAND test_aggregated_order_book)

# ============================================================================
# Analytics Tests
# ============================================================================

add_executable(test_analytics test_analytics.cpp)
target_link_libraries(test_analytics ${COMMON_LIBS})
add_test(NAME AnalyticsTests COMMAND test_analytics)

# ============================================================================
# Configuration and Utilities Tests
# ============================================================================

add_executable(test_configuration test_configuration.cpp)
target_link_libraries(test_configuration ${COMMON_LIBS})
add_test(NAME ConfigurationTests COMMAND test_configuration)

# ============================================================================
# Lock-free Data Structures Tests
# ============================================================================

add_executable(test_lock_free_queue test_lock_free_queue.cpp)
target_link_libraries(test_lock_free_queue ${COMMON_LIBS})
add_test(NAME LockFreeQueueTests COMMAND test_lock_free_queue)

add_executable(test_ring_buffer test_ring_buffer.cpp)
target_link_libraries(test_ring_buffer ${COMMON_LIBS})
add_test(NAME RingBufferTests COMMAND test_ring_buffer)

add_executable(test_memory_pool test_memory_pool.cpp)
target_link_libraries(test_memory_pool ${COMMON_LIBS})
add_test(NAME MemoryPoolTests COMMAND test_memory_pool)

# ============================================================================
# Arbitrage Monitor Tests
# ============================================================================

add_executable(test_arbitrage_monitor test_arbitrage_monitor.cpp)
target_link_libraries(test_arbitrage_monitor ${COMMON_LIBS})
add_test(NAME ArbitrageMonitorTests COMMAND test_arbitrage_monitor)

# ============================================================================
# Additional Component Tests
# ============================================================================

add_executable(test_rate_limiter test_rate_limiter.cpp)
target_link_libraries(test_rate_limiter ${COMMON_LIBS})
add_test(NAME RateLimiterTests COMMAND test_rate_limiter)

add_executable(test_order_book_manager test_order_book_manager.cpp)
target_link_libraries(test_order_book_manager ${COMMON_LIBS})
add_test(NAME OrderBookManagerTests COMMAND test_order_book_manager)

add_executable(test_seq_lock test_seq_lock.cpp)
target_link_libraries(test_seq_lock ${COMMON_LIBS})
add_test(NAME SeqLockTests COMMAND test_seq_lock)

add_executable(test_market_data_recorder test_market_data_recorder.cpp)
target_link_libraries(test_market_data_recorder ${COMMON_LIBS})
add_test(NAME MarketDataRecorderTests COMMAND test_market_data_recorder)

add_executable(test_time_utils test_time_utils.cpp)
target_link_libraries(test_time_utils ${COMMON_LIBS})
add_test(NAME TimeUtilsTests COMMAND test_time_utils)

add_executable(test_metrics_collector test_metrics_collector.cpp)
target_link_libraries(test_metrics_collector ${COMMON_LIBS})
add_test(NAME MetricsCollectorTests COMMAND test_metrics_collector)

# ============================================================================
# Test Target
# ============================================================================

# Enable testing
enable_testing()

# Add custom target to run all tests
add_custom_target(run_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS
        test_order_book
        test_aggregated_order_book
        test_analytics
        test_configuration
        test_lock_free_queue
        test_ring_buffer
        test_memory_pool
        test_arbitrage_monitor
        test_rate_limiter
        test_order_book_manager
        test_seq_lock
        test_market_data_recorder
        test_time_utils
        test_metrics_collector
    COMMENT "Running all unit tests"
)

# Verbose test output
add_custom_target(run_tests_verbose
    COMMAND ${CMAKE_CTEST_COMMAND} --verbose
    DEPENDS
        test_order_book
        test_aggregated_order_book
        test_analytics
        test_configuration
        test_lock_free_queue
        test_ring_buffer
        test_memory_pool
        test_arbitrage_monitor
        test_rate_limiter
        test_order_book_manager
        test_seq_lock
        test_market_data_recorder
        test_time_utils
        test_metrics_collector
    COMMENT "Running all unit tests (verbose)"
)
