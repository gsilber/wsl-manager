# WSL Manager

A native Windows tool for managing WSL instances. You can create instances, back them up, copy them, move them and remove them. It includes a Win32 GUI (`wslmgr-gui.exe`) and a command-line tool (`wslmgr.exe`), and both use the same core library.

## Features

- **Create** a new instance in a storage folder you choose. It can come from any of these sources:
  - a clean install from the online catalog (`wsl --install`),
  - a backup file (`.tar`, `.tar.gz`, `.tar.xz` or `.vhdx`),
  - a copy of an existing instance. For WSL 2, the tool copies the virtual disk directly instead of exporting and re-importing it.
- **Back up** an instance in any export format. A small `<file>.wslmeta` file is saved next to the backup. When you restore the backup, it brings back the default Linux user, which `wsl --import` would otherwise reset to root.
- **Remove** an instance, with an optional backup first. You can also choose to delete its storage folder if nothing else is left in it.
- **Move** an instance's storage to another folder (`wsl --manage --move`).
- **Other tasks:** rename an instance, set the default instance, set the default user, stop an instance, shut down WSL, and open a shell or the storage folder.

New instances and backups go under a default storage folder. Out of the box this is `%USERPROFILE%\WSL`. Change it under File → Default Storage Folder or with `wslmgr config --default-folder <folder>`. The GUI also remembers the parent folder you used the last time you created an instance.

## Building

You need Visual Studio 2022 or later, or the Build Tools, with the **Desktop development with C++** workload. That workload includes CMake and Ninja.

```bat
build.cmd            &rem Release build -> build\Release\
build.cmd Debug
```

Both executables link the C runtime statically, so they run without installing the VC++ redistributable.

> **Controlled Folder Access:** If Windows Security's ransomware protection guards this folder, it will block the compiler from writing to `build\`. Either allow `cmake.exe`, `ninja.exe`, `cl.exe`, `link.exe` and `rc.exe` through Controlled Folder Access, or build somewhere outside the protected folders:
>
> ```bat
> set WSLMGR_BUILD_DIR=D:\builds\wsl-manager
> build.cmd
> ```

## Testing

```bat
test.cmd              &rem build (MSVC, Debug) and run the tests with ctest
test.cmd --coverage   &rem build with clang-cl and report coverage of src\core and src\cli
```

The tests run the real core and CLI code against `fake_wsl.exe`, a stand-in for `wsl.exe` that simulates WSL in a scratch registry key and temp folders. They never touch your real WSL instances or settings. The first build downloads GoogleTest.

The coverage run needs the Visual Studio component **C++ Clang Compiler for Windows** (Visual Studio Installer → Modify → Individual components). It prints a summary and writes an HTML report to `build\coverage\report\html\index.html`.

For how the code is structured and why, see [docs/design.md](docs/design.md).

> **After a Visual Studio update**, CMake can fail to configure because the build folders still refer to the old compiler version. Delete `build\` and run the script again.

## Command line

```text
wslmgr list [--online]
wslmgr info <name>
wslmgr create <name> --distro <distro> [--web-download] [--location <folder>] [--version 1|2]
wslmgr create <name> --from-backup <file> [--location <folder>] [--version 1|2]
wslmgr create <name> --clone <source> [--location <folder>]
wslmgr backup <name> [--file <path>] [--format vhd|tar|tar.gz|tar.xz] [--force]
wslmgr remove <name> [--backup [--file <path>] [--format <fmt>]] [--delete-folder]
wslmgr move <name> <folder>
wslmgr rename <name> <new-name>
wslmgr set-default <name>
wslmgr set-user <name> <linux-user>
wslmgr stop <name>
wslmgr shell <name>
wslmgr shutdown
wslmgr config [--default-folder <folder>]
```

If an instance is running and the command needs it stopped, `wslmgr` asks for confirmation first. `remove` always asks. Pass `-y` / `--yes` to skip these prompts.

## Notes

- After WSL stops an instance, the WSL VM can keep its virtual disk attached until the VM goes idle, which usually takes under a minute. Move and copy wait for the disk to be released.
- Rename changes the instance's registry entry. Existing Start menu shortcuts and Windows Terminal profiles keep the old name.
- After a clean install, the instance isn't launched. Open a shell on it to create your Linux user.

## Layout

```text
src/core/         WSL operations (registry + wsl.exe), process runner, utilities
src/cli/          wslmgr.exe (Cli.cpp holds the logic; main.cpp just calls it)
src/gui/          wslmgr-gui.exe (main window, dialogs, resources, manifest)
tests/            GoogleTest suite and fake_wsl.exe
docs/             design document
scripts/          shared build-script helpers
```
