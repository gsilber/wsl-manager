// wslmgr-gui: Win32 front end for the WSL manager core.

#include "Dialogs.h"
#include "core/Wsl.h"
#include "resource.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace wslmgr;
namespace fs = std::filesystem;

namespace {

constexpr wchar_t kAppTitle[] = L"WSL Manager";
constexpr wchar_t kWindowClass[] = L"WslManagerMainWindow";

constexpr UINT WM_APP_OUTPUT = WM_APP + 1;  // wParam: progress flag, lParam: std::wstring*
constexpr UINT WM_APP_DONE = WM_APP + 2;    // lParam: std::wstring* error, or null on success
constexpr UINT WM_APP_LIST = WM_APP + 3;    // lParam: ListResult*
constexpr UINT_PTR kRefreshTimer = 1;
constexpr int kListId = 3001;
constexpr int kLogId = 3002;

struct Command {
    UINT id;
    const wchar_t* buttonLabel;  // null for menu-only commands
    bool needsSelection;
};

constexpr Command kCommands[] = {
    {IDM_NEW, L"New...", false},
    {IDM_CLONE, L"Clone...", true},
    {IDM_BACKUP, L"Back Up...", true},
    {IDM_MOVE, L"Move...", true},
    {IDM_REMOVE, L"Remove...", true},
    {IDM_SETDEFAULT, L"Set Default", true},
    {IDM_SHELL, L"Open Shell", true},
    {IDM_STOP, L"Stop", true},
    {IDM_REFRESH, L"Refresh", false},
    {IDM_OPENFOLDER, nullptr, true},
    {IDM_RENAME, nullptr, true},
    {IDM_DEFAULTUSER, nullptr, true},
    {IDM_PROTECT, nullptr, true},
    {IDM_SHUTDOWN, nullptr, false},
    {IDM_DEFAULTROOT, nullptr, false},
};

// Commands the core refuses for a protected instance; greyed out instead.
constexpr UINT kChangesInstance[] = {IDM_CLONE,      IDM_BACKUP, IDM_MOVE,   IDM_REMOVE,
                                     IDM_SETDEFAULT, IDM_STOP,   IDM_RENAME, IDM_DEFAULTUSER};

enum Column { ColName, ColState, ColVersion, ColDefault, ColProtected, ColSize, ColLocation, ColOs };

struct ColumnInfo {
    const wchar_t* title;
    int width;  // at 96 DPI
    int format;
};
constexpr ColumnInfo kColumns[] = {
    {L"Name", 200, LVCFMT_LEFT},     {L"State", 80, LVCFMT_LEFT},      {L"WSL", 45, LVCFMT_LEFT},
    {L"Default", 60, LVCFMT_LEFT},   {L"Protected", 70, LVCFMT_LEFT},  {L"Disk Size", 85, LVCFMT_RIGHT},
    {L"Location", 300, LVCFMT_LEFT}, {L"OS", 110, LVCFMT_LEFT},
};

struct ListResult {
    std::vector<Distro> distros;
    std::wstring error;
};

struct AppState {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND list = nullptr;
    HWND log = nullptr;
    HWND status = nullptr;
    HWND progress = nullptr;
    std::vector<std::pair<UINT, HWND>> buttons;
    HFONT font = nullptr;
    HFONT monoFont = nullptr;
    UINT dpi = 96;
    HMENU contextMenu = nullptr;

    std::vector<Distro> distros;
    bool busy = false;
    bool refreshing = false;
    bool refreshAgain = false;
    std::wstring operation;
};

AppState g;

int Scale(int value) { return MulDiv(value, static_cast<int>(g.dpi), 96); }

// ---------------------------------------------------------------------------
// Log and status

void AppendLog(const std::wstring& text) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[16];
    swprintf_s(stamp, L"[%02u:%02u:%02u] ", t.wHour, t.wMinute, t.wSecond);

    std::wstring line = stamp;
    for (wchar_t c : text) {
        if (c == L'\n')
            line += L"\r\n           ";
        else if (c != L'\r')
            line += c;
    }
    line += L"\r\n";

    int end = GetWindowTextLengthW(g.log);
    SendMessageW(g.log, EM_SETSEL, end, end);
    SendMessageW(g.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
}

void SetStatus(const std::wstring& text) {
    SendMessageW(g.status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

void ShowError(const std::wstring& message) {
    MessageBoxW(g.window, message.c_str(), kAppTitle, MB_OK | MB_ICONERROR);
}

// ---------------------------------------------------------------------------
// Instance list

const Distro* Selected() {
    int index = ListView_GetNextItem(g.list, -1, LVNI_SELECTED);
    if (index < 0) return nullptr;
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = index;
    if (!ListView_GetItem(g.list, &item)) return nullptr;
    size_t i = static_cast<size_t>(item.lParam);
    return i < g.distros.size() ? &g.distros[i] : nullptr;
}

void SetCell(int row, int column, const std::wstring& text) {
    ListView_SetItemText(g.list, row, column, const_cast<wchar_t*>(text.c_str()));
}

void PopulateList(const std::wstring& selectName) {
    SendMessageW(g.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g.list);
    int selectRow = -1;
    for (size_t i = 0; i < g.distros.size(); ++i) {
        const Distro& d = g.distros[i];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(d.name.c_str());
        item.lParam = static_cast<LPARAM>(i);
        int row = ListView_InsertItem(g.list, &item);
        SetCell(row, ColState, StateName(d));
        SetCell(row, ColVersion, std::to_wstring(d.version));
        SetCell(row, ColDefault, d.isDefault ? L"Yes" : L"");
        SetCell(row, ColProtected, d.isProtected ? L"Yes" : L"");
        SetCell(row, ColSize, d.diskBytes ? FormatBytes(d.diskBytes) : L"");
        SetCell(row, ColLocation, d.basePath);
        SetCell(row, ColOs, Trim(d.flavor + L" " + d.osVersion));
        if (EqualsIgnoreCase(d.name, selectName)) selectRow = row;
    }
    if (selectRow >= 0) {
        ListView_SetItemState(g.list, selectRow, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g.list, selectRow, FALSE);
    }
    SendMessageW(g.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g.list, nullptr, TRUE);
}

void StartRefresh() {
    if (g.refreshing) {
        g.refreshAgain = true;
        return;
    }
    g.refreshing = true;
    HWND window = g.window;
    std::thread([window] {
        auto result = std::make_unique<ListResult>();
        try {
            result->distros = ListDistros(true);
        } catch (const WslError& e) {
            result->error = e.message();
        }
        if (PostMessageW(window, WM_APP_LIST, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
    }).detach();
}

// ---------------------------------------------------------------------------
// Command state

bool IsCommandEnabled(UINT id) {
    if (id == IDM_EXIT || id == IDM_ABOUT) return true;
    // While an operation runs, only harmless commands stay available.
    if (g.busy && id != IDM_REFRESH && id != IDM_SHELL && id != IDM_OPENFOLDER) return false;
    const Distro* selected = Selected();
    if (selected && selected->isProtected && std::find(std::begin(kChangesInstance), std::end(kChangesInstance), id) !=
                                                 std::end(kChangesInstance))
        return false;
    for (const auto& c : kCommands) {
        if (c.id == id) return !c.needsSelection || Selected() != nullptr;
    }
    return true;
}

void UpdateCommandState() {
    for (auto& [id, button] : g.buttons) EnableWindow(button, IsCommandEnabled(id));
}

void SetBusy(bool busy) {
    g.busy = busy;
    ShowWindow(g.progress, busy ? SW_SHOW : SW_HIDE);
    SendMessageW(g.progress, PBM_SETMARQUEE, busy, 30);
    UpdateCommandState();
}

// Runs `work` on a worker thread; its output streams into the log.
void RunOperation(const std::wstring& title, std::function<void(const OutputFn&)> work) {
    if (g.busy) return;
    g.operation = title;
    SetBusy(true);
    SetStatus(title + L"...");
    AppendLog(title + L"...");

    HWND window = g.window;
    std::thread([window, work = std::move(work)] {
        OutputFn out = [window](const std::wstring& line, bool progress) {
            auto text = std::make_unique<std::wstring>(line);
            if (PostMessageW(window, WM_APP_OUTPUT, progress, reinterpret_cast<LPARAM>(text.get()))) text.release();
        };
        std::unique_ptr<std::wstring> error;
        try {
            work(out);
        } catch (const WslError& e) {
            error = std::make_unique<std::wstring>(e.message());
        } catch (const std::exception& e) {
            error = std::make_unique<std::wstring>(Utf8ToWide(e.what()));
        }
        if (PostMessageW(window, WM_APP_DONE, 0, reinterpret_cast<LPARAM>(error.get()))) error.release();
    }).detach();
}

// Operations that need an instance stopped ask first if it is running.
bool ConfirmStop(const Distro& distro, const std::wstring& purpose) {
    if (!distro.running) return true;
    std::wstring question = L"'" + distro.name + L"' is running. It will be stopped to " + purpose +
                            L", which ends any programs running in it.\n\nContinue?";
    return MessageBoxW(g.window, question.c_str(), kAppTitle, MB_YESNO | MB_ICONWARNING) == IDYES;
}

const Distro* FindLoaded(const std::wstring& name) {
    for (const auto& d : g.distros)
        if (EqualsIgnoreCase(d.name, name)) return &d;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Commands

void DoCreate(const std::wstring& cloneSource) {
    auto request = ShowCreateDialog(g.window, g.distros, cloneSource);
    if (!request) return;
    if (request->source == CreateRequest::Source::Clone) {
        const Distro* source = FindLoaded(request->cloneSource);
        if (source && !ConfirmStop(*source, L"copy its disk")) return;
    }

    // Remember the parent folder as the default for next time.
    try {
        fs::path parent = fs::path(NormalizeFolder(request->location)).parent_path();
        if (!parent.empty() && parent != parent.root_path() &&
            !EqualsIgnoreCase(NormalizeFolder(parent.wstring()), NormalizeFolder(GetDefaultRoot())))
            SetDefaultRoot(parent.wstring());
    } catch (const WslError&) {
    }

    CreateRequest r = *request;
    RunOperation(L"Creating '" + r.name + L"'", [r](const OutputFn& out) {
        switch (r.source) {
            case CreateRequest::Source::Clean:
                InstallClean({r.distro, r.name, r.location, r.version, r.webDownload}, out);
                break;
            case CreateRequest::Source::Backup:
                RestoreBackup(r.name, r.location, r.backupFile, r.version, out);
                break;
            case CreateRequest::Source::Clone:
                Clone(r.cloneSource, r.name, r.location, out);
                break;
        }
    });
}

void OnCommand(UINT id) {
    if (!IsCommandEnabled(id)) return;
    const Distro* selected = Selected();
    Distro d = selected ? *selected : Distro{};
    std::wstring name = d.name;

    switch (id) {
        case IDM_NEW:
            DoCreate(L"");
            break;
        case IDM_CLONE:
            DoCreate(name);
            break;
        case IDM_BACKUP: {
            auto request = ShowBackupDialog(g.window, d);
            if (!request || !ConfirmStop(d, L"back it up")) return;
            RunOperation(L"Backing up '" + name + L"'", [name, r = *request](const OutputFn& out) {
                Backup(name, r.file, r.format, out);
            });
            break;
        }
        case IDM_MOVE: {
            auto location = ShowMoveDialog(g.window, d);
            if (!location || !ConfirmStop(d, L"move it")) return;
            RunOperation(L"Moving '" + name + L"'",
                         [name, location = *location](const OutputFn& out) { Move(name, location, out); });
            break;
        }
        case IDM_REMOVE: {
            auto request = ShowRemoveDialog(g.window, d);
            if (!request) return;
            RunOperation(L"Removing '" + name + L"'", [name, r = *request](const OutputFn& out) {
                if (r.backupFirst) Backup(name, r.backup.file, r.backup.format, out);
                Remove(name, r.deleteFolder, out);
            });
            break;
        }
        case IDM_RENAME: {
            auto newName = ShowTextDialog(g.window, L"Rename Instance", L"New name for '" + name + L"':", name);
            if (!newName || *newName == name || !ConfirmStop(d, L"rename it")) return;
            RunOperation(L"Renaming '" + name + L"'",
                         [name, newName = *newName](const OutputFn& out) { Rename(name, newName, out); });
            break;
        }
        case IDM_DEFAULTUSER: {
            auto user = ShowTextDialog(g.window, L"Set Default User",
                                       L"Linux user that '" + name + L"' logs in as by default:", L"");
            if (!user) return;
            RunOperation(L"Setting the default user of '" + name + L"'",
                         [name, user = *user](const OutputFn& out) { SetDefaultUser(name, user, out); });
            break;
        }
        case IDM_PROTECT: {
            bool protect = !d.isProtected;
            if (!protect) {
                std::wstring question = L"Allow WSL Manager to remove, move, rename, stop, back up and copy '" + name +
                                        L"' again?";
                if (EqualsIgnoreCase(name, L"docker-desktop") || EqualsIgnoreCase(name, L"docker-desktop-data"))
                    question += L"\n\nDocker Desktop manages this instance. Changing it outside Docker Desktop "
                                L"can break Docker.";
                if (MessageBoxW(g.window, question.c_str(), kAppTitle, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) !=
                    IDYES)
                    return;
            }
            RunOperation((protect ? L"Protecting '" : L"Unprotecting '") + name + L"'",
                         [name, protect](const OutputFn&) { SetProtected(name, protect); });
            break;
        }
        case IDM_SETDEFAULT:
            RunOperation(L"Setting '" + name + L"' as default", [name](const OutputFn& out) { SetDefault(name, out); });
            break;
        case IDM_STOP:
            RunOperation(L"Stopping '" + name + L"'", [name](const OutputFn& out) { Terminate(name, out); });
            break;
        case IDM_SHUTDOWN:
            if (MessageBoxW(g.window, L"Stop all running WSL instances and the WSL virtual machine?", kAppTitle,
                            MB_YESNO | MB_ICONQUESTION) != IDYES)
                return;
            RunOperation(L"Shutting down WSL", [](const OutputFn& out) { Shutdown(out); });
            break;
        case IDM_SHELL:
            try {
                OpenShell(name);
                AppendLog(L"Opened a shell on '" + name + L"'.");
                SetTimer(g.window, kRefreshTimer, 3000, nullptr);  // pick up the Running state
            } catch (const WslError& e) {
                ShowError(e.message());
            }
            break;
        case IDM_OPENFOLDER:
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(g.window, L"open", d.basePath.c_str(), nullptr, nullptr,
                                                        SW_SHOWNORMAL)) <= 32)
                ShowError(L"Could not open " + d.basePath);
            break;
        case IDM_REFRESH:
            StartRefresh();
            break;
        case IDM_DEFAULTROOT:
            if (auto folder = PickFolder(g.window, L"Default Storage Folder for New Instances", GetDefaultRoot())) {
                try {
                    SetDefaultRoot(*folder);
                    AppendLog(L"Default storage folder set to " + GetDefaultRoot() + L".");
                } catch (const WslError& e) {
                    ShowError(e.message());
                }
            }
            break;
        case IDM_ABOUT:
            MessageBoxW(g.window,
                        (L"WSL Manager 1.0\n\nCreate, back up, copy, move and remove WSL instances.\n\n"
                         L"Default storage folder:\n" + GetDefaultRoot() +
                         L"\n\nCopyright (C) 2026 Greg Silber\n"
                         L"This program is free software, licensed under the GNU General Public License "
                         L"version 3 or later. It comes with ABSOLUTELY NO WARRANTY; see the LICENSE file "
                         L"or https://www.gnu.org/licenses/gpl-3.0.html for details.")
                            .c_str(),
                        kAppTitle, MB_OK | MB_ICONINFORMATION);
            break;
        case IDM_EXIT:
            PostMessageW(g.window, WM_CLOSE, 0, 0);
            break;
    }
}

// ---------------------------------------------------------------------------
// Window setup and layout

void CreateFonts() {
    if (g.font) DeleteObject(g.font);
    if (g.monoFont) DeleteObject(g.monoFont);
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, g.dpi);
    g.font = CreateFontIndirectW(&metrics.lfMessageFont);
    LOGFONTW mono{};
    mono.lfHeight = -MulDiv(9, static_cast<int>(g.dpi), 72);
    mono.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(mono.lfFaceName, L"Consolas");
    g.monoFont = CreateFontIndirectW(&mono);
}

void ApplyFonts() {
    for (auto& [id, button] : g.buttons) SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    SendMessageW(g.list, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    SendMessageW(g.status, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    SendMessageW(g.log, WM_SETFONT, reinterpret_cast<WPARAM>(g.monoFont), TRUE);
}

int TextWidth(HWND control, const wchar_t* text) {
    HDC dc = GetDC(control);
    HGDIOBJ old = SelectObject(dc, g.font);
    SIZE size{};
    GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &size);
    SelectObject(dc, old);
    ReleaseDC(control, dc);
    return size.cx;
}

void Layout() {
    RECT client;
    GetClientRect(g.window, &client);
    SendMessageW(g.status, WM_SIZE, 0, 0);
    RECT statusRect;
    GetWindowRect(g.status, &statusRect);
    int statusHeight = statusRect.bottom - statusRect.top;

    int margin = Scale(8);
    int gap = Scale(6);
    int buttonHeight = Scale(28);
    int width = client.right - 2 * margin;

    int x = margin;
    int y = margin;
    for (auto& [id, button] : g.buttons) {
        wchar_t label[64];
        GetWindowTextW(button, label, 64);
        int buttonWidth = std::max(Scale(72), TextWidth(button, label) + Scale(24));
        if (x > margin && x + buttonWidth > client.right - margin) {
            x = margin;
            y += buttonHeight + gap;
        }
        MoveWindow(button, x, y, buttonWidth, buttonHeight, TRUE);
        x += buttonWidth + gap;
    }
    y += buttonHeight + margin;

    int bottom = client.bottom - statusHeight - margin;
    int logHeight = std::clamp((bottom - y) / 3, Scale(80), Scale(220));
    int listHeight = std::max(Scale(60), bottom - y - logHeight - margin);
    MoveWindow(g.list, margin, y, width, listHeight, TRUE);
    MoveWindow(g.log, margin, y + listHeight + margin, width, std::max(0, bottom - (y + listHeight + margin)), TRUE);

    int parts[] = {std::max(Scale(100), static_cast<int>(client.right) - Scale(180)), -1};
    SendMessageW(g.status, SB_SETPARTS, 2, reinterpret_cast<LPARAM>(parts));
    RECT part;
    SendMessageW(g.status, SB_GETRECT, 1, reinterpret_cast<LPARAM>(&part));
    InflateRect(&part, -Scale(4), -Scale(3));
    MoveWindow(g.progress, part.left, part.top, part.right - part.left, part.bottom - part.top, TRUE);
}

void CreateControls(HWND window) {
    for (const auto& c : kCommands) {
        if (!c.buttonLabel) continue;
        HWND button = CreateWindowExW(0, WC_BUTTONW, c.buttonLabel, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(c.id)),
                                      g.instance, nullptr);
        g.buttons.emplace_back(c.id, button);
    }

    g.list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL |
                                 LVS_SHOWSELALWAYS,
                             0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kListId)), g.instance,
                             nullptr);
    ListView_SetExtendedListViewStyle(g.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SetWindowTheme(g.list, L"Explorer", nullptr);
    for (int i = 0; i < static_cast<int>(std::size(kColumns)); ++i) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        column.pszText = const_cast<wchar_t*>(kColumns[i].title);
        column.cx = Scale(kColumns[i].width);
        column.fmt = kColumns[i].format;
        ListView_InsertColumn(g.list, i, &column);
    }

    g.log = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                                ES_AUTOVSCROLL,
                            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kLogId)), g.instance,
                            nullptr);
    SendMessageW(g.log, EM_SETLIMITTEXT, 0, 0);

    g.status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, window,
                               nullptr, g.instance, nullptr);
    g.progress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | PBS_MARQUEE, 0, 0, 0, 0, g.status, nullptr,
                                 g.instance, nullptr);

    ApplyFonts();
}

void ShowContextMenu(POINT screen) {
    if (screen.x == -1 && screen.y == -1) {  // keyboard: anchor to the selected row
        int index = ListView_GetNextItem(g.list, -1, LVNI_SELECTED);
        RECT rect{};
        if (index >= 0)
            ListView_GetItemRect(g.list, index, &rect, LVIR_LABEL);
        screen = {rect.left, rect.bottom};
        ClientToScreen(g.list, &screen);
    }
    HMENU menu = GetSubMenu(g.contextMenu, 1);  // the "Instance" menu
    SetMenuDefaultItem(menu, IDM_SHELL, FALSE);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, screen.x, screen.y, 0, g.window, nullptr);
}

LRESULT CALLBACK WndProc(HWND window, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g.window = window;
            g.dpi = GetDpiForWindow(window);
            g.contextMenu = LoadMenuW(g.instance, MAKEINTRESOURCEW(IDR_MAINMENU));
            CreateFonts();
            CreateControls(window);
            SetWindowPos(window, nullptr, 0, 0, Scale(1060), Scale(660), SWP_NOMOVE | SWP_NOZORDER);
            Layout();
            UpdateCommandState();
            SetStatus(L"Loading instances...");
            AppendLog(L"WSL Manager started. Default storage folder: " + GetDefaultRoot());
            StartRefresh();
            return 0;
        }

        case WM_SIZE:
            Layout();
            return 0;

        case WM_SETFOCUS:
            SetFocus(g.list);
            return 0;

        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize = {Scale(640), Scale(420)};
            return 0;
        }

        case WM_DPICHANGED: {
            UINT oldDpi = g.dpi;
            g.dpi = HIWORD(wParam);
            CreateFonts();
            ApplyFonts();
            for (int i = 0; i < static_cast<int>(std::size(kColumns)); ++i)
                ListView_SetColumnWidth(g.list, i, MulDiv(ListView_GetColumnWidth(g.list, i), g.dpi, oldDpi));
            const RECT* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            return 0;
        }

        case WM_COMMAND:
            OnCommand(LOWORD(wParam));
            return 0;

        case WM_INITMENUPOPUP: {
            HMENU menu = reinterpret_cast<HMENU>(wParam);
            int count = GetMenuItemCount(menu);
            for (int i = 0; i < count; ++i) {
                UINT id = GetMenuItemID(menu, i);
                if (id != static_cast<UINT>(-1) && id != 0)
                    EnableMenuItem(menu, id, MF_BYCOMMAND | (IsCommandEnabled(id) ? MF_ENABLED : MF_GRAYED));
            }
            const Distro* selected = Selected();
            CheckMenuItem(menu, IDM_PROTECT, MF_BYCOMMAND | (selected && selected->isProtected ? MF_CHECKED : MF_UNCHECKED));
            return 0;
        }

        case WM_CONTEXTMENU:
            if (reinterpret_cast<HWND>(wParam) == g.list && Selected()) {
                ShowContextMenu({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
                return 0;
            }
            break;

        case WM_NOTIFY: {
            auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->hwndFrom != g.list) break;
            switch (header->code) {
                case LVN_ITEMCHANGED:
                    UpdateCommandState();
                    break;
                case NM_DBLCLK:
                case NM_RETURN:
                    OnCommand(IDM_SHELL);
                    break;
                case LVN_KEYDOWN:
                    if (reinterpret_cast<NMLVKEYDOWN*>(lParam)->wVKey == VK_DELETE) OnCommand(IDM_REMOVE);
                    break;
            }
            return 0;
        }

        case WM_TIMER:
            if (wParam == kRefreshTimer) {
                KillTimer(window, kRefreshTimer);
                StartRefresh();
            }
            return 0;

        case WM_APP_OUTPUT: {
            std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring*>(lParam));
            if (wParam)
                SetStatus(g.operation + L": " + Trim(*line));
            else
                AppendLog(L"  " + Trim(*line));
            return 0;
        }

        case WM_APP_DONE: {
            std::unique_ptr<std::wstring> error(reinterpret_cast<std::wstring*>(lParam));
            SetBusy(false);
            if (error) {
                AppendLog(g.operation + L" failed: " + *error);
                SetStatus(g.operation + L" failed.");
                ShowError(*error);
            } else {
                AppendLog(g.operation + L" completed.");
                SetStatus(g.operation + L" completed.");
            }
            StartRefresh();
            return 0;
        }

        case WM_APP_LIST: {
            std::unique_ptr<ListResult> result(reinterpret_cast<ListResult*>(lParam));
            g.refreshing = false;
            const Distro* selected = Selected();
            std::wstring selectName = selected ? selected->name : L"";
            if (result->error.empty()) {
                g.distros = std::move(result->distros);
                PopulateList(selectName);
                if (!g.busy) {
                    size_t running = std::count_if(g.distros.begin(), g.distros.end(),
                                                   [](const Distro& d) { return d.running; });
                    SetStatus(std::to_wstring(g.distros.size()) + L" instances, " + std::to_wstring(running) +
                              L" running.");
                }
            } else {
                AppendLog(L"Could not list instances: " + result->error);
            }
            UpdateCommandState();
            if (g.refreshAgain) {
                g.refreshAgain = false;
                StartRefresh();
            }
            return 0;
        }

        case WM_CLOSE:
            if (g.busy &&
                MessageBoxW(window,
                            (g.operation + L" is still in progress. If you quit now, wsl.exe keeps working in the "
                                           L"background but you won't see the result.\n\nQuit anyway?")
                                .c_str(),
                            kAppTitle, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                return 0;
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            if (g.contextMenu) DestroyMenu(g.contextMenu);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, msg, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS |
                                              ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    g.instance = instance;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszMenuName = MAKEINTRESOURCEW(IDR_MAINMENU);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    HWND window = CreateWindowExW(0, kWindowClass, kAppTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                  CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, showCommand);
    UpdateWindow(window);

    HACCEL accelerators = LoadAcceleratorsW(instance, MAKEINTRESOURCEW(IDR_ACCEL));
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (TranslateAcceleratorW(window, accelerators, &msg)) continue;
        if (IsDialogMessageW(window, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
