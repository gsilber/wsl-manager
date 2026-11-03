// fake_wsl: a stand-in for wsl.exe used by the test suite.
//
// It simulates the parts of WSL that WSL Manager relies on: the per-user Lxss
// registry entries and the virtual disk files. All state lives under a scratch
// HKCU key and scratch folders, so tests never touch real WSL instances.
//
// Environment variables:
//   FAKE_WSL_LXSS         HKCU-relative key to use as the Lxss root (required)
//   FAKE_WSL_LOG          file that receives each invocation's arguments, tab-separated
//   FAKE_WSL_FAIL         fail any invocation whose tab-joined arguments contain this text
//   FAKE_WSL_FAIL_MSG     message printed for a simulated failure; "-" prints nothing
//   FAKE_WSL_UTF16        "1": write output as UTF-16LE, as wsl.exe does without WSL_UTF8
//   FAKE_WSL_PROGRESS     "1": print '\r'-terminated progress lines during long commands
//                         (and before a simulated failure)
//   FAKE_WSL_NO_REGISTER  "1": --install and --import succeed without registering
//   FAKE_WSL_NO_FILES     "1": --export succeeds without writing the file
//   FAKE_WSL_ONLINE       "empty" or "bare": variants of the --list --online catalog
//
// Test-only command:  --emit <hex>   writes the given raw bytes to stdout.

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr DWORD kWsl2Flags = 0xF;
constexpr DWORD kWsl1Flags = 0x7;

struct Failure {
    std::wstring message;
};

std::wstring Env(const wchar_t* name) {
    wchar_t buffer[4096];
    DWORD len = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    return len && len < std::size(buffer) ? std::wstring(buffer, len) : std::wstring();
}

bool Flag(const wchar_t* name) { return Env(name) == L"1"; }

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), len, nullptr, nullptr);
    return out;
}

void WriteRaw(const void* data, size_t size) {
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), data, static_cast<DWORD>(size), &written, nullptr);
}

void Print(const std::wstring& text) {
    if (Flag(L"FAKE_WSL_UTF16")) {
        WriteRaw(text.data(), text.size() * sizeof(wchar_t));
    } else {
        std::string utf8 = ToUtf8(text);
        WriteRaw(utf8.data(), utf8.size());
    }
}

void PrintLine(const std::wstring& text) { Print(text + L"\r\n"); }

void Progress(const std::wstring& what) {
    if (!Flag(L"FAKE_WSL_PROGRESS")) return;
    for (int percent : {0, 50, 100}) Print(what + L" " + std::to_wstring(percent) + L"%\r");
}

bool EqualsIgnoreCase(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// ---------------------------------------------------------------------------
// Registry model

std::wstring LxssKey() { return Env(L"FAKE_WSL_LXSS"); }
std::wstring DistroKey(const std::wstring& id) { return LxssKey() + L"\\" + id; }

std::optional<std::wstring> GetString(const std::wstring& key, const wchar_t* value) {
    wchar_t buffer[2048];
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS)
        return std::nullopt;
    return std::wstring(buffer);
}

DWORD GetDword(const std::wstring& key, const wchar_t* value, DWORD fallback) {
    DWORD data = 0;
    DWORD size = sizeof(data);
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value, RRF_RT_REG_DWORD, nullptr, &data, &size) != ERROR_SUCCESS)
        return fallback;
    return data;
}

void SetString(const std::wstring& key, const wchar_t* value, const std::wstring& data) {
    RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), value, REG_SZ, data.c_str(),
                    static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
}

void SetDword(const std::wstring& key, const wchar_t* value, DWORD data) {
    RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), value, REG_DWORD, &data, sizeof(data));
}

std::vector<std::wstring> DistroIds() {
    std::vector<std::wstring> ids;
    HKEY root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, LxssKey().c_str(), 0, KEY_READ, &root) != ERROR_SUCCESS) return ids;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD len = static_cast<DWORD>(std::size(name));
        if (RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        ids.push_back(name);
    }
    RegCloseKey(root);
    return ids;
}

std::optional<std::wstring> FindId(const std::wstring& name) {
    for (const auto& id : DistroIds()) {
        auto distroName = GetString(DistroKey(id), L"DistributionName");
        if (distroName && EqualsIgnoreCase(*distroName, name)) return id;
    }
    return std::nullopt;
}

std::wstring RequireId(const std::wstring& name) {
    if (auto id = FindId(name)) return *id;
    throw Failure{L"There is no distribution with the supplied name.\r\nError code: Wsl/Service/WSL_E_DISTRO_NOT_FOUND"};
}

std::wstring BasePath(const std::wstring& id) {
    std::wstring path = GetString(DistroKey(id), L"BasePath").value_or(L"");
    return path.rfind(L"\\\\?\\", 0) == 0 ? path.substr(4) : path;
}

std::wstring NewId() {
    GUID guid;
    CoCreateGuid(&guid);
    wchar_t text[64];
    StringFromGUID2(guid, text, static_cast<int>(std::size(text)));
    return text;
}

void Register(const std::wstring& name, const std::wstring& location, int version, const std::wstring& flavor) {
    if (Flag(L"FAKE_WSL_NO_REGISTER")) return;
    std::wstring key = DistroKey(NewId());
    SetString(key, L"DistributionName", name);
    SetString(key, L"BasePath", L"\\\\?\\" + location);
    SetDword(key, L"Flags", version == 2 ? kWsl2Flags : kWsl1Flags);
    SetDword(key, L"State", 1);
    SetDword(key, L"Version", 2);
    SetDword(key, L"DefaultUid", 0);
    if (version == 2) SetString(key, L"VhdFileName", L"ext4.vhdx");
    if (!flavor.empty()) SetString(key, L"Flavor", flavor);
}

// ---------------------------------------------------------------------------
// Commands

std::wstring OptionValue(const std::vector<std::wstring>& args, const wchar_t* option, const wchar_t* fallback = L"") {
    auto it = std::find(args.begin(), args.end(), option);
    return it != args.end() && it + 1 != args.end() ? *(it + 1) : fallback;
}

bool HasOption(const std::vector<std::wstring>& args, const wchar_t* option) {
    return std::find(args.begin(), args.end(), option) != args.end();
}

void WriteTextFile(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw Failure{L"The system cannot find the path specified."};
    file << text;
}

int ListOnline() {
    std::wstring mode = Env(L"FAKE_WSL_ONLINE");
    PrintLine(L"The following is a list of valid distributions that can be installed.");
    PrintLine(L"Install using 'wsl.exe --install <Distro>'.");
    PrintLine(L"");
    if (mode == L"empty") return 0;
    if (mode == L"bare") PrintLine(L"NAMES below are case-insensitive.");  // not the table header
    PrintLine(L"NAME            FRIENDLY NAME");
    PrintLine(L"Ubuntu          Ubuntu");
    PrintLine(L"Debian          Debian GNU/Linux");
    PrintLine(L"");
    if (mode == L"bare") PrintLine(L"Bare-Distro");
    return 0;
}

int ListRunning() {
    bool any = false;
    for (const auto& id : DistroIds()) {
        if (GetDword(DistroKey(id), L"FakeRunning", 0)) {
            PrintLine(GetString(DistroKey(id), L"DistributionName").value_or(L""));
            any = true;
        }
    }
    if (any) {
        PrintLine(L"");  // wsl.exe ends the list with a blank line
        return 0;
    }
    PrintLine(L"There are no running distributions.");
    return -1;
}

int Install(const std::vector<std::wstring>& args) {
    std::wstring distro = args.size() > 1 ? args[1] : L"";
    std::wstring name = OptionValue(args, L"--name", distro.c_str());
    std::wstring location = OptionValue(args, L"--location");
    int version = std::stoi(OptionValue(args, L"--version", L"2"));
    if (FindId(name)) throw Failure{L"A distribution with the supplied name already exists."};
    Progress(L"Downloading:");
    fs::create_directories(location);
    if (version == 2) WriteTextFile(fs::path(location) / L"ext4.vhdx", "vhd:" + ToUtf8(distro));
    Register(name, location, version, L"fake");
    PrintLine(L"The operation completed successfully.");
    return 0;
}

int Import(const std::vector<std::wstring>& args) {
    if (args.size() < 4) throw Failure{L"Invalid command line argument: --import"};
    const std::wstring& name = args[1];
    const std::wstring& location = args[2];
    const std::wstring& file = args[3];
    bool vhd = HasOption(args, L"--vhd");
    int version = vhd ? 2 : std::stoi(OptionValue(args, L"--version", L"2"));
    if (!fs::is_regular_file(file)) throw Failure{L"The system cannot find the file specified."};
    if (FindId(name)) throw Failure{L"A distribution with the supplied name already exists."};
    Progress(L"Importing:");
    fs::create_directories(location);
    if (version == 2)
        fs::copy_file(file, fs::path(location) / L"ext4.vhdx", fs::copy_options::overwrite_existing);
    else
        fs::create_directories(fs::path(location) / L"rootfs");
    Register(name, location, version, L"");
    PrintLine(L"The operation completed successfully.");
    return 0;
}

int Export(const std::vector<std::wstring>& args) {
    if (args.size() < 3) throw Failure{L"Invalid command line argument: --export"};
    std::wstring id = RequireId(args[1]);
    const std::wstring& file = args[2];
    std::wstring format = OptionValue(args, L"--format", L"tar");
    Progress(L"Exporting:");
    if (!Flag(L"FAKE_WSL_NO_FILES")) {
        if (format == L"vhd")
            fs::copy_file(fs::path(BasePath(id)) / L"ext4.vhdx", file, fs::copy_options::overwrite_existing);
        else
            WriteTextFile(file, "tar:" + ToUtf8(args[1]));
    }
    PrintLine(L"The operation completed successfully.");
    return 0;
}

int Unregister(const std::wstring& name) {
    std::wstring id = RequireId(name);
    std::wstring base = BasePath(id);
    std::error_code ec;
    fs::remove(fs::path(base) / L"ext4.vhdx", ec);
    fs::remove_all(fs::path(base) / L"rootfs", ec);
    if (EqualsIgnoreCase(GetString(LxssKey(), L"DefaultDistribution").value_or(L""), id))
        RegDeleteKeyValueW(HKEY_CURRENT_USER, LxssKey().c_str(), L"DefaultDistribution");
    RegDeleteTreeW(HKEY_CURRENT_USER, DistroKey(id).c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, DistroKey(id).c_str());
    PrintLine(L"The operation completed successfully.");
    return 0;
}

int Manage(const std::vector<std::wstring>& args) {
    std::wstring id = RequireId(args.size() > 1 ? args[1] : L"");
    if (HasOption(args, L"--move")) {
        std::wstring target = OptionValue(args, L"--move");
        fs::create_directories(target);
        if (!MoveFileExW((fs::path(BasePath(id)) / L"ext4.vhdx").c_str(), (fs::path(target) / L"ext4.vhdx").c_str(),
                         MOVEFILE_COPY_ALLOWED))
            throw Failure{L"The process cannot access the file because it is being used by another process."};
        SetString(DistroKey(id), L"BasePath", L"\\\\?\\" + target);
    } else if (HasOption(args, L"--set-default-user")) {
        if (OptionValue(args, L"--set-default-user") == L"nobody") throw Failure{L"The user was not found."};
        SetDword(DistroKey(id), L"DefaultUid", 1000);
    } else {
        throw Failure{L"Invalid command line argument: --manage"};
    }
    PrintLine(L"The operation completed successfully.");
    return 0;
}

int Dispatch(const std::vector<std::wstring>& args) {
    if (args.empty()) throw Failure{L"No command."};
    const std::wstring& command = args[0];

    if (command == L"--emit") {
        std::string bytes;
        for (size_t i = 0; i + 1 < args.at(1).size(); i += 2)
            bytes.push_back(static_cast<char>(std::stoi(args[1].substr(i, 2), nullptr, 16)));
        WriteRaw(bytes.data(), bytes.size());
        return 0;
    }
    if (command == L"--list") {
        if (HasOption(args, L"--online")) return ListOnline();
        if (HasOption(args, L"--running")) return ListRunning();
        for (const auto& id : DistroIds()) PrintLine(GetString(DistroKey(id), L"DistributionName").value_or(L""));
        return 0;
    }
    if (command == L"--install") return Install(args);
    if (command == L"--import") return Import(args);
    if (command == L"--export") return Export(args);
    if (command == L"--unregister") return Unregister(args.size() > 1 ? args[1] : L"");
    if (command == L"--manage") return Manage(args);
    if (command == L"--set-default") {
        SetString(LxssKey(), L"DefaultDistribution", RequireId(args.size() > 1 ? args[1] : L""));
        return 0;
    }
    if (command == L"--terminate") {
        SetDword(DistroKey(RequireId(args.size() > 1 ? args[1] : L"")), L"FakeRunning", 0);
        PrintLine(L"The operation completed successfully.");
        return 0;
    }
    if (command == L"--shutdown") {
        for (const auto& id : DistroIds()) SetDword(DistroKey(id), L"FakeRunning", 0);
        return 0;
    }
    if (command == L"--distribution") {
        SetDword(DistroKey(RequireId(args.size() > 1 ? args[1] : L"")), L"FakeRunning", 1);
        return 0;
    }
    throw Failure{L"Invalid command line argument: " + command};
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::vector<std::wstring> args(argv + 1, argv + argc);
    std::wstring joined;
    for (const auto& arg : args) joined += (joined.empty() ? L"" : L"\t") + arg;

    if (std::wstring log = Env(L"FAKE_WSL_LOG"); !log.empty()) {
        std::ofstream file(fs::path(log), std::ios::binary | std::ios::app);
        file << ToUtf8(joined) << "\n";
    }
    if (LxssKey().empty()) {
        PrintLine(L"fake_wsl: FAKE_WSL_LXSS is not set; refusing to run.");
        return 2;
    }

    std::wstring fail = Env(L"FAKE_WSL_FAIL");
    if (!fail.empty() && joined.find(fail) != std::wstring::npos) {
        Progress(L"Working:");
        std::wstring message = Env(L"FAKE_WSL_FAIL_MSG");
        if (message != L"-") PrintLine(message.empty() ? L"Simulated failure.\r\nError code: Fake/E_FAIL" : message);
        return -1;
    }

    try {
        return Dispatch(args);
    } catch (const Failure& failure) {
        PrintLine(failure.message);
    } catch (const std::exception& e) {
        PrintLine(L"fake_wsl internal error: " + std::wstring(e.what(), e.what() + strlen(e.what())));
    }
    return -1;
}
