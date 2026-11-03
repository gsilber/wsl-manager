cmake_minimum_required(VERSION 3.21)
project(WslManager VERSION 1.0.0 LANGUAGES CXX RC)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "WSL Manager is a Windows application and must be built with MSVC.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static runtime so the executables run without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(wslcore STATIC
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

add_executable(wslmgr src/cli/main.cpp)
target_link_libraries(wslmgr PRIVATE wslcore)

add_executable(wslmgr-gui WIN32
    src/gui/Dialogs.cpp
    src/gui/Dialogs.h
    src/gui/MainWindow.cpp
    src/gui/resource.h
    src/gui/app.rc
)
target_link_libraries(wslmgr-gui PRIVATE wslcore comctl32 shell32 ole32 uxtheme)
# app.rc embeds app.manifest (common controls v6, per-monitor DPI awareness).
target_link_options(wslmgr-gui PRIVATE /MANIFEST:NO)
set_property(SOURCE src/gui/app.rc APPEND PROPERTY OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/app.manifest)
