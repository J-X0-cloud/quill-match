include(FetchContent)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(googletest)

add_executable(quill_tests
    flat_hash_map_test.cpp
    object_pool_test.cpp
    price_ladder_test.cpp
    order_book_test.cpp
    order_types_test.cpp
    replace_test.cpp
    matching_engine_test.cpp
    book_model_test.cpp
    spsc_queue_test.cpp
    latency_histogram_test.cpp
    engine_runner_test.cpp
    itch_codec_test.cpp
    itch_feed_test.cpp
    order_flow_test.cpp
    fix_message_test.cpp
    fix_gateway_test.cpp)

target_include_directories(quill_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(quill_tests PRIVATE quill::quill GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(quill_tests DISCOVERY_TIMEOUT 60)
