add_library(quill
    order_book.cpp
    matching_engine.cpp
    latency_histogram.cpp
    engine_runner.cpp)
add_library(quill::quill ALIAS quill)

target_include_directories(quill PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_link_libraries(quill PUBLIC quill::options)
