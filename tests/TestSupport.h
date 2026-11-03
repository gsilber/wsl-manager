#pragma once

// Shared fixtures and helpers for the WSL Manager tests.
//
// Every test that touches WSL derives from WslTest, which points the core
// library (via wslmgr::CurrentEnvironment) at fake_wsl.exe and at a scratch
// HKCU key and temp folder that are deleted afterwards. Real WSL instances and
// the real WslManager settings are never read or written.

#include "core/Environment.h"
#include "core/Wsl.h"

#include <gtest/gtest.h>
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace test {

std::filesystem::path FakeWslPath();

// Points the core library at fake_wsl.exe and a key that holds no instances.
// Installed for the whole test run so no test can reach the real wsl.exe.
void InstallSafeEnvironment();

// Collects what an operation reports through OutputFn.
struct Captured {
    std::vector<std::wstring> lines;
    std::vector<std::wstring> progress;

    wslmgr::OutputFn Fn();
    bool Has(std::wstring_view text) const;  // some line contains `text`
    std::wstring All() const;
};

struct DistroSpec {
    std::wstring name;
    int version = 2;
    std::string diskContent = "disk";  // written to ext4.vhdx for WSL 2
    bool createDisk = true;
    bool running = false;
    uint32_t uid = 0;
    std::wstring flavor = L"ubuntu";
    std::wstring osVersion = L"24.04";
    std::wstring basePath;  // default: <temp>\instances\<name>; "-" for none
    bool longPathPrefix = true;
    bool includeFlags = true;
    uint32_t state = 1;
};

struct CliResult {
    int exitCode = 0;
    std::wstring out;
    std::wstring err;
};

class WslTest : public ::testing::Test {
protected:
    void SetUp() override;
    void TearDown() override;

    const std::filesystem::path& Temp() const { return temp_; }
    std::filesystem::path Path(const std::wstring& relative) const { return temp_ / relative; }
    const std::wstring& LxssKey() const { return lxssKey_; }
    std::wstring DistroKey(const std::wstring& id) const { return lxssKey_ + L"\\" + id; }

    // Registers a fake instance directly in the scratch registry; returns its id.
    std::wstring AddDistro(const DistroSpec& spec);
    void MakeDefault(const std::wstring& id);
    std::wstring RegString(const std::wstring& key, const wchar_t* value) const;
    DWORD RegDword(const std::wstring& key, const wchar_t* value) const;

    // Sets a FAKE_WSL_* variable for the rest of the test.
    void SetFake(const wchar_t* name, const std::wstring& value);

    // Arguments of every fake wsl.exe call so far, one tab-separated line each.
    std::vector<std::wstring> WslCalls() const;
    bool Called(std::wstring_view argsSubstring) const;

    // Runs the CLI in-process with stdout/stderr captured to files and stdin fed from
    // `input`; pass nullptr to run with no standard input handle at all.
    CliResult RunCli(std::vector<std::wstring> args, const char* input = "");

    static void WriteFile(const std::filesystem::path& path, const std::string& content);
    static std::string ReadFile(const std::filesystem::path& path);

private:
    std::wstring root_;
    std::wstring lxssKey_;
    std::filesystem::path temp_;
};

// Redirects stdout and stderr to a hidden console screen buffer (so the console
// code paths run) and can type keys into the console input for prompts.
class ConsoleScope {
public:
    ConsoleScope();
    ~ConsoleScope();
    ConsoleScope(const ConsoleScope&) = delete;
    ConsoleScope& operator=(const ConsoleScope&) = delete;

    std::wstring Text() const;            // visible text, rows joined with '\n'
    void Type(const std::wstring& keys);  // queues keystrokes; '\r' is Enter

private:
    bool allocated_ = false;
    HANDLE buffer_ = INVALID_HANDLE_VALUE;
    HANDLE input_ = INVALID_HANDLE_VALUE;
    HANDLE oldOut_ = nullptr;
    HANDLE oldErr_ = nullptr;
    HANDLE oldIn_ = nullptr;
};

}  // namespace test
