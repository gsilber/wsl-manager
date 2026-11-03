#pragma once

// Core WSL management API shared by the CLI and the GUI. All functions throw
// WslError on failure. Long operations block and report output through OutputFn,
// so callers that need responsiveness should run them on a worker thread.

#include "Util.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wslmgr {

struct Distro {
    std::wstring id;        // registry GUID under HKCU\...\Lxss
    std::wstring name;
    std::wstring basePath;  // storage folder (without the \\?\ prefix)
    std::wstring vhdPath;   // virtual disk file; empty for WSL 1
    int version = 2;        // WSL 1 or 2
    uint32_t defaultUid = 0;
    uint32_t registryState = 1;  // 1 = installed; other values mean install/uninstall in progress
    bool isDefault = false;
    bool isProtected = false;  // see IsProtected()
    bool running = false;
    uint64_t diskBytes = 0;
    std::wstring flavor;
    std::wstring osVersion;
};

struct OnlineDistro {
    std::wstring name;
    std::wstring friendlyName;
};

enum class BackupFormat { Vhd, Tar, TarGz, TarXz };

const wchar_t* FormatArg(BackupFormat format);        // value for `wsl --export --format`
const wchar_t* FormatExtension(BackupFormat format);  // e.g. L".tar.gz"
std::optional<BackupFormat> ParseFormat(std::wstring_view text);
BackupFormat FormatFromPath(std::wstring_view path);  // guessed from the file extension
std::wstring WithBackupExtension(std::wstring_view path, BackupFormat format);
std::wstring StripBackupExtension(std::wstring_view fileName);

std::wstring StateName(const Distro& distro);
std::wstring NormalizeFolder(std::wstring_view path);  // absolute, no trailing separator

// Queries. ListDistros reads the registry; `queryRunning` additionally asks wsl.exe
// which instances are running.
std::vector<Distro> ListDistros(bool queryRunning = true);
std::optional<Distro> FindDistro(std::wstring_view name, bool queryRunning = false);
std::vector<OnlineDistro> ListOnlineDistros();

// Throws if `name` is not a valid, unused instance name.
void ValidateNewName(std::wstring_view name);

// Protection. A protected instance can't be removed, moved, renamed, stopped, made
// the default, given a new default user, or backed up or copied (both stop it);
// those operations throw instead. Instances Docker Desktop manages are protected
// unless the user has explicitly unprotected them.
bool IsProtected(std::wstring_view name);
void SetProtected(std::wstring_view name, bool isProtected);

// Settings: the folder new instances and backups go to by default.
std::wstring GetDefaultRoot();
void SetDefaultRoot(std::wstring_view folder);
std::wstring DefaultLocationFor(std::wstring_view name);
std::wstring DefaultBackupPath(std::wstring_view name, BackupFormat format);
BackupFormat DefaultBackupFormat(const Distro& distro);

// Creation. `location` is the folder that will hold the instance's virtual disk.
struct InstallOptions {
    std::wstring distro;  // name from ListOnlineDistros()
    std::wstring name;
    std::wstring location;
    int version = 2;
    bool webDownload = false;
};
void InstallClean(const InstallOptions& options, const OutputFn& out);
void RestoreBackup(std::wstring_view name, std::wstring_view location, std::wstring_view file, int version,
                   const OutputFn& out);
void Clone(std::wstring_view source, std::wstring_view name, std::wstring_view location, const OutputFn& out);

// Maintenance. Operations that need the instance stopped terminate it first.
void Backup(std::wstring_view name, std::wstring_view file, BackupFormat format, const OutputFn& out);
void Remove(std::wstring_view name, bool deleteEmptyFolder, const OutputFn& out);
void Move(std::wstring_view name, std::wstring_view location, const OutputFn& out);
void Rename(std::wstring_view name, std::wstring_view newName, const OutputFn& out);
void SetDefault(std::wstring_view name, const OutputFn& out);
void SetDefaultUser(std::wstring_view name, std::wstring_view user, const OutputFn& out);
void Terminate(std::wstring_view name, const OutputFn& out);
void Shutdown(const OutputFn& out);
void OpenShell(std::wstring_view name);

}  // namespace wslmgr
