include(FetchContent)

FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    URL_HASH SHA256=f179ec217f9b3b3f3c6e8b02d3e7eda997b49e4ce26d6b235c9053bec9c0bf9f
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Stand-in for wsl.exe that simulates WSL against a scratch registry key.
add_executable(fake_wsl fake_wsl/main.cpp)
target_compile_definitions(fake_wsl PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(fake_wsl PRIVATE /W4 /utf-8 /permissive- /EHsc)
target_link_libraries(fake_wsl PRIVATE advapi32 ole32)

add_executable(wslmgr_tests
    TestMain.cpp
    TestSupport.cpp
    TestSupport.h
    CliTests.cpp
    ProcessTests.cpp
    UtilTests.cpp
    WslTests.cpp
)
target_link_libraries(wslmgr_tests PRIVATE wslcli GTest::gtest ole32 shell32)
# Tests set only the DistroSpec fields they care about; the rest keep their defaults.
# (A source property, so it lands after the /W4 that wslcore propagates.)
set_source_files_properties(WslTests.cpp CliTests.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:Clang>:-Wno-missing-designated-field-initializers>")
# The tests find fake_wsl.exe next to themselves.
add_dependencies(wslmgr_tests fake_wsl)
set_target_properties(fake_wsl wslmgr_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

include(GoogleTest)
gtest_discover_tests(wslmgr_tests DISCOVERY_MODE PRE_TEST)
