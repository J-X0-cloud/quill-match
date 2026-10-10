# Shared warning and code generation settings, applied through an interface target so
# that third-party code pulled in by FetchContent is not affected.

add_library(quill_options INTERFACE)
add_library(quill::options ALIAS quill_options)

target_compile_features(quill_options INTERFACE cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(quill_options INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Woverloaded-virtual
        -Wcast-align
        -Wimplicit-fallthrough
        -Wmissing-declarations)
    if(QUILL_WERROR)
        target_compile_options(quill_options INTERFACE -Werror)
    endif()
    if(QUILL_NATIVE)
        target_compile_options(quill_options INTERFACE -march=native)
    endif()
    if(QUILL_SANITIZE AND QUILL_TSAN)
        message(FATAL_ERROR "QUILL_SANITIZE and QUILL_TSAN cannot be combined")
    endif()
    if(QUILL_TSAN)
        target_compile_options(quill_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(quill_options INTERFACE -fsanitize=thread)
    endif()
    if(QUILL_SANITIZE)
        target_compile_options(quill_options INTERFACE
            -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
        target_link_options(quill_options INTERFACE -fsanitize=address,undefined)
    endif()
elseif(MSVC)
    target_compile_options(quill_options INTERFACE /W4 /permissive-)
    if(QUILL_WERROR)
        target_compile_options(quill_options INTERFACE /WX)
    endif()
endif()

find_package(Threads REQUIRED)
target_link_libraries(quill_options INTERFACE Threads::Threads)
