#include "Wsl.h"

#include "Environment.h"
#include "Process.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace wslmgr {
namespace {

constexpr wchar_t kMetadataSuffix[] = L".wslmeta";
constexpr wchar_t kDefaultVhdName[] = L"ext4.vhdx";
constexpr DWORD kFlagWsl2 = 0x8;  // LXSS_DISTRO_FLAGS_VM_MODE

// Docker Desktop owns these; changing them behind its back breaks Docker.
constexpr const wchar_t* kProtectedByDefault[] = {L"docker-desktop", L"docker-desktop-data"};

const wchar_t* LxssKey() { return CurrentEnvironment().lxssKey.c_str(); }
const wchar_t* SettingsKey() { return CurrentEnvironment().settingsKey.c_str(); }
std::wstring LxssSubKey(const std::wstring& id) { return CurrentEnvironment().lxssKey + L"\\" + id; }
// One DWORD per instance name (1 = protected, 0 = explicitly unprotected).
std::wstring ProtectionKey() { return CurrentEnvironment().settingsKey + L"\\Protected"; }

std::optional<std::wstring> ReadRegString(const wchar_t* key, const wchar_t* value) {
    std::wstring text(MAX_PATH, L'\0');
    for (;;) {
        DWORD size = static_cast<DWORD>(text.size() * sizeof(wchar_t));
        LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_SZ, nullptr, text.data(), &size);
        if (status == ERROR_MORE_DATA) {
            text.resize(size / sizeof(wchar_t) + 1);
            continue;
        }
        if (status != ERROR_SUCCESS) return std::nullopt;
        text.resize(wcsnlen(text.c_str(), text.size()));
        return text;
    }
}

std::optional<DWORD> ReadRegDword(const wchar_t* key, const wchar_t* value) {
    DWORD data = 0;
    DWORD size = sizeof(data);
    if (RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_DWORD, nullptr, &data, &size) != ERROR_SUCCESS)
        return std::nullopt;
    return data;
}

void WriteRegValue(const wchar_t* key, const wchar_t* value, DWORD type, const void* data, size_t size) {
    LSTATUS status = RegSetKeyValueW(HKEY_CURRENT_USER, key, value, type, data, static_cast<DWORD>(size));
    if (status != ERROR_SUCCESS) throw WslError(L"Could not write to the registry: " + LastErrorMessage(status));
}

void WriteRegString(const wchar_t* key, const wchar_t* value, std::wstring_view data) {
    std::wstring text(data);
    WriteRegValue(key, value, REG_SZ, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
}

void WriteRegDword(const wchar_t* key, const wchar_t* value, DWORD data) {
    WriteRegValue(key, value, REG_DWORD, &data, sizeof(data));
}

void Say(const OutputFn& out, const std::wstring& message) {
    if (out) out(message, false);
}

// Runs wsl.exe with the given arguments; throws with wsl.exe's own message on failure.
ProcessResult RunWsl(const std::vector<std::wstring>& args, const OutputFn& out, bool check = true) {
    ProcessResult result = RunProcess(WslExePath(), args, out);
    if (check && result.exitCode != 0) {
        std::wstring message = Trim(result.output);
        if (message.empty()) {
            wchar_t code[16];
            swprintf_s(code, L"0x%08lX", result.exitCode);
            message = L"wsl.exe exited with code " + std::wstring(code) + L".";
        }
        throw WslError(L"wsl.exe " + args.front() + L" failed:\n" + message);
    }
    return result;
}

Distro RequireDistro(std::wstring_view name) {
    auto distro = FindDistro(name, true);
    if (!distro) throw WslError(L"There is no WSL instance named '" + std::wstring(name) + L"'.");
    return *distro;
}

// Like RequireDistro, but refuses protected instances. `what` completes
// "WSL Manager won't ...", e.g. L"remove it".
Distro RequireUnprotected(std::wstring_view name, const wchar_t* what) {
    Distro distro = RequireDistro(name);
    if (distro.isProtected)
        throw WslError(L"'" + distro.name + L"' is protected, so WSL Manager won't " + what +
                       L". If you're sure, unprotect it first (Instance > Protected in the app, or "
                       L"wslmgr unprotect " + distro.name + L").");
    return distro;
}

void StopDistro(const Distro& distro, const OutputFn& out) {
    if (distro.running) Say(out, L"Stopping " + distro.name + L"...");
    RunWsl({L"--terminate", distro.name}, nullptr, false);
}

// After an instance is terminated, the WSL VM can keep its virtual disk attached
// until the VM goes idle (typically under a minute). Operations that copy or move
// the disk file wait for that instead of failing with a sharing violation.
void WaitForDiskRelease(const std::wstring& vhdPath, const OutputFn& out) {
    if (vhdPath.empty()) return;
    const ULONGLONG timeoutMs = CurrentEnvironment().diskReleaseTimeoutMs;
    const DWORD pollMs = CurrentEnvironment().diskReleasePollMs;
    bool announced = false;
    for (ULONGLONG start = GetTickCount64();;) {
        HANDLE file = CreateFileW(vhdPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            return;
        }
        if (GetLastError() != ERROR_SHARING_VIOLATION) return;  // e.g. missing file: let wsl.exe report it
        ULONGLONG elapsed = GetTickCount64() - start;
        if (elapsed >= timeoutMs)
            throw WslError(L"The virtual disk " + vhdPath +
                           L" is still in use. Make sure the instance is stopped, or shut down WSL "
                           L"(this stops all instances) and try again.");
        if (!announced) {
            Say(out, L"Waiting for WSL to release the virtual disk (this can take up to a minute)...");
            announced = true;
        }
        if (out) out(L"Waiting for the virtual disk... " + std::to_wstring(elapsed / 1000) + L"s", true);
        Sleep(pollMs);
    }
}

void ValidateNameSyntax(std::wstring_view name) {
    if (name.empty()) throw WslError(L"The instance name cannot be empty.");
    if (name.size() > 64) throw WslError(L"The instance name must be 64 characters or fewer.");
    for (wchar_t c : name) {
        bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'.' ||
                  c == L'_' || c == L'-';
        if (!ok)
            throw WslError(L"The instance name may only contain letters, digits, '.', '_' and '-' ('" +
                           std::wstring(name) + L"').");
    }
}

bool RemoveFolderIfEmpty(const std::wstring& folder) {
    std::error_code ec;
    if (folder.empty() || !fs::is_directory(folder, ec) || !fs::is_empty(folder, ec)) return false;
    return RemoveDirectoryW(folder.c_str()) != 0;
}

// Checks that `location` can hold a new virtual disk and creates the folder.
std::wstring PrepareTargetFolder(std::wstring_view location) {
    std::wstring folder = NormalizeFolder(location);
    std::error_code ec;
    if (fs::exists(folder, ec) && !fs::is_directory(folder, ec))
        throw WslError(L"The storage location is a file, not a folder:\n" + folder);
    for (const Distro& d : ListDistros(false)) {
        if (!d.basePath.empty() && EqualsIgnoreCase(NormalizeFolder(d.basePath), folder))
            throw WslError(L"The folder " + folder + L" already holds the instance '" + d.name +
                           L"'. Choose a different storage location.");
    }
    if (fs::exists(fs::path(folder) / kDefaultVhdName, ec))
        throw WslError(L"The folder " + folder + L" already contains a virtual disk (" + kDefaultVhdName +
                       L"). Choose an empty folder.");
    fs::create_directories(folder, ec);
    if (ec) throw WslError(L"Could not create the folder " + folder + L":\n" + LastErrorMessage(ec.value()));
    return folder;
}

std::wstring AbsolutePath(std::wstring_view path) {
    std::error_code ec;
    fs::path absolute = fs::absolute(fs::path(Trim(path)), ec);
    return ec ? std::wstring(Trim(path)) : absolute.lexically_normal().wstring();
}

void WriteMetadata(const std::wstring& backupFile, const Distro& distro, BackupFormat format) {
    std::ofstream file(fs::path(backupFile + kMetadataSuffix), std::ios::binary | std::ios::trunc);
    file << "# WSL Manager backup metadata\n"
         << "name=" << WideToUtf8(distro.name) << "\n"
         << "wsl_version=" << distro.version << "\n"
         << "default_uid=" << distro.defaultUid << "\n"
         << "flavor=" << WideToUtf8(distro.flavor) << "\n"
         << "os_version=" << WideToUtf8(distro.osVersion) << "\n"
         << "format=" << WideToUtf8(FormatArg(format)) << "\n"
         << "created=" << WideToUtf8(Timestamp()) << "\n";
}

std::map<std::wstring, std::wstring> ReadMetadata(const std::wstring& backupFile) {
    std::map<std::wstring, std::wstring> values;
    std::ifstream file(fs::path(backupFile + kMetadataSuffix), std::ios::binary);
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        values[Trim(Utf8ToWide(line.substr(0, eq)))] = Trim(Utf8ToWide(line.substr(eq + 1)));
    }
    return values;
}

// wsl --import registers a bare instance; carry over the default user and the
// OS description from the source instance or backup metadata.
void ApplyDetails(std::wstring_view name, uint32_t uid, const std::wstring& flavor, const std::wstring& osVersion,
                  const OutputFn& out) {
    auto distro = FindDistro(name);
    if (!distro) return;
    std::wstring key = LxssSubKey(distro->id);
    if (!flavor.empty()) WriteRegString(key.c_str(), L"Flavor", flavor);
    if (!osVersion.empty()) WriteRegString(key.c_str(), L"OsVersion", osVersion);
    if (uid != 0) {
        WriteRegDword(key.c_str(), L"DefaultUid", uid);
        Say(out, L"Default user restored (uid " + std::to_wstring(uid) + L").");
    }
}

std::vector<std::wstring> RunningDistroNames() {
    std::vector<std::wstring> names;
    ProcessResult result = RunWsl({L"--list", L"--running", L"--quiet"}, nullptr, false);
    std::wistringstream lines(result.output);
    std::wstring line;
    while (std::getline(lines, line)) {
        line = Trim(line);
        if (!line.empty()) names.push_back(line);
    }
    return names;
}

}  // namespace

Environment& CurrentEnvironment() {
    static Environment environment;
    return environment;
}

std::wstring WslExePath() {
    if (!CurrentEnvironment().wslExe.empty()) return CurrentEnvironment().wslExe;
    wchar_t system[MAX_PATH];
    UINT len = GetSystemDirectoryW(system, MAX_PATH);
    return std::wstring(system, len) + L"\\wsl.exe";
}

const wchar_t* FormatArg(BackupFormat format) {
    switch (format) {
        case BackupFormat::Vhd: return L"vhd";
        case BackupFormat::TarGz: return L"tar.gz";
        case BackupFormat::TarXz: return L"tar.xz";
        default: return L"tar";
    }
}

const wchar_t* FormatExtension(BackupFormat format) {
    switch (format) {
        case BackupFormat::Vhd: return L".vhdx";
        case BackupFormat::TarGz: return L".tar.gz";
        case BackupFormat::TarXz: return L".tar.xz";
        default: return L".tar";
    }
}

std::optional<BackupFormat> ParseFormat(std::wstring_view text) {
    if (EqualsIgnoreCase(text, L"vhd") || EqualsIgnoreCase(text, L"vhdx")) return BackupFormat::Vhd;
    if (EqualsIgnoreCase(text, L"tar")) return BackupFormat::Tar;
    if (EqualsIgnoreCase(text, L"tar.gz") || EqualsIgnoreCase(text, L"tgz")) return BackupFormat::TarGz;
    if (EqualsIgnoreCase(text, L"tar.xz") || EqualsIgnoreCase(text, L"txz")) return BackupFormat::TarXz;
    return std::nullopt;
}

BackupFormat FormatFromPath(std::wstring_view path) {
    if (EndsWithIgnoreCase(path, L".vhdx") || EndsWithIgnoreCase(path, L".vhd")) return BackupFormat::Vhd;
    if (EndsWithIgnoreCase(path, L".tar.gz") || EndsWithIgnoreCase(path, L".tgz")) return BackupFormat::TarGz;
    if (EndsWithIgnoreCase(path, L".tar.xz") || EndsWithIgnoreCase(path, L".txz")) return BackupFormat::TarXz;
    return BackupFormat::Tar;
}

std::wstring StripBackupExtension(std::wstring_view fileName) {
    for (const wchar_t* ext : {L".tar.gz", L".tar.xz", L".tgz", L".txz", L".tar", L".vhdx", L".vhd"}) {
        if (EndsWithIgnoreCase(fileName, ext)) return std::wstring(fileName.substr(0, fileName.size() - wcslen(ext)));
    }
    return std::wstring(fileName);
}

std::wstring WithBackupExtension(std::wstring_view path, BackupFormat format) {
    return StripBackupExtension(path) + FormatExtension(format);
}

std::wstring StateName(const Distro& distro) {
    switch (distro.registryState) {
        case 1: return distro.running ? L"Running" : L"Stopped";
        case 3: return L"Installing";
        case 4: return L"Uninstalling";
        case 5: return L"Converting";
        default: return L"Unknown";
    }
}

std::wstring NormalizeFolder(std::wstring_view path) {
    std::wstring folder = AbsolutePath(StripLongPathPrefix(Trim(path)));
    if (folder.empty()) throw WslError(L"No storage location was given.");
    // AbsolutePath has normalised separators to '\'; keep the one in a drive root like "C:\".
    while (folder.size() > 3 && folder.back() == L'\\') folder.pop_back();
    return folder;
}

std::vector<Distro> ListDistros(bool queryRunning) {
    std::vector<Distro> result;
    HKEY lxss = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, LxssKey(), 0, KEY_READ, &lxss) != ERROR_SUCCESS) return result;
    std::wstring defaultId = ReadRegString(LxssKey(), L"DefaultDistribution").value_or(L"");

    for (DWORD index = 0;; ++index) {
        wchar_t id[256];  // registry key names are at most 255 characters
        DWORD idLen = static_cast<DWORD>(std::size(id));
        if (RegEnumKeyExW(lxss, index, id, &idLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;

        std::wstring key = LxssSubKey(id);
        auto name = ReadRegString(key.c_str(), L"DistributionName");
        if (!name || name->empty()) continue;

        Distro d;
        d.id = id;
        d.name = *name;
        d.basePath = StripLongPathPrefix(ReadRegString(key.c_str(), L"BasePath").value_or(L""));
        DWORD flags = ReadRegDword(key.c_str(), L"Flags").value_or(kFlagWsl2);
        d.version = (flags & kFlagWsl2) ? 2 : 1;
        d.defaultUid = ReadRegDword(key.c_str(), L"DefaultUid").value_or(0);
        d.registryState = ReadRegDword(key.c_str(), L"State").value_or(1);
        d.isDefault = EqualsIgnoreCase(d.id, defaultId);
        d.isProtected = IsProtected(d.name);
        d.flavor = ReadRegString(key.c_str(), L"Flavor").value_or(L"");
        d.osVersion = ReadRegString(key.c_str(), L"OsVersion").value_or(L"");
        if (d.version == 2 && !d.basePath.empty()) {
            std::wstring vhd = ReadRegString(key.c_str(), L"VhdFileName").value_or(kDefaultVhdName);
            d.vhdPath = (fs::path(d.basePath) / vhd).wstring();
            WIN32_FILE_ATTRIBUTE_DATA info;
            if (GetFileAttributesExW(d.vhdPath.c_str(), GetFileExInfoStandard, &info))
                d.diskBytes = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        }
        result.push_back(std::move(d));
    }
    RegCloseKey(lxss);

    if (queryRunning) {
        for (const std::wstring& running : RunningDistroNames()) {
            for (Distro& d : result) {
                if (EqualsIgnoreCase(d.name, running)) d.running = true;
            }
        }
    }
    std::sort(result.begin(), result.end(), [](const Distro& a, const Distro& b) {
        return CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    return result;
}

std::optional<Distro> FindDistro(std::wstring_view name, bool queryRunning) {
    for (Distro& d : ListDistros(queryRunning)) {
        if (EqualsIgnoreCase(d.name, name)) return std::move(d);
    }
    return std::nullopt;
}

std::vector<OnlineDistro> ListOnlineDistros() {
    ProcessResult result = RunWsl({L"--list", L"--online"}, nullptr);
    std::vector<OnlineDistro> distros;
    bool inTable = false;
    std::wistringstream lines(result.output);
    std::wstring line;
    while (std::getline(lines, line)) {
        std::wstring text = Trim(line);
        if (!inTable) {
            inTable = text.rfind(L"NAME", 0) == 0 && text.find(L"FRIENDLY NAME") != std::wstring::npos;
            continue;
        }
        if (text.empty()) continue;
        size_t split = text.find_first_of(L" \t");
        OnlineDistro d;
        d.name = text.substr(0, split);
        d.friendlyName = split == std::wstring::npos ? d.name : Trim(text.substr(split));
        distros.push_back(std::move(d));
    }
    if (distros.empty())
        throw WslError(L"Could not read the list of available distributions:\n" + Trim(result.output));
    return distros;
}

void ValidateNewName(std::wstring_view name) {
    ValidateNameSyntax(name);
    if (FindDistro(name)) throw WslError(L"A WSL instance named '" + std::wstring(name) + L"' already exists.");
}

bool IsProtected(std::wstring_view name) {
    if (auto setting = ReadRegDword(ProtectionKey().c_str(), std::wstring(name).c_str())) return *setting != 0;
    return std::any_of(std::begin(kProtectedByDefault), std::end(kProtectedByDefault),
                       [&](const wchar_t* protectedName) { return EqualsIgnoreCase(protectedName, name); });
}

void SetProtected(std::wstring_view name, bool isProtected) {
    Distro distro = RequireDistro(name);
    WriteRegDword(ProtectionKey().c_str(), distro.name.c_str(), isProtected ? 1 : 0);
}

std::wstring GetDefaultRoot() {
    if (auto root = ReadRegString(SettingsKey(), L"DefaultRoot"); root && !root->empty()) return *root;
    wchar_t profile[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    return (len && len < MAX_PATH ? std::wstring(profile, len) : std::wstring(L"C:")) + L"\\WSL";
}

void SetDefaultRoot(std::wstring_view folder) {
    WriteRegString(SettingsKey(), L"DefaultRoot", NormalizeFolder(folder));
}

std::wstring DefaultLocationFor(std::wstring_view name) {
    return (fs::path(GetDefaultRoot()) / std::wstring(name)).wstring();
}

std::wstring DefaultBackupPath(std::wstring_view name, BackupFormat format) {
    std::wstring file = std::wstring(name) + L"-" + Timestamp() + FormatExtension(format);
    return (fs::path(GetDefaultRoot()) / L"Backups" / file).wstring();
}

BackupFormat DefaultBackupFormat(const Distro& distro) {
    return distro.version == 2 ? BackupFormat::Vhd : BackupFormat::Tar;
}

void InstallClean(const InstallOptions& options, const OutputFn& out) {
    if (options.distro.empty()) throw WslError(L"No distribution was selected.");
    ValidateNewName(options.name);
    std::wstring folder = PrepareTargetFolder(options.location);
    Say(out, L"Installing " + options.distro + L" as '" + options.name + L"' in " + folder + L"...");

    std::vector<std::wstring> args{L"--install", options.distro, L"--name",      options.name,
                                   L"--location", folder,        L"--no-launch", L"--version",
                                   std::to_wstring(options.version)};
    if (options.webDownload) args.push_back(L"--web-download");
    try {
        RunWsl(args, out);
    } catch (...) {
        RemoveFolderIfEmpty(folder);
        throw;
    }

    if (FindDistro(options.name))
        Say(out, L"Installed '" + options.name + L"'. Open a shell on it to finish first-time setup.");
    else
        Say(out, L"Installation finished, but '" + options.name +
                     L"' is not registered yet. Launch it once to complete setup.");
}

void RestoreBackup(std::wstring_view name, std::wstring_view location, std::wstring_view file, int version,
                   const OutputFn& out) {
    ValidateNewName(name);
    std::wstring backup = AbsolutePath(file);
    std::error_code ec;
    if (!fs::is_regular_file(backup, ec)) throw WslError(L"The backup file was not found:\n" + backup);
    std::wstring folder = PrepareTargetFolder(location);

    bool isVhd = FormatFromPath(backup) == BackupFormat::Vhd;
    std::vector<std::wstring> args{L"--import", std::wstring(name), folder, backup};
    if (isVhd) {
        args.push_back(L"--vhd");
    } else {
        args.push_back(L"--version");
        args.push_back(std::to_wstring(version));
    }
    Say(out, L"Restoring " + backup + L" as '" + std::wstring(name) + L"' in " + folder + L"...");
    try {
        RunWsl(args, out);
    } catch (...) {
        RemoveFolderIfEmpty(folder);
        throw;
    }

    auto metadata = ReadMetadata(backup);
    ApplyDetails(name, static_cast<uint32_t>(wcstoul(metadata[L"default_uid"].c_str(), nullptr, 10)),
                 metadata[L"flavor"], metadata[L"os_version"], out);
    Say(out, L"Restored '" + std::wstring(name) + L"'.");
}

void Clone(std::wstring_view source, std::wstring_view name, std::wstring_view location, const OutputFn& out) {
    Distro src = RequireUnprotected(source, L"copy it (that would stop it)");
    ValidateNewName(name);
    std::wstring folder = PrepareTargetFolder(location);
    StopDistro(src, out);

    try {
        bool copied = false;
        std::error_code ec;
        if (src.version == 2 && !src.vhdPath.empty() && fs::is_regular_file(src.vhdPath, ec)) {
            // Importing the source's own disk makes a single copy, avoiding an export round trip.
            WaitForDiskRelease(src.vhdPath, out);
            Say(out, L"Copying the virtual disk of '" + src.name + L"' (" + FormatBytes(src.diskBytes) + L")...");
            try {
                RunWsl({L"--import", std::wstring(name), folder, src.vhdPath, L"--vhd"}, out);
                copied = true;
            } catch (const WslError& e) {
                Say(out, L"Direct disk copy failed; falling back to export and import.\n" + e.message());
            }
        }
        if (!copied) {
            BackupFormat format = src.version == 2 ? BackupFormat::Vhd : BackupFormat::Tar;
            std::wstring temp = (fs::path(folder) / (L"wslmgr-clone-temp" + std::wstring(FormatExtension(format))))
                                    .wstring();
            Say(out, L"Exporting '" + src.name + L"'...");
            try {
                RunWsl({L"--export", src.name, temp, L"--format", FormatArg(format)}, out);
                Say(out, L"Importing as '" + std::wstring(name) + L"'...");
                std::vector<std::wstring> args{L"--import", std::wstring(name), folder, temp};
                if (format == BackupFormat::Vhd) {
                    args.push_back(L"--vhd");
                } else {
                    args.push_back(L"--version");
                    args.push_back(std::to_wstring(src.version));
                }
                RunWsl(args, out);
            } catch (...) {
                DeleteFileW(temp.c_str());
                throw;
            }
            DeleteFileW(temp.c_str());
        }
    } catch (...) {
        RemoveFolderIfEmpty(folder);
        throw;
    }

    ApplyDetails(name, src.defaultUid, src.flavor, src.osVersion, out);
    Say(out, L"Created '" + std::wstring(name) + L"' as a copy of '" + src.name + L"'.");
}

void Backup(std::wstring_view name, std::wstring_view file, BackupFormat format, const OutputFn& out) {
    Distro distro = RequireUnprotected(name, L"back it up (that would stop it)");
    if (format == BackupFormat::Vhd && distro.version != 2)
        throw WslError(L"The vhdx backup format is only available for WSL 2 instances.");

    std::wstring path = AbsolutePath(file);
    if (path.empty()) throw WslError(L"No backup file was given.");
    std::error_code ec;
    fs::path parent = fs::path(path).parent_path();
    fs::create_directories(parent, ec);
    if (ec) throw WslError(L"Could not create the folder " + parent.wstring() + L":\n" + LastErrorMessage(ec.value()));

    StopDistro(distro, out);
    Say(out, L"Exporting '" + distro.name + L"' to " + path + L" (" + FormatArg(format) + L")...");
    RunWsl({L"--export", distro.name, path, L"--format", FormatArg(format)}, out);
    WriteMetadata(path, distro, format);

    uint64_t size = 0;
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
        size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    Say(out, L"Backup saved: " + path + L" (" + FormatBytes(size) + L").");
}

void Remove(std::wstring_view name, bool deleteEmptyFolder, const OutputFn& out) {
    Distro distro = RequireUnprotected(name, L"remove it");
    StopDistro(distro, out);
    Say(out, L"Unregistering '" + distro.name + L"' and deleting its virtual disk...");
    RunWsl({L"--unregister", distro.name}, out);
    // Forget its protection setting so a future instance with this name starts fresh.
    RegDeleteKeyValueW(HKEY_CURRENT_USER, ProtectionKey().c_str(), distro.name.c_str());
    if (deleteEmptyFolder && RemoveFolderIfEmpty(distro.basePath))
        Say(out, L"Removed the empty folder " + distro.basePath + L".");
    Say(out, L"Removed '" + distro.name + L"'.");
}

void Move(std::wstring_view name, std::wstring_view location, const OutputFn& out) {
    Distro distro = RequireUnprotected(name, L"move it");
    std::wstring target = NormalizeFolder(location);
    if (!distro.basePath.empty() && EqualsIgnoreCase(target, NormalizeFolder(distro.basePath)))
        throw WslError(L"'" + distro.name + L"' is already stored in " + target + L".");
    std::wstring folder = PrepareTargetFolder(target);

    StopDistro(distro, out);
    WaitForDiskRelease(distro.vhdPath, out);
    Say(out, L"Moving '" + distro.name + L"' from " + distro.basePath + L" to " + folder + L"...");
    try {
        RunWsl({L"--manage", distro.name, L"--move", folder}, out);
    } catch (...) {
        RemoveFolderIfEmpty(folder);
        throw;
    }
    if (RemoveFolderIfEmpty(distro.basePath)) Say(out, L"Removed the empty folder " + distro.basePath + L".");
    Say(out, L"Moved '" + distro.name + L"' to " + folder + L".");
}

void Rename(std::wstring_view name, std::wstring_view newName, const OutputFn& out) {
    Distro distro = RequireUnprotected(name, L"rename it");
    ValidateNameSyntax(newName);
    if (!EqualsIgnoreCase(name, newName) && FindDistro(newName))
        throw WslError(L"A WSL instance named '" + std::wstring(newName) + L"' already exists.");
    StopDistro(distro, out);
    WriteRegString(LxssSubKey(distro.id).c_str(), L"DistributionName", newName);
    Say(out, L"Renamed '" + distro.name + L"' to '" + std::wstring(newName) +
                 L"'. Existing Start menu shortcuts and Windows Terminal profiles keep the old name.");
}

void SetDefault(std::wstring_view name, const OutputFn& out) {
    Distro distro = RequireUnprotected(name, L"make it the default instance");
    RunWsl({L"--set-default", distro.name}, out);
    Say(out, L"'" + distro.name + L"' is now the default instance.");
}

void SetDefaultUser(std::wstring_view name, std::wstring_view user, const OutputFn& out) {
    Distro distro = RequireUnprotected(name, L"change its default user");
    if (Trim(user).empty()) throw WslError(L"No user name was given.");
    RunWsl({L"--manage", distro.name, L"--set-default-user", Trim(user)}, out);
    Say(out, L"Default user of '" + distro.name + L"' set to " + Trim(user) + L".");
}

void Terminate(std::wstring_view name, const OutputFn& out) {
    Distro distro = RequireUnprotected(name, L"stop it");
    RunWsl({L"--terminate", distro.name}, out);
    Say(out, L"Stopped '" + distro.name + L"'.");
}

void Shutdown(const OutputFn& out) {
    RunWsl({L"--shutdown"}, out);
    Say(out, L"WSL has been shut down.");
}

void OpenShell(std::wstring_view name) {
    Distro distro = RequireDistro(name);
    LaunchInNewConsole(WslExePath(), {L"--distribution", distro.name, L"--cd", L"~"});
}

}  // namespace wslmgr
