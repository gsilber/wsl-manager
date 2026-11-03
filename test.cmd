@echo off
rem Builds and runs the test suite.
rem Usage: test.cmd [--coverage]
rem   (no option)  MSVC Debug build in build\tests, run with ctest
rem   --coverage   clang-cl build in build\coverage with source-based coverage of
rem                src\core and src\cli; prints a summary and writes an HTML report
rem                to build\coverage\report\html\index.html. Needs the Visual Studio
rem                component "C++ Clang Compiler for Windows".
rem The tests use a fake wsl.exe and a scratch registry key; they never touch real
rem WSL instances.
setlocal

call "%~dp0scripts\vs-env.cmd" || exit /b 1

if /i "%~1"=="--coverage" goto coverage

set "BUILD_DIR=%~dp0build\tests"
cmake -S "%~dp0." -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWSLMGR_BUILD_TESTS=ON || exit /b 1
cmake --build "%BUILD_DIR%" || exit /b 1
ctest --test-dir "%BUILD_DIR%" --output-on-failure || exit /b 1
exit /b 0

:coverage
where clang-cl >nul 2>&1 || (
    echo clang-cl was not found. In the Visual Studio Installer, add the individual
    echo component "C++ Clang Compiler for Windows", then run this again.
    exit /b 1
)
set "BUILD_DIR=%~dp0build\coverage"
cmake -S "%~dp0." -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWSLMGR_BUILD_TESTS=ON ^
    -DWSLMGR_COVERAGE=ON -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_C_COMPILER=clang-cl || exit /b 1
cmake --build "%BUILD_DIR%" || exit /b 1

set "REPORT_DIR=%BUILD_DIR%\report"
if exist "%REPORT_DIR%" rmdir /s /q "%REPORT_DIR%"
mkdir "%REPORT_DIR%"
set "LLVM_PROFILE_FILE=%REPORT_DIR%\tests.profraw"
"%BUILD_DIR%\tests\wslmgr_tests.exe" --gtest_brief=1
set "TEST_RESULT=%ERRORLEVEL%"

set "TEST_EXE=%BUILD_DIR%\tests\wslmgr_tests.exe"
set "PROFILE=%REPORT_DIR%\tests.profdata"
set "IGNORE=-ignore-filename-regex=(tests|_deps|gui)[\\/]"
llvm-profdata merge -sparse "%REPORT_DIR%\tests.profraw" -o "%PROFILE%" || exit /b 1
llvm-cov show "%TEST_EXE%" -instr-profile="%PROFILE%" "%IGNORE%" -format=html -show-branches=count ^
    -output-dir="%REPORT_DIR%\html" || exit /b 1
echo.
llvm-cov report "%TEST_EXE%" -instr-profile="%PROFILE%" "%IGNORE%" -show-branch-summary || exit /b 1
echo.
echo Coverage report: %REPORT_DIR%\html\index.html
exit /b %TEST_RESULT%
