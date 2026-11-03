#pragma once

#include "core/Wsl.h"

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

struct CreateRequest {
    enum class Source { Clean, Backup, Clone };
    Source source = Source::Clean;
    std::wstring distro;       // Clean
    bool webDownload = false;  // Clean
    std::wstring backupFile;   // Backup
    std::wstring cloneSource;  // Clone
    std::wstring name;
    std::wstring location;
    int version = 2;
};

struct BackupRequest {
    std::wstring file;
    wslmgr::BackupFormat format = wslmgr::BackupFormat::Vhd;
};

struct RemoveRequest {
    bool backupFirst = true;
    BackupRequest backup;
    bool deleteFolder = true;
};

// Each returns std::nullopt if the user cancels.
std::optional<CreateRequest> ShowCreateDialog(HWND owner, const std::vector<wslmgr::Distro>& existing,
                                              const std::wstring& cloneSource);
std::optional<BackupRequest> ShowBackupDialog(HWND owner, const wslmgr::Distro& distro);
std::optional<std::wstring> ShowMoveDialog(HWND owner, const wslmgr::Distro& distro);
std::optional<RemoveRequest> ShowRemoveDialog(HWND owner, const wslmgr::Distro& distro);
std::optional<std::wstring> ShowTextDialog(HWND owner, const std::wstring& title, const std::wstring& prompt,
                                           const std::wstring& initial);
std::optional<std::wstring> PickFolder(HWND owner, const std::wstring& title, const std::wstring& initial);
