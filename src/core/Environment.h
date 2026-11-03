#pragma once

#include <string>

namespace wslmgr {

// Process-wide settings that locate WSL and its registry data. The application
// uses the defaults; the test suite points them at a fake wsl.exe and a scratch
// registry key so tests never touch real WSL instances.
struct Environment {
    std::wstring wslExe;  // empty: %SystemRoot%\System32\wsl.exe
    std::wstring lxssKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss";  // under HKCU
    std::wstring settingsKey = L"Software\\WslManager";                            // under HKCU
    unsigned long diskReleaseTimeoutMs = 120000;
    unsigned long diskReleasePollMs = 2000;
};

Environment& CurrentEnvironment();

// The wsl.exe that operations run: Environment::wslExe if set, else the system one.
std::wstring WslExePath();

}  // namespace wslmgr
