# Instance Manager for WSL

A native Windows tool for managing WSL instances. You can create instances, back them up, copy them, move them and remove them. It includes a Win32 GUI (`wslmgr-gui.exe`) and a command-line tool (`wslmgr.exe`), and both use the same core library.

## Features

- **Create** a new instance in a storage folder you choose. It can come from any of these sources:
  - a clean install from the online catalog (`wsl --install`),
  - a backup file (`.tar`, `.tar.gz`, `.tar.xz` or `.vhdx`),
  - a copy of an existing instance. For WSL 2, the tool copies the virtual disk directly instead of exporting and re-importing it.
- **Back up** an instance in any export format. A small `<file>.wslmeta` file is saved next to the backup. When you restore the backup, it brings back the default Linux user, which `wsl --import` would otherwise reset to root.
- **Remove** an instance, with an optional backup first. You can also choose to delete its storage folder if nothing else is left in it.
- **Move** an instance's storage to another folder (`wsl --manage --move`).
- **Protect** an instance so Instance Manager for WSL won't remove, move, rename, stop, back up or copy it, make it the default, or change its default user. Backing up and copying count as changes because both stop the instance. `docker-desktop` (and the older `docker-desktop-data`) are protected by default, because Docker Desktop manages them. Toggle protection with Home → Protect (or right-click → Protected), or with `wslmgr protect` / `wslmgr unprotect`. You can still view a protected instance and open a shell on it.
- **Other tasks:** rename an instance, set the default instance, set the default user, stop an instance, shut down WSL, and open a shell or the storage folder.

The GUI looks like current Microsoft 365 apps:

- An Office-style ribbon (File, Home, View) with Fluent icons and tooltips.
- The instance list, with a status dot for each instance.
- A details pane for the selected instance.
- A collapsible activity log.

It follows Windows' light or dark app mode. To fix it to one theme, use View → Light or Dark. You can show or hide the details pane and activity log on the View tab too.

New instances and backups go under a default storage folder. Out of the box this is `%USERPROFILE%\WSL`. Change it under File → Default Storage Folder or with `wslmgr config --default-folder <folder>`. The GUI also remembers the parent folder you used the last time you created an instance.

## Installing

Run `WslManager-<version>-x64.msi`. It installs for the current user only and doesn't need administrator rights. It:

- installs both programs to `%LOCALAPPDATA%\Programs\WSL Manager` (the folder keeps the app's original name, so upgrades from earlier versions work),
- adds an **Instance Manager for WSL** shortcut to the Start menu, the desktop, both or neither. You choose on the Shortcuts page; by default only the Start menu gets one, and an upgrade keeps your earlier choice,
- adds the install folder to your user `PATH`, so `wslmgr` works in any new terminal.

Installing a newer version replaces the old one. To uninstall, use Settings → Apps → Installed apps. Uninstalling removes the programs, the shortcuts and the `PATH` entry. It keeps your default storage folder setting, and it never touches your WSL instances or backups.

For a silent install or uninstall: `msiexec /i WslManager-1.0.0-x64.msi /qn` and `msiexec /x WslManager-1.0.0-x64.msi /qn`. To choose the shortcuts in a silent install, add `DESKTOPSHORTCUT=1` for a desktop shortcut, or `STARTMENUSHORTCUT=""` for no Start menu shortcut.

## Building

You need Visual Studio 2022 or later, or the Build Tools, with the **Desktop development with C++** workload. That workload includes CMake and Ninja.

```bat
build.cmd            &rem Release build -> build\Release\
build.cmd Debug
```

Both executables link the C runtime statically, so they run without installing the VC++ redistributable.

The application icon, `src/gui/app.ico`, and the Store logo images in `packaging/msix/Assets` are drawn by `scripts/make-icon.ps1`. To change the icon, edit the script and run `powershell -ExecutionPolicy Bypass -File scripts/make-icon.ps1`, then commit the new files. The build doesn't run the script.

To build the installer and the Microsoft Store package:

```bat
package.cmd          &rem -> build\package\WslManager-<version>-x64.msi and InstanceManagerForWSL-<version>-x64.msix
package.cmd --test   &rem    also InstanceManagerForWSL-<version>-x64-test.msix, signed for testing on this PC
```

This uses the [WiX Toolset](https://wixtoolset.org/) 5, which is pinned in `.config/dotnet-tools.json` and restored automatically. It needs the .NET SDK 8 or later, and the Windows SDK for the MSIX tools. The package version comes from `project(VERSION ...)` in `CMakeLists.txt`, so bump that for each release. `package.cmd` does its own Release build in `build\package`, so it works even while you have the programs in `build\Release` open.

### Microsoft Store package

`scripts/make-msix.ps1` builds the MSIX from `packaging/msix/AppxManifest.xml`. The identity in that manifest, and the publisher in the script, must match Partner Center (Product management → Product identity). The `.msix` is unsigned; upload it as is, and the Store signs it.

The package turns off MSIX's registry and file-system write virtualization (the `unvirtualizedResources` capability). Without that, the app's changes to WSL's registration and new instance folders would go to a private copy that WSL can't see. Partner Center asks you to justify this restricted capability, and `runFullTrust`, when you submit. The `wslmgr` command comes from an app execution alias, because MSIX can't change `PATH`.

To try the package on your own PC before uploading it, run `package.cmd --test`. That signs a copy with a self-signed test certificate, which it creates in your personal certificate store the first time. Trust the certificate once, from an elevated PowerShell, then install:

```powershell
Import-Certificate -FilePath build\package\InstanceManagerForWSL-test.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople
Add-AppxPackage -Path build\package\InstanceManagerForWSL-1.0.0-x64-test.msix
```

Remove it again with `Get-AppxPackage GregSilber.InstanceManagerforWSL | Remove-AppxPackage`. The store version and the MSI can't be installed at the same time without two `wslmgr` commands competing, so uninstall the MSI first.

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
wslmgr protect <name>
wslmgr unprotect <name>
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
src/gui/          wslmgr-gui.exe (main window, dialogs, resources, manifest, icon)
tests/            GoogleTest suite and fake_wsl.exe
installer/        WiX source for the MSI (Package.wxs) and its licence page
docs/             design document
scripts/          shared build-script helpers, and make-icon.ps1 (draws src/gui/app.ico)
```

## License

Copyright (C) 2026 Greg Silber

Instance Manager for WSL is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

Instance Manager for WSL is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

The full text is in [LICENSE](LICENSE). The installer shows it on its licence page and asks you to accept it before installing.
