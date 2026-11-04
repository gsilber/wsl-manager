cmake_minimum_required(VERSION 3.21)
project(WslManager VERSION 1.0.0 LANGUAGES CXX RC)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "WSL Manager is a Windows application and must be built with MSVC.")
endif()

# package.cmd reads this to version the MSI.
file(WRITE ${CMAKE_BINARY_DIR}/version.txt "${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static runtime so the executables run without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(WSLMGR_BUILD_TESTS "Build the test suite (downloads GoogleTest)" OFF)
option(WSLMGR_COVERAGE "Instrument src/core and src/cli for llvm-cov (requires clang-cl)" OFF)

if(WSLMGR_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "WSLMGR_COVERAGE requires clang-cl (-DCMAKE_CXX_COMPILER=clang-cl).")
    endif()
    # The linker is invoked directly (not through clang-cl), so add Clang's
    # profiling runtime explicitly.
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} /clang:-print-runtime-dir
                    OUTPUT_VARIABLE CLANG_RUNTIME_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
    # Look only in this clang's own runtime folder, and look again if a toolchain
    # update has removed the library found last time.
    if(CLANG_PROFILE_RUNTIME AND NOT EXISTS "${CLANG_PROFILE_RUNTIME}")
        unset(CLANG_PROFILE_RUNTIME CACHE)
    endif()
    find_library(CLANG_PROFILE_RUNTIME NAMES clang_rt.profile-x86_64 clang_rt.profile
                 PATHS ${CLANG_RUNTIME_DIR} NO_DEFAULT_PATH REQUIRED)
    set(WSLMGR_COVERAGE_FLAGS -fprofile-instr-generate -fcoverage-mapping)
endif()

add_library(wslcore STATIC
    src/core/Environment.h
    src/core/Process.cpp
    src/core/Process.h
    src/core/Util.cpp
    src/core/Util.h
    src/core/Wsl.cpp
    src/core/Wsl.h
)
# Expose headers as "core/...": putting src/core itself on the include path would
# let core/Process.h shadow the CRT's <process.h> (Windows paths are case-insensitive).
target_include_directories(wslcore PUBLIC src)
target_compile_definitions(wslcore PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(wslcore PUBLIC /W4 /utf-8 /permissive- /EHsc)
target_link_libraries(wslcore PUBLIC advapi32)
if(WSLMGR_COVERAGE)
    target_compile_options(wslcore PRIVATE ${WSLMGR_COVERAGE_FLAGS})
    target_link_libraries(wslcore PUBLIC ${CLANG_PROFILE_RUNTIME})
endif()

# The command line lives in a library so the tests can run it in-process.
add_library(wslcli STATIC src/cli/Cli.cpp src/cli/Cli.h)
target_link_libraries(wslcli PUBLIC wslcore)
if(WSLMGR_COVERAGE)
    target_compile_options(wslcli PRIVATE ${WSLMGR_COVERAGE_FLAGS})
endif()

add_executable(wslmgr src/cli/main.cpp)
target_link_libraries(wslmgr PRIVATE wslcli)

add_executable(wslmgr-gui WIN32
    src/gui/Dialogs.cpp
    src/gui/Dialogs.h
    src/gui/MainWindow.cpp
    src/gui/Theme.cpp
    src/gui/Theme.h
    src/gui/resource.h
    src/gui/app.rc
)
target_link_libraries(wslmgr-gui PRIVATE wslcore comctl32 shell32 ole32 uxtheme dwmapi gdiplus)
# app.rc embeds app.manifest (common controls v6, per-monitor DPI awareness) and app.ico.
target_link_options(wslmgr-gui PRIVATE /MANIFEST:NO)
set_property(SOURCE src/gui/app.rc APPEND PROPERTY OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/app.manifest
             ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/app.ico)

if(WSLMGR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
