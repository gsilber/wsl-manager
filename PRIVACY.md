# Privacy policy for Instance Manager for WSL

_Last updated: November 18, 2026_

Instance Manager for WSL doesn't collect, store or share any personal information. It has no accounts, analytics, telemetry, advertising or crash reporting. It never sends anything about you, or about how you use it, to the developer or to anyone else.

## What the app stores on your PC

Everything the app saves stays on your PC:

- **Settings:** your default storage folder, the folder you last used, your theme and layout choices, and which WSL instances you've marked as protected. These are stored in the Windows registry under `HKCU\Software\WslManager`.
- **Backup details:** when you back up an instance, the app saves a small `.wslmeta` file next to the backup. It records the instance's default Linux user so that restoring the backup can bring it back.
- **WSL's own settings:** when you rename an instance, or change the default instance or default user, the app updates WSL's registration for your Windows account. WSL stores this in the registry under `HKCU\Software\Microsoft\Windows\CurrentVersion\Lxss`.

You can delete the backup files at any time. Removing the app doesn't delete your WSL instances or backups.

## What happens when you uninstall

- **Microsoft Store version:** Windows keeps the app's settings private to the app and deletes them automatically when you uninstall it. The only registry location the Store version can change outside its private storage is WSL's own registration (`HKCU\Software\Microsoft\Windows\CurrentVersion\Lxss`). It changes that only when you rename an instance, or when it restores the default user of an instance you've just restored or copied. Those changes are part of your WSL setup, so they stay after you uninstall, just as if you'd made them with `wsl.exe`. To undo them, rename the instance back or set its default user with `wsl --manage <name> --set-default-user <user>`.
- **Installer (MSI) version:** uninstalling removes the program, its shortcuts and its `PATH` entry, but keeps your settings under `HKCU\Software\WslManager`. To remove them, delete that key in Registry Editor.
- **Instances and backups** you created stay in the folders you chose, in either version. Remove an instance with the app or with `wsl --unregister <name>`, and delete backup files like any other file.

## Network use

The app doesn't connect to the internet itself. To list or install Linux distributions from the online catalog, it asks Windows' own WSL tool (`wsl.exe`) to do so. `wsl.exe` then downloads the catalog and the distributions from Microsoft's servers. Microsoft's privacy statement covers that: https://privacy.microsoft.com/privacystatement

## Contact

If you have questions about this policy, open an issue at https://github.com/gsilber/wsl-manager/issues.

## Changes

If this policy changes, the new version will be posted here with a new date.
