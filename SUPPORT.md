# Support for Instance Manager for WSL

Instance Manager for WSL is developed and supported by Greg Silber. This page explains how to get help, report a problem or suggest a feature.

## Getting help

To ask a question, report a bug or request a feature, open an issue:

**https://github.com/gsilber/wsl-manager/issues**

Search the existing issues first, in case someone has already asked. You need a free GitHub account to open an issue.

If you don't have a GitHub account, or would rather not post publicly, email **gsilbercd@gmail.com**.

## Reporting a problem

To help track it down, please include:

- the app version (in the GUI under View → About, or in Settings → Apps → Installed apps),
- how you installed the app: from the Microsoft Store or with the MSI installer,
- your Windows version (run `winver`) and your WSL version (run `wsl --version`),
- what you did, what you expected, and what happened instead,
- the text of any error message, and the lines from the activity log at the bottom of the window. In the command-line tool, copy the output that starts with `error:`.

Leave out anything you'd rather keep private, such as user names or folder paths. The app never sends reports or diagnostics anywhere itself. See the [privacy policy](PRIVACY.md).

## Common questions

**Moving or copying an instance waits for a long time.**
After WSL stops an instance, it can keep the instance's virtual disk attached until the WSL virtual machine goes idle. This usually takes under a minute. The app waits up to two minutes for the disk to be released. If it still times out, choose Home → Shut Down WSL and try again. Shut Down WSL stops every running instance, so the app never does it for you.

**Some commands are greyed out for an instance.**
The instance is probably protected. `docker-desktop` and `docker-desktop-data` are protected by default, because Docker Desktop manages them and changing them outside Docker can break it. To turn protection off, select the instance and choose Home → Protect again, or run `wslmgr unprotect <name>`.

**The list of distributions to install is empty.**
The app reads the catalog from `wsl --list --online`, and it currently only understands English output. On a Windows installation in another language, the list can come back empty. Listing, backing up and managing your existing instances isn't affected.

**After renaming an instance, its Start menu shortcut or Windows Terminal profile still shows the old name.**
Rename changes WSL's own registration, but WSL doesn't update the shortcuts and profiles it created earlier. You can rename or remove them yourself.

**A new instance logs in as root, or asks me to create a user.**
After a clean install, the instance isn't launched, so open a shell on it to create your Linux user. A restored backup gets its default user back from the `.wslmeta` file saved next to the backup. Backups made with other tools don't have that file, so set the default user afterwards with Home → Default User or `wslmgr set-user <name> <user>`.

**The `wslmgr` command isn't found.**
Open a new terminal window after installing. With the Store version, check that the `wslmgr.exe` alias is turned on under Settings → Apps → Advanced app settings → App execution aliases. Don't install the Store version and the MSI at the same time, or there will be two `wslmgr` commands.

## Uninstalling

Uninstall from Settings → Apps → Installed apps. Uninstalling removes the app, but it never removes your WSL instances or backups.

## Source code

Instance Manager for WSL is free software under the GNU General Public License, version 3. The source code is at https://github.com/gsilber/wsl-manager.
