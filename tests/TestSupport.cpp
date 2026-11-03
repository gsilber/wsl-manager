#include "TestSupport.h"

#include "cli/Cli.h"

#include <objbase.h>

#include <atomic>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace test {
namespace {

constexpr wchar_t kTestRootKey[] = L"Software\\WslManagerTests";

const wchar_t* const kFakeVariables[] = {
    L"FAKE_WSL_LXSS",     L"FAKE_WSL_LOG",         L"FAKE_WSL_FAIL",     L"FAKE_WSL_FAIL_MSG",
    L"FAKE_WSL_UTF16",    L"FAKE_WSL_PROGRESS",    L"FAKE_WSL_NO_REGISTER", L"FAKE_WSL_NO_FILES",
    L"FAKE_WSL_ONLINE",
};

void ClearFakeVariables() {
    for (const wchar_t* name : kFakeVariables) SetEnvironmentVariableW(name, nullptr);
}

std::wstring UniqueSuffix() {
    static std::atomic<int> counter{0};
    return std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++counter);
}

void SetRegString(const std::wstring& key, const wchar_t* value, const std::wstring& data) {
    RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), value, REG_SZ, data.c_str(),
                    static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
}

void SetRegDword(const std::wstring& key, const wchar_t* value, DWORD data) {
    RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), value, REG_DWORD, &data, sizeof(data));
}

std::wstring FromUtf8(const std::string& text) { return wslmgr::Utf8ToWide(text); }

}  // namespace

fs::path FakeWslPath() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    return fs::path(self).parent_path() / L"fake_wsl.exe";
}

void InstallSafeEnvironment() {
    std::wstring nowhere = std::wstring(kTestRootKey) + L"\\none";
    wslmgr::Environment& env = wslmgr::CurrentEnvironment();
    env = wslmgr::Environment{};
    env.wslExe = FakeWslPath().wstring();
    env.lxssKey = nowhere;
    env.settingsKey = nowhere;
    ClearFakeVariables();
    SetEnvironmentVariableW(L"FAKE_WSL_LXSS", nowhere.c_str());
}

// ---------------------------------------------------------------------------
// Captured

wslmgr::OutputFn Captured::Fn() {
    return [this](const std::wstring& line, bool isProgress) { (isProgress ? progress : lines).push_back(line); };
}

bool Captured::Has(std::wstring_view text) const {
    for (const auto& line : lines)
        if (line.find(text) != std::wstring::npos) return true;
    return false;
}

std::wstring Captured::All() const {
    std::wstring all;
    for (const auto& line : lines) all += line + L"\n";
    return all;
}

// ---------------------------------------------------------------------------
// WslTest

void WslTest::SetUp() {
    std::wstring suffix = UniqueSuffix();
    root_ = std::wstring(kTestRootKey) + L"\\" + suffix;
    lxssKey_ = root_ + L"\\Lxss";
    RegCreateKeyExW(HKEY_CURRENT_USER, lxssKey_.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, nullptr, nullptr);

    wchar_t tempBase[MAX_PATH];
    GetTempPathW(MAX_PATH, tempBase);
    temp_ = fs::path(tempBase) / L"wslmgr-tests" / suffix;
    fs::remove_all(temp_);
    fs::create_directories(temp_);

    wslmgr::Environment& env = wslmgr::CurrentEnvironment();
    env = wslmgr::Environment{};
    env.wslExe = FakeWslPath().wstring();
    env.lxssKey = lxssKey_;
    env.settingsKey = root_ + L"\\Settings";
    env.diskReleaseTimeoutMs = 3000;
    env.diskReleasePollMs = 20;

    ClearFakeVariables();
    SetEnvironmentVariableW(L"FAKE_WSL_LXSS", lxssKey_.c_str());
    SetEnvironmentVariableW(L"FAKE_WSL_LOG", (temp_ / L"wsl-calls.log").c_str());
}

void WslTest::TearDown() {
    InstallSafeEnvironment();
    RegDeleteTreeW(HKEY_CURRENT_USER, root_.c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, root_.c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, kTestRootKey);  // only succeeds once empty
    std::error_code ec;
    fs::remove_all(temp_, ec);
}

std::wstring WslTest::AddDistro(const DistroSpec& spec) {
    GUID guid;
    CoCreateGuid(&guid);
    wchar_t id[64];
    StringFromGUID2(guid, id, 64);
    std::wstring key = DistroKey(id);

    fs::path base = spec.basePath.empty() ? temp_ / L"instances" / spec.name : fs::path(spec.basePath);
    SetRegString(key, L"DistributionName", spec.name);
    if (spec.basePath != L"-") {
        fs::create_directories(base);
        SetRegString(key, L"BasePath", (spec.longPathPrefix ? L"\\\\?\\" : L"") + base.wstring());
    }
    if (spec.includeFlags) SetRegDword(key, L"Flags", spec.version == 2 ? 0xF : 0x7);
    SetRegDword(key, L"State", spec.state);
    SetRegDword(key, L"DefaultUid", spec.uid);
    if (!spec.flavor.empty()) SetRegString(key, L"Flavor", spec.flavor);
    if (!spec.osVersion.empty()) SetRegString(key, L"OsVersion", spec.osVersion);
    if (spec.running) SetRegDword(key, L"FakeRunning", 1);
    if (spec.version == 2) {
        SetRegString(key, L"VhdFileName", L"ext4.vhdx");
        if (spec.createDisk && spec.basePath != L"-") WriteFile(base / L"ext4.vhdx", spec.diskContent);
    }
    return id;
}

void WslTest::MakeDefault(const std::wstring& id) { SetRegString(lxssKey_, L"DefaultDistribution", id); }

std::wstring WslTest::RegString(const std::wstring& key, const wchar_t* value) const {
    wchar_t buffer[2048] = {};
    DWORD size = sizeof(buffer);
    RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value, RRF_RT_REG_SZ, nullptr, buffer, &size);
    return buffer;
}

DWORD WslTest::RegDword(const std::wstring& key, const wchar_t* value) const {
    DWORD data = 0;
    DWORD size = sizeof(data);
    RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value, RRF_RT_REG_DWORD, nullptr, &data, &size);
    return data;
}

void WslTest::SetFake(const wchar_t* name, const std::wstring& value) { SetEnvironmentVariableW(name, value.c_str()); }

std::vector<std::wstring> WslTest::WslCalls() const {
    std::vector<std::wstring> calls;
    std::istringstream lines(ReadFile(temp_ / L"wsl-calls.log"));
    std::string line;
    while (std::getline(lines, line)) calls.push_back(FromUtf8(line));
    return calls;
}

bool WslTest::Called(std::wstring_view argsSubstring) const {
    for (const auto& call : WslCalls())
        if (call.find(argsSubstring) != std::wstring::npos) return true;
    return false;
}

CliResult WslTest::RunCli(std::vector<std::wstring> args, const char* input) {
    fs::path outPath = temp_ / L"cli-out.txt";
    fs::path errPath = temp_ / L"cli-err.txt";
    fs::path inPath = temp_ / L"cli-in.txt";
    WriteFile(inPath, input ? input : "");

    auto open = [](const fs::path& path, DWORD access, DWORD disposition) {
        return CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, disposition, 0, nullptr);
    };
    HANDLE out = open(outPath, GENERIC_WRITE, CREATE_ALWAYS);
    HANDLE err = open(errPath, GENERIC_WRITE, CREATE_ALWAYS);
    HANDLE in = input ? open(inPath, GENERIC_READ, OPEN_EXISTING) : nullptr;

    HANDLE oldOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE oldErr = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE oldIn = GetStdHandle(STD_INPUT_HANDLE);
    SetStdHandle(STD_OUTPUT_HANDLE, out);
    SetStdHandle(STD_ERROR_HANDLE, err);
    SetStdHandle(STD_INPUT_HANDLE, in);

    args.insert(args.begin(), L"wslmgr");
    std::vector<wchar_t*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    CliResult result;
    result.exitCode = ::RunCli(static_cast<int>(argv.size()), argv.data());

    SetStdHandle(STD_OUTPUT_HANDLE, oldOut);
    SetStdHandle(STD_ERROR_HANDLE, oldErr);
    SetStdHandle(STD_INPUT_HANDLE, oldIn);
    CloseHandle(out);
    CloseHandle(err);
    if (in) CloseHandle(in);

    result.out = FromUtf8(ReadFile(outPath));
    result.err = FromUtf8(ReadFile(errPath));
    return result;
}

void WslTest::WriteFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << content;
}

std::string WslTest::ReadFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

// ---------------------------------------------------------------------------
// ConsoleScope

ConsoleScope::ConsoleScope() {
    if (!GetConsoleWindow()) {
        allocated_ = AllocConsole() != 0;
        if (HWND window = GetConsoleWindow()) ShowWindow(window, SW_HIDE);
    }
    buffer_ = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        CONSOLE_TEXTMODE_BUFFER, nullptr);
    input_ = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
    FlushConsoleInputBuffer(input_);
    oldOut_ = GetStdHandle(STD_OUTPUT_HANDLE);
    oldErr_ = GetStdHandle(STD_ERROR_HANDLE);
    oldIn_ = GetStdHandle(STD_INPUT_HANDLE);
    SetStdHandle(STD_OUTPUT_HANDLE, buffer_);
    SetStdHandle(STD_ERROR_HANDLE, buffer_);
    SetStdHandle(STD_INPUT_HANDLE, input_);
}

ConsoleScope::~ConsoleScope() {
    SetStdHandle(STD_OUTPUT_HANDLE, oldOut_);
    SetStdHandle(STD_ERROR_HANDLE, oldErr_);
    SetStdHandle(STD_INPUT_HANDLE, oldIn_);
    CloseHandle(buffer_);
    CloseHandle(input_);
    if (allocated_) FreeConsole();
}

std::wstring ConsoleScope::Text() const {
    CONSOLE_SCREEN_BUFFER_INFO info;
    GetConsoleScreenBufferInfo(buffer_, &info);
    std::wstring text;
    for (SHORT row = 0; row <= info.dwCursorPosition.Y; ++row) {
        std::wstring line(static_cast<size_t>(info.dwSize.X), L' ');
        DWORD read = 0;
        ReadConsoleOutputCharacterW(buffer_, line.data(), info.dwSize.X, COORD{0, row}, &read);
        line.erase(line.find_last_not_of(L' ') + 1);
        text += line + L"\n";
    }
    return text;
}

void ConsoleScope::Type(const std::wstring& keys) {
    std::vector<INPUT_RECORD> records;
    for (wchar_t c : keys) {
        for (BOOL down : {TRUE, FALSE}) {
            INPUT_RECORD record{};
            record.EventType = KEY_EVENT;
            record.Event.KeyEvent.bKeyDown = down;
            record.Event.KeyEvent.wRepeatCount = 1;
            record.Event.KeyEvent.wVirtualKeyCode = c == L'\r' ? VK_RETURN : VkKeyScanW(c) & 0xFF;
            record.Event.KeyEvent.uChar.UnicodeChar = c;
            records.push_back(record);
        }
    }
    DWORD written = 0;
    WriteConsoleInputW(input_, records.data(), static_cast<DWORD>(records.size()), &written);
}

}  // namespace test
