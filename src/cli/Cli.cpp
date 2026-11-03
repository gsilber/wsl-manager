// wslmgr: command-line front end for the WSL manager core.

#include "cli/Cli.h"

#include "core/Wsl.h"

#include <windows.h>

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace wslmgr;

namespace {

constexpr wchar_t kUsage[] = LR"(WSL Manager - manage Windows Subsystem for Linux instances

Usage: wslmgr <command> [arguments] [options]

Instances
  list [--online]                       List installed instances (or installable distributions).
  info <name>                           Show details about an instance.
  create <name> --distro <distro>       Clean install from the online catalog.
         [--web-download]
  create <name> --from-backup <file>    Restore a backup (.tar, .tar.gz, .tar.xz, .vhdx).
  create <name> --clone <source>        Copy an existing instance.
      create options: --location <folder>  Storage folder (default: <default folder>\<name>)
                      --version <1|2>      WSL version for clean installs and tar restores
  backup <name> [--file <path>] [--format vhd|tar|tar.gz|tar.xz] [--force]
  remove <name> [--backup [--file <path>] [--format <fmt>]] [--delete-folder]
  move <name> <folder>                  Move an instance's storage to another folder.
  rename <name> <new-name>
  set-default <name>                    Make an instance the default for `wsl`.
  set-user <name> <linux-user>          Set the user an instance logs in as.
  stop <name>                           Terminate a running instance.
  shell <name>                          Open a shell on an instance in a new window.
  shutdown                              Stop all instances and the WSL virtual machine.
  protect <name>                        Refuse to remove, move, rename, stop, back up or copy
                                        an instance (docker-desktop is protected by default).
  unprotect <name>                      Allow those changes again.

Settings
  config [--default-folder <folder>]    Show or set the default storage folder.

Options
  -y, --yes     Don't ask for confirmation (e.g. to stop a running instance or to remove).

Copyright (C) 2026 Greg Silber. Licensed under the GNU GPL version 3 or later;
this program comes with ABSOLUTELY NO WARRANTY. See the LICENSE file.
)";

HANDLE StdOut() { return GetStdHandle(STD_OUTPUT_HANDLE); }
HANDLE StdErr() { return GetStdHandle(STD_ERROR_HANDLE); }

bool IsConsole(HANDLE handle) {
    DWORD mode;
    return GetConsoleMode(handle, &mode) != 0;
}

void Write(HANDLE handle, std::wstring_view text) {
    DWORD written;
    if (IsConsole(handle)) {
        WriteConsoleW(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    } else {
        std::string utf8 = WideToUtf8(text);
        WriteFile(handle, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
}

size_t g_progressWidth = 0;

void ClearProgress() {
    if (g_progressWidth == 0) return;
    Write(StdOut(), L"\r" + std::wstring(g_progressWidth, L' ') + L"\r");
    g_progressWidth = 0;
}

void Print(std::wstring_view text) {
    ClearProgress();
    Write(StdOut(), std::wstring(text) + L"\n");
}

void PrintError(std::wstring_view text) {
    ClearProgress();
    Write(StdErr(), std::wstring(text) + L"\n");
}

void OnOutput(const std::wstring& line, bool progress) {
    if (progress) {
        if (!IsConsole(StdOut())) return;
        ClearProgress();
        std::wstring text = Trim(line);
        Write(StdOut(), text);
        g_progressWidth = text.size();
    } else {
        Print(L"  " + Trim(line));
    }
}

const OutputFn kOut = OnOutput;

struct Args {
    std::vector<std::wstring> positional;
    std::map<std::wstring, std::wstring> values;
    std::set<std::wstring> flags;

    bool Has(const wchar_t* flag) const { return flags.count(flag) > 0; }
    std::wstring Value(const wchar_t* name) const {
        auto it = values.find(name);
        return it == values.end() ? L"" : it->second;
    }
};

// Parses options for one command. Unknown options are an error.
Args ParseArgs(int argc, wchar_t** argv, std::initializer_list<const wchar_t*> valueOptions,
               std::initializer_list<const wchar_t*> flagOptions) {
    Args args;
    for (int i = 2; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"-y") arg = L"--yes";
        if (arg.rfind(L"--", 0) != 0) {
            args.positional.push_back(arg);
            continue;
        }
        if (arg == L"--yes" ||
            std::any_of(flagOptions.begin(), flagOptions.end(), [&](const wchar_t* f) { return arg == f; })) {
            args.flags.insert(arg);
        } else if (std::any_of(valueOptions.begin(), valueOptions.end(), [&](const wchar_t* v) { return arg == v; })) {
            if (i + 1 >= argc) throw WslError(L"Option " + arg + L" needs a value.");
            args.values[arg] = argv[++i];
        } else {
            throw WslError(L"Unknown option " + arg + L". Run 'wslmgr help' for usage.");
        }
    }
    return args;
}

void RequirePositional(const Args& args, size_t count, const wchar_t* usage) {
    if (args.positional.size() != count) throw WslError(std::wstring(L"Usage: wslmgr ") + usage);
}

// Reads one line from standard input, which may be a console, a pipe or a file.
// Returns an empty string at end of input or if there is no input at all.
std::wstring ReadLine() {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD read = 0;
    if (IsConsole(in)) {
        wchar_t buffer[256];
        ReadConsoleW(in, buffer, static_cast<DWORD>(std::size(buffer)), &read, nullptr);
        return std::wstring(buffer, read);
    }
    std::string line;
    char c;
    while (ReadFile(in, &c, 1, &read, nullptr) && read == 1 && c != '\n') line.push_back(c);
    return Utf8ToWide(line);
}

bool AskYesNo(const std::wstring& question) {
    Write(StdOut(), question + L" [y/N] ");
    std::wstring answer = Trim(ReadLine());
    return answer == L"y" || answer == L"Y" || answer == L"yes";
}

Distro Require(const std::wstring& name, bool queryRunning = true) {
    auto distro = FindDistro(name, queryRunning);
    if (!distro) throw WslError(L"There is no WSL instance named '" + name + L"'.");
    return *distro;
}

// Operations that need an instance stopped ask first if it is running.
void ConfirmStop(const Distro& distro, const Args& args) {
    if (!distro.running || args.Has(L"--yes")) return;
    if (!AskYesNo(L"'" + distro.name + L"' is running and will be stopped. Continue?"))
        throw WslError(L"Cancelled. Pass --yes to stop the instance without asking.");
}

int ParseVersion(const Args& args) {
    std::wstring v = args.Value(L"--version");
    if (v.empty() || v == L"2") return 2;
    if (v == L"1") return 1;
    throw WslError(L"--version must be 1 or 2.");
}

BackupFormat ParseFormatOption(const Args& args, const Distro& distro) {
    std::wstring text = args.Value(L"--format");
    if (text.empty()) {
        std::wstring file = args.Value(L"--file");
        return file.empty() ? DefaultBackupFormat(distro) : FormatFromPath(file);
    }
    auto format = ParseFormat(text);
    if (!format) throw WslError(L"Unknown format '" + text + L"'. Use vhd, tar, tar.gz or tar.xz.");
    return *format;
}

std::wstring Pad(std::wstring text, size_t width) {
    text.resize(std::max(width, text.size()), L' ');
    return text;
}

int CmdList(int argc, wchar_t** argv) {
    Args args = ParseArgs(argc, argv, {}, {L"--online"});
    if (args.Has(L"--online")) {
        auto online = ListOnlineDistros();
        size_t width = 4;
        for (const auto& d : online) width = std::max(width, d.name.size());
        Print(Pad(L"NAME", width + 2) + L"DESCRIPTION");
        for (const auto& d : online) Print(Pad(d.name, width + 2) + d.friendlyName);
        return 0;
    }

    auto distros = ListDistros();
    if (distros.empty()) {
        Print(L"No WSL instances are installed.");
        return 0;
    }
    size_t nameWidth = 4;
    for (const auto& d : distros) nameWidth = std::max(nameWidth, d.name.size());
    Print(L"  " + Pad(L"NAME", nameWidth + 2) + Pad(L"STATE", 10) + Pad(L"WSL", 5) + Pad(L"PROTECTED", 11) +
          Pad(L"SIZE", 11) + L"LOCATION");
    for (const auto& d : distros) {
        Print((d.isDefault ? L"* " : L"  ") + Pad(d.name, nameWidth + 2) + Pad(StateName(d), 10) +
              Pad(std::to_wstring(d.version), 5) + Pad(d.isProtected ? L"yes" : L"", 11) +
              Pad(d.diskBytes ? FormatBytes(d.diskBytes) : L"-", 11) + d.basePath);
    }
    Print(L"\n* = default instance.   Default storage folder: " + GetDefaultRoot());
    return 0;
}

int CmdInfo(int argc, wchar_t** argv) {
    Args args = ParseArgs(argc, argv, {}, {});
    RequirePositional(args, 1, L"info <name>");
    Distro d = Require(args.positional[0]);
    Print(L"Name:          " + d.name);
    Print(L"State:         " + StateName(d));
    Print(L"WSL version:   " + std::to_wstring(d.version));
    Print(L"Default:       " + std::wstring(d.isDefault ? L"yes" : L"no"));
    Print(L"Protected:     " + std::wstring(d.isProtected ? L"yes" : L"no"));
    Print(L"OS:            " + Trim(d.flavor + L" " + d.osVersion));
    Print(L"Default UID:   " + std::to_wstring(d.defaultUid));
    Print(L"Location:      " + d.basePath);
    if (!d.vhdPath.empty()) Print(L"Virtual disk:  " + d.vhdPath + L" (" + FormatBytes(d.diskBytes) + L")");
    Print(L"Registry ID:   " + d.id);
    return 0;
}

int CmdCreate(int argc, wchar_t** argv) {
    Args args = ParseArgs(argc, argv, {L"--distro", L"--from-backup", L"--clone", L"--location", L"--version"},
                          {L"--web-download"});
    RequirePositional(args, 1, L"create <name> (--distro <d> | --from-backup <file> | --clone <source>) [options]");
    const std::wstring& name = args.positional[0];
    int sources = !args.Value(L"--distro").empty() + !args.Value(L"--from-backup").empty() +
                  !args.Value(L"--clone").empty();
    if (sources != 1) throw WslError(L"Specify exactly one of --distro, --from-backup or --clone.");

    std::wstring location = args.Value(L"--location");
    if (location.empty()) location = DefaultLocationFor(name);

    if (!args.Value(L"--distro").empty()) {
        InstallClean({args.Value(L"--distro"), name, location, ParseVersion(args), args.Has(L"--web-download")}, kOut);
    } else if (!args.Value(L"--from-backup").empty()) {
        RestoreBackup(name, location, args.Value(L"--from-backup"), ParseVersion(args), kOut);
    } else {
        ConfirmStop(Require(args.Value(L"--clone")), args);
        Clone(args.Value(L"--clone"), name, location, kOut);
    }
    return 0;
}

int CmdBackup(int argc, wchar_t** argv) {
    Args args = ParseArgs(argc, argv, {L"--file", L"--format"}, {L"--force"});
    RequirePositional(args, 1, L"backup <name> [--file <path>] [--format <fmt>] [--force]");
    Distro d = Require(args.positional[0]);
    BackupFormat format = ParseFormatOption(args, d);
    std::wstring file = args.Value(L"--file");
    if (file.empty()) file = DefaultBackupPath(d.name, format);
    if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES && !args.Has(L"--force"))
        throw WslError(L"The file " + file + L" already exists. Pass --force to overwrite it.");
    ConfirmStop(d, args);
    Backup(d.name, file, format, kOut);
    return 0;
}

int CmdRemove(int argc, wchar_t** argv) {
    Args args = ParseArgs(argc, argv, {L"--file", L"--format"}, {L"--backup", L"--delete-folder"});
    RequirePositional(args, 1, L"remove <name> [--backup [--file <path>] [--format <fmt>]] [--delete-folder]");
    Distro d = Require(args.positional[0]);
    if (!args.Has(L"--yes")) {
        std::wstring question = L"Permanently delete '" + d.name + L"' and its virtual disk";
        if (d.diskBytes) question += L" (" + FormatBytes(d.diskBytes) + L")";
        question += args.Has(L"--backup") ? L" after backing it up?" : L"? No backup will be made.";
        if (!AskYesNo(question)) throw WslError(L"Cancelled. Pass --yes to remove without asking.");
    }
    if (args.Has(L"--backup")) {
        BackupFormat format = ParseFormatOption(args, d);
        std::wstring file = args.Value(L"--file");
        if (file.empty()) file = DefaultBackupPath(d.name, format);
        Backup(d.name, file, format, kOut);
    }
    Remove(d.name, args.Has(L"--delete-folder"), kOut);
    return 0;
}

int CmdMove(int argc, wchar_t** argv) {
    Args args = ParseArgs(argc, argv, {}, {});
    RequirePositional(args, 2, L"move <name> <folder>");
    ConfirmStop(Require(args.positional[0]), args);
    Move(args.positional[0], args.positional[1], kOut);
    return 0;
}

int CmdRename(int argc, wchar_t** argv) {
    Args args = ParseArgs(argc, argv, {}, {});
    RequirePositional(args, 2, L"rename <name> <new-name>");
    ConfirmStop(Require(args.positional[0]), args);
    Rename(args.positional[0], args.positional[1], kOut);
    return 0;
}

int CmdConfig(int argc, wchar_t** argv) {
    Args args = ParseArgs(argc, argv, {L"--default-folder"}, {});
    if (!args.Value(L"--default-folder").empty()) SetDefaultRoot(args.Value(L"--default-folder"));
    Print(L"Default storage folder: " + GetDefaultRoot());
    return 0;
}

int SimpleCommand(int argc, wchar_t** argv, size_t count, const wchar_t* usage,
                  const std::function<void(const Args&)>& action) {
    Args args = ParseArgs(argc, argv, {}, {});
    RequirePositional(args, count, usage);
    action(args);
    return 0;
}

int Run(int argc, wchar_t** argv) {
    std::wstring command = argc > 1 ? argv[1] : L"help";
    if (command == L"help" || command == L"--help" || command == L"-h" || command == L"/?") {
        Write(StdOut(), kUsage);
        return argc > 1 ? 0 : 1;
    }
    if (command == L"list" || command == L"ls") return CmdList(argc, argv);
    if (command == L"info") return CmdInfo(argc, argv);
    if (command == L"create") return CmdCreate(argc, argv);
    if (command == L"backup") return CmdBackup(argc, argv);
    if (command == L"remove" || command == L"rm") return CmdRemove(argc, argv);
    if (command == L"move") return CmdMove(argc, argv);
    if (command == L"rename") return CmdRename(argc, argv);
    if (command == L"config") return CmdConfig(argc, argv);
    if (command == L"set-default")
        return SimpleCommand(argc, argv, 1, L"set-default <name>", [](const Args& a) { SetDefault(a.positional[0], kOut); });
    if (command == L"set-user")
        return SimpleCommand(argc, argv, 2, L"set-user <name> <linux-user>",
                             [](const Args& a) { SetDefaultUser(a.positional[0], a.positional[1], kOut); });
    if (command == L"stop")
        return SimpleCommand(argc, argv, 1, L"stop <name>", [](const Args& a) { Terminate(a.positional[0], kOut); });
    if (command == L"shell")
        return SimpleCommand(argc, argv, 1, L"shell <name>", [](const Args& a) { OpenShell(a.positional[0]); });
    if (command == L"protect" || command == L"unprotect") {
        bool protect = command == L"protect";
        return SimpleCommand(argc, argv, 1, protect ? L"protect <name>" : L"unprotect <name>", [&](const Args& a) {
            SetProtected(a.positional[0], protect);
            Print(L"'" + Require(a.positional[0], false).name + (protect ? L"' is now protected." : L"' is no longer protected."));
        });
    }
    if (command == L"shutdown")
        return SimpleCommand(argc, argv, 0, L"shutdown", [](const Args&) { Shutdown(kOut); });
    throw WslError(L"Unknown command '" + command + L"'. Run 'wslmgr help' for usage.");
}

}  // namespace

int RunCli(int argc, wchar_t** argv) {
    try {
        return Run(argc, argv);
    } catch (const WslError& e) {
        PrintError(L"error: " + e.message());
    } catch (const std::exception& e) {
        PrintError(L"error: " + Utf8ToWide(e.what()));
    }
    return 1;
}
