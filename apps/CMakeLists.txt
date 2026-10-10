# Command-line front end. The subcommands live in a small library so the test suite can
# exercise them without spawning processes.
add_library(quill_cli STATIC
    cli/args.cpp
    cli/simulate.cpp
    cli/replay.cpp
    cli/fix_console.cpp
    cli/report.cpp)
target_include_directories(quill_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(quill_cli PUBLIC quill::quill)

add_executable(quill_app cli/main.cpp)
set_target_properties(quill_app PROPERTIES OUTPUT_NAME quill)
target_link_libraries(quill_app PRIVATE quill_cli)
