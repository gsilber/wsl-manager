// wslmgr-gui: Win32 front end for the WSL manager core, drawn in a Fluent
// (Microsoft 365 style) look: a tabbed ribbon, an instance list with a details
// pane, and a collapsible activity log. Follows Windows' light/dark setting.

#include "Dialogs.h"
#include "Theme.h"
#include "core/Environment.h"
#include "core/Wsl.h"
#include "resource.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <cmath>
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
constexpr UINT_PTR kProgressTimer = 2;
constexpr int kListId = 3001;
constexpr int kLogId = 3002;

// Code points shared by Segoe Fluent Icons and Segoe MDL2 Assets.
namespace glyph {
constexpr wchar_t Add = 0xE710, Copy = 0xE8C8, Save = 0xE74E, MoveToFolder = 0xE8DE, Delete = 0xE74D,
                  Rename = 0xE8AC, CommandPrompt = 0xE756, FolderOpen = 0xE838, Stop = 0xE71A,
                  FavoriteStar = 0xE734, Contact = 0xE77B, Shield = 0xEA18, Refresh = 0xE72C, Power = 0xE7E8,
                  Folder = 0xE8B7, Info = 0xE946, DockRight = 0xE90D, History = 0xE81C, System = 0xE770,
                  Sunny = 0xE706, Moon = 0xE708, ChevronDown = 0xE70D, ChevronUp = 0xE70E;
}

// ---------------------------------------------------------------------------
// Commands and the ribbon

struct Command {
    UINT id;
    bool needsSelection;
};

constexpr Command kCommands[] = {
    {IDM_NEW, false},       {IDM_CLONE, true},     {IDM_BACKUP, true},      {IDM_MOVE, true},
    {IDM_REMOVE, true},     {IDM_SETDEFAULT, true}, {IDM_SHELL, true},       {IDM_STOP, true},
    {IDM_REFRESH, false},   {IDM_OPENFOLDER, true}, {IDM_RENAME, true},      {IDM_DEFAULTUSER, true},
    {IDM_PROTECT, true},    {IDM_SHUTDOWN, false},  {IDM_DEFAULTROOT, false},
};

// Always available, even while an operation runs.
constexpr UINT kAlwaysEnabled[] = {IDM_EXIT,         IDM_ABOUT,       IDM_TAB_FILE,     IDM_TAB_HOME,
                                   IDM_TAB_VIEW,     IDM_VIEW_DETAILS, IDM_VIEW_LOG,    IDM_THEME_SYSTEM,
                                   IDM_THEME_LIGHT,  IDM_THEME_DARK,  IDM_REFRESH,      IDM_SHELL,
                                   IDM_OPENFOLDER};

// Commands the core refuses for a protected instance; greyed out instead.
constexpr UINT kChangesInstance[] = {IDM_CLONE,      IDM_BACKUP, IDM_MOVE,   IDM_REMOVE,
                                     IDM_SETDEFAULT, IDM_STOP,   IDM_RENAME, IDM_DEFAULTUSER};

template <size_t N>
bool Contains(const UINT (&ids)[N], UINT id) {
    return std::find(std::begin(ids), std::end(ids), id) != std::end(ids);
}

enum class Tab { Home, View };
enum class Tint { Accent, Neutral, Danger };

struct RibbonItem {
    UINT id;
    wchar_t glyph;
    const wchar_t* label;
    bool large;
    Tint tint;
    const wchar_t* tip;
};

struct RibbonGroup {
    Tab tab;
    const wchar_t* label;
    std::vector<RibbonItem> items;
};

const std::vector<RibbonGroup>& Ribbon() {
    static const std::vector<RibbonGroup> groups = {
        {Tab::Home, L"Create",
         {{IDM_NEW, glyph::Add, L"New", true, Tint::Accent,
           L"New instance (Ctrl+N)\nInstall from the online catalog, restore a backup, or copy an instance."},
          {IDM_CLONE, glyph::Copy, L"Copy", true, Tint::Accent,
           L"Copy instance\nCreate a new instance from a copy of the selected one."}}},
        {Tab::Home, L"Run",
         {{IDM_SHELL, glyph::CommandPrompt, L"Open Shell", true, Tint::Accent,
           L"Open shell (Enter)\nOpen a terminal on the selected instance."},
          {IDM_STOP, glyph::Stop, L"Stop", true, Tint::Danger, L"Stop\nStop the selected instance."},
          {IDM_OPENFOLDER, glyph::FolderOpen, L"Open Folder", true, Tint::Accent,
           L"Open folder\nOpen the folder that stores the selected instance."}}},
        {Tab::Home, L"Manage",
         {{IDM_BACKUP, glyph::Save, L"Back Up", true, Tint::Accent,
           L"Back up\nExport the selected instance to a backup file."},
          {IDM_MOVE, glyph::MoveToFolder, L"Move", true, Tint::Accent,
           L"Move storage\nMove the selected instance's virtual disk to another folder."},
          {IDM_REMOVE, glyph::Delete, L"Remove", true, Tint::Danger,
           L"Remove (Del)\nUnregister the selected instance and delete its disk, optionally backing it up first."}}},
        {Tab::Home, L"Settings",
         {{IDM_RENAME, glyph::Rename, L"Rename", false, Tint::Neutral, L"Rename the selected instance."},
          {IDM_SETDEFAULT, glyph::FavoriteStar, L"Set as Default", false, Tint::Neutral,
           L"Make the selected instance the one wsl.exe starts by default."},
          {IDM_DEFAULTUSER, glyph::Contact, L"Default User", false, Tint::Neutral,
           L"Choose the Linux user the selected instance logs in as."},
          {IDM_PROTECT, glyph::Shield, L"Protect", true, Tint::Accent,
           L"Protect\nPrevent WSL Manager from changing, stopping or removing the selected instance."}}},
        {Tab::Home, L"WSL",
         {{IDM_REFRESH, glyph::Refresh, L"Refresh", false, Tint::Neutral, L"Refresh the instance list (F5)."},
          {IDM_SHUTDOWN, glyph::Power, L"Shut Down WSL", false, Tint::Neutral,
           L"Stop all instances and the WSL virtual machine."},
          {IDM_DEFAULTROOT, glyph::Folder, L"Storage Folder", false, Tint::Neutral,
           L"Choose the default folder for new instances and backups."}}},
        {Tab::View, L"Show",
         {{IDM_VIEW_DETAILS, glyph::DockRight, L"Details Pane", true, Tint::Accent,
           L"Show or hide the details of the selected instance."},
          {IDM_VIEW_LOG, glyph::History, L"Activity Log", true, Tint::Accent,
           L"Show or hide the log of WSL Manager's operations."}}},
        {Tab::View, L"Theme",
         {{IDM_THEME_SYSTEM, glyph::System, L"Use Windows Setting", false, Tint::Neutral,
           L"Follow the light or dark app mode chosen in Windows Settings."},
          {IDM_THEME_LIGHT, glyph::Sunny, L"Light", false, Tint::Neutral, L"Always use the light theme."},
          {IDM_THEME_DARK, glyph::Moon, L"Dark", false, Tint::Neutral, L"Always use the dark theme."}}},
        {Tab::View, L"Help",
         {{IDM_ABOUT, glyph::Info, L"About", true, Tint::Accent, L"Version, licence and the default storage folder."}}},
    };
    return groups;
}

enum Column { ColName, ColState, ColVersion, ColDefault, ColProtected, ColSize, ColLocation, ColOs };

struct ColumnInfo {
    const wchar_t* title;
    int width;  // at 96 DPI
    int format;
};
constexpr ColumnInfo kColumns[] = {
    {L"Name", 190, LVCFMT_LEFT},     {L"State", 80, LVCFMT_LEFT},      {L"WSL", 45, LVCFMT_LEFT},
    {L"Default", 60, LVCFMT_LEFT},   {L"Protected", 72, LVCFMT_LEFT},  {L"Disk Size", 80, LVCFMT_RIGHT},
    {L"Location", 280, LVCFMT_LEFT}, {L"OS", 110, LVCFMT_LEFT},
};

enum StateImage { ImageStopped, ImageRunning, ImageOther };

struct ListResult {
    std::vector<Distro> distros;
    std::wstring error;
};

struct RibbonButton {
    const RibbonItem* item;
    Tab tab;
    HWND hwnd;
    bool compact;  // drawn as a small (icon + label in a row) button in the current layout
};

struct GroupLabel {
    RECT rect;
    const wchar_t* text;
};

struct AppState {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND list = nullptr;
    HWND log = nullptr;
    HWND tooltip = nullptr;
    HWND tabFile = nullptr, tabHome = nullptr, tabView = nullptr;
    HWND expander = nullptr;  // "Activity" toggle above the log
    std::vector<RibbonButton> ribbon;
    std::vector<GroupLabel> groupLabels;
    HMENU menus = nullptr;
    HIMAGELIST stateImages = nullptr;

    HFONT font = nullptr, semibold = nullptr, title = nullptr, smallFont = nullptr, mono = nullptr;
    HFONT iconLarge = nullptr, iconSmall = nullptr;
    UINT dpi = 96;

    Tab tab = Tab::Home;
    bool showDetails = true;
    bool showLog = true;
    RECT ribbonRect{}, progressRect{}, listCard{}, detailsCard{}, logCard{}, statusRect{};
    std::wstring statusText;
    double progressPhase = 0;

    std::vector<Distro> distros;
    bool loaded = false;
    bool busy = false;
    bool refreshing = false;
    bool refreshAgain = false;
    std::wstring operation;
};

AppState g;

int Scale(int value) { return MulDiv(value, static_cast<int>(g.dpi), 96); }

// ---------------------------------------------------------------------------
// Settings (HKCU\Software\WslManager\Gui)

std::wstring GuiKey() { return CurrentEnvironment().settingsKey + L"\\Gui"; }

DWORD ReadGuiSetting(const wchar_t* name, DWORD fallback) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, GuiKey().c_str(), name, RRF_RT_REG_DWORD, nullptr, &value, &size) !=
        ERROR_SUCCESS)
        return fallback;
    return value;
}

void WriteGuiSetting(const wchar_t* name, DWORD value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, GuiKey().c_str(), name, REG_DWORD, &value, sizeof(value));
}

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
    g.statusText = text;
    InvalidateRect(g.window, &g.statusRect, FALSE);
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

StateImage ImageFor(const Distro& d) {
    if (d.running) return ImageRunning;
    return d.registryState == 1 ? ImageStopped : ImageOther;
}

COLORREF StateColor(const Distro& d) {
    const auto& c = ui::Colors();
    switch (ImageFor(d)) {
        case ImageRunning: return c.success;
        case ImageOther: return c.warning;
        default: return c.textSecondary;
    }
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
        item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(d.name.c_str());
        item.lParam = static_cast<LPARAM>(i);
        item.iImage = ImageFor(d);
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
    if (selectRow < 0 && !g.distros.empty() && selectName.empty()) selectRow = 0;
    if (selectRow >= 0) {
        ListView_SetItemState(g.list, selectRow, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g.list, selectRow, FALSE);
    }
    SendMessageW(g.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g.list, nullptr, TRUE);
    InvalidateRect(g.window, &g.detailsCard, FALSE);
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

// Small anti-aliased status dots for the list, one per StateImage. The image
// height also sets the row height, giving the list roomier rows.
HIMAGELIST MakeStateImages() {
    const auto& c = ui::Colors();
    int width = Scale(16), height = Scale(30);
    HIMAGELIST images = ImageList_Create(width, height, ILC_COLOR32, 3, 0);
    COLORREF colors[] = {c.textSecondary, c.success, c.warning};
    float radius = Scale(8) / 2.0f;
    float cx = width / 2.0f, cy = height / 2.0f;
    for (COLORREF color : colors) {
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        auto* pixels = static_cast<DWORD*>(bits);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
                float a = std::clamp(radius + 0.5f - d, 0.0f, 1.0f);
                auto pm = [a](int channel) { return static_cast<DWORD>(channel * a + 0.5f); };
                pixels[y * width + x] = (static_cast<DWORD>(a * 255 + 0.5f) << 24) | (pm(GetRValue(color)) << 16) |
                                        (pm(GetGValue(color)) << 8) | pm(GetBValue(color));
            }
        }
        ImageList_Add(images, bitmap, nullptr);
        DeleteObject(bitmap);
    }
    return images;
}

// ---------------------------------------------------------------------------
// Command state

bool IsCommandEnabled(UINT id) {
    if (Contains(kAlwaysEnabled, id) && id != IDM_SHELL && id != IDM_OPENFOLDER) return true;
    // While an operation runs, only harmless commands stay available.
    if (g.busy && !Contains(kAlwaysEnabled, id)) return false;
    const Distro* selected = Selected();
    if (selected && selected->isProtected && Contains(kChangesInstance, id)) return false;
    for (const auto& c : kCommands) {
        if (c.id == id) return !c.needsSelection || selected != nullptr;
    }
    return true;
}

bool IsCommandChecked(UINT id) {
    switch (id) {
        case IDM_PROTECT: {
            const Distro* selected = Selected();
            return selected && selected->isProtected;
        }
        case IDM_VIEW_DETAILS: return g.showDetails;
        case IDM_VIEW_LOG: return g.showLog;
        case IDM_THEME_SYSTEM: return ui::GetThemeMode() == ui::ThemeMode::System;
        case IDM_THEME_LIGHT: return ui::GetThemeMode() == ui::ThemeMode::Light;
        case IDM_THEME_DARK: return ui::GetThemeMode() == ui::ThemeMode::Dark;
    }
    return false;
}

void UpdateCommandState() {
    for (auto& b : g.ribbon) {
        EnableWindow(b.hwnd, IsCommandEnabled(b.item->id));
        InvalidateRect(b.hwnd, nullptr, FALSE);  // checked state may have changed too
    }
    for (HWND w : {g.tabFile, g.tabHome, g.tabView, g.expander}) InvalidateRect(w, nullptr, FALSE);
    InvalidateRect(g.window, &g.detailsCard, FALSE);
}

void SetBusy(bool busy) {
    g.busy = busy;
    g.progressPhase = 0;
    if (busy)
        SetTimer(g.window, kProgressTimer, 16, nullptr);
    else
        KillTimer(g.window, kProgressTimer);
    InvalidateRect(g.window, &g.progressRect, FALSE);
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
// Theme

void ApplyTheme() {
    const auto& c = ui::Colors();
    ui::ApplyWindowFrame(g.window, c.chrome);
    ui::ApplyControlTheme(g.list, L"Explorer", L"DarkMode_Explorer");
    ui::ApplyControlTheme(ListView_GetHeader(g.list), nullptr, L"DarkMode_ItemsView");
    ui::ApplyControlTheme(g.log, L"Explorer", L"DarkMode_Explorer");
    ui::ApplyControlTheme(g.tooltip, nullptr, L"DarkMode_Explorer");
    ListView_SetBkColor(g.list, c.card);
    ListView_SetTextBkColor(g.list, c.card);
    ListView_SetTextColor(g.list, c.text);
    HIMAGELIST old = g.stateImages;
    g.stateImages = MakeStateImages();
    ListView_SetImageList(g.list, g.stateImages, LVSIL_SMALL);
    if (old) ImageList_Destroy(old);
    ui::FlushMenus();
    RedrawWindow(g.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
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

void Layout();

void SelectTab(Tab tab) {
    g.tab = tab;
    Layout();
}

void ChangeThemeMode(ui::ThemeMode mode) {
    ui::SetThemeMode(mode);
    WriteGuiSetting(L"Theme", static_cast<DWORD>(mode));
    ApplyTheme();
}

void ShowFileMenu() {
    RECT r;
    GetWindowRect(g.tabFile, &r);
    TrackPopupMenu(GetSubMenu(g.menus, 0), TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, g.window, nullptr);
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
        case IDM_TAB_FILE:
            ShowFileMenu();
            break;
        case IDM_TAB_HOME:
            SelectTab(Tab::Home);
            break;
        case IDM_TAB_VIEW:
            SelectTab(Tab::View);
            break;
        case IDM_VIEW_DETAILS:
            g.showDetails = !g.showDetails;
            WriteGuiSetting(L"DetailsPane", g.showDetails);
            Layout();
            break;
        case IDM_VIEW_LOG:
            g.showLog = !g.showLog;
            WriteGuiSetting(L"ActivityLog", g.showLog);
            Layout();
            break;
        case IDM_THEME_SYSTEM:
            ChangeThemeMode(ui::ThemeMode::System);
            break;
        case IDM_THEME_LIGHT:
            ChangeThemeMode(ui::ThemeMode::Light);
            break;
        case IDM_THEME_DARK:
            ChangeThemeMode(ui::ThemeMode::Dark);
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
    UpdateCommandState();
}

// ---------------------------------------------------------------------------
// Fonts and layout

void CreateFonts() {
    for (HFONT f : {g.font, g.semibold, g.title, g.smallFont, g.mono, g.iconLarge, g.iconSmall})
        if (f) DeleteObject(f);
    g.font = ui::CreateUiFont(g.dpi, 9);
    g.semibold = ui::CreateUiFont(g.dpi, 9, FW_SEMIBOLD);
    g.title = ui::CreateUiFont(g.dpi, 15, FW_SEMIBOLD);
    g.smallFont = ui::CreateUiFont(g.dpi, 8);
    g.iconLarge = ui::CreateIconFont(Scale(24));
    g.iconSmall = ui::CreateIconFont(Scale(16));
    LOGFONTW mono{};
    mono.lfHeight = -MulDiv(9, static_cast<int>(g.dpi), 72);
    mono.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(mono.lfFaceName, L"Cascadia Mono");
    g.mono = CreateFontIndirectW(&mono);
    // Fall back to Consolas where Cascadia Mono isn't installed.
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, g.mono);
    wchar_t face[LF_FACESIZE];
    GetTextFaceW(dc, LF_FACESIZE, face);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    if (_wcsicmp(face, L"Cascadia Mono") != 0) {
        DeleteObject(g.mono);
        wcscpy_s(mono.lfFaceName, L"Consolas");
        g.mono = CreateFontIndirectW(&mono);
    }
}

void ApplyFonts() {
    SendMessageW(g.list, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    SendMessageW(g.log, WM_SETFONT, reinterpret_cast<WPARAM>(g.mono), TRUE);
    SendMessageW(g.tooltip, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    SendMessageW(g.tooltip, TTM_SETMAXTIPWIDTH, 0, Scale(320));
}

int TextWidth(HFONT font, const wchar_t* text) {
    HDC dc = GetDC(g.window);
    HGDIOBJ old = SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &size);
    SelectObject(dc, old);
    ReleaseDC(g.window, dc);
    return size.cx;
}

RibbonButton* FindButton(HWND hwnd) {
    for (auto& b : g.ribbon)
        if (b.hwnd == hwnd) return &b;
    return nullptr;
}

constexpr int kRibbonContent = 68;  // height of the button area, at 96 DPI
constexpr int kSmallButton = 22;

// Lays out the current tab's groups. In compact mode every button is small,
// stacked three to a column, for narrow windows. Returns the width needed.
int LayoutRibbon(bool compact, bool apply) {
    int left = g.ribbonRect.left;
    int x = left + Scale(6);
    int top = g.ribbonRect.top + Scale(6);
    int contentH = Scale(kRibbonContent);
    int smallH = Scale(kSmallButton);
    int smallGap = (contentH - 3 * smallH) / 2;
    if (apply) g.groupLabels.clear();

    for (const auto& group : Ribbon()) {
        if (group.tab != g.tab) continue;
        int groupLeft = x;
        std::vector<RibbonButton*> column;
        auto flush = [&] {
            if (column.empty()) return;
            int width = 0;
            for (auto* b : column)
                width = std::max(width, Scale(30) + TextWidth(g.font, b->item->label) + Scale(10));
            int y = top;
            for (auto* b : column) {
                if (apply) {
                    b->compact = true;
                    MoveWindow(b->hwnd, x, y, width, smallH, FALSE);
                }
                y += smallH + smallGap;
            }
            x += width + Scale(2);
            column.clear();
        };
        for (const auto& item : group.items) {
            RibbonButton* b = nullptr;
            for (auto& candidate : g.ribbon)
                if (candidate.item == &item) b = &candidate;
            if (!item.large || compact) {
                column.push_back(b);
                if (column.size() == 3) flush();
            } else {
                flush();
                int width = std::max(Scale(56), TextWidth(g.font, item.label) + Scale(18));
                if (apply) {
                    b->compact = false;
                    MoveWindow(b->hwnd, x, top, width, contentH, FALSE);
                }
                x += width + Scale(2);
            }
        }
        flush();
        x -= Scale(2);
        x = std::max(x, groupLeft + TextWidth(g.smallFont, group.label) + Scale(12));
        if (apply)
            g.groupLabels.push_back({{groupLeft, top + contentH, x, g.ribbonRect.bottom - Scale(3)}, group.label});
        x += Scale(17);
    }
    return x - Scale(17) + Scale(6) - left;
}

void Layout() {
    RECT client;
    GetClientRect(g.window, &client);
    int width = client.right, height = client.bottom;
    int m = Scale(8);

    // Tabs
    int tabX = m;
    for (HWND tab : {g.tabFile, g.tabHome, g.tabView}) {
        wchar_t text[32];
        GetWindowTextW(tab, text, 32);
        int w = TextWidth(g.semibold, text) + Scale(28);
        MoveWindow(tab, tabX, Scale(3), w, Scale(32), FALSE);
        tabX += w + Scale(2);
    }

    // Ribbon
    g.ribbonRect = {m, Scale(38), width - m, Scale(38) + Scale(6 + kRibbonContent + 22)};
    bool compact = LayoutRibbon(false, false) > g.ribbonRect.right - g.ribbonRect.left;
    LayoutRibbon(compact, true);
    for (auto& b : g.ribbon) ShowWindow(b.hwnd, b.tab == g.tab ? SW_SHOWNA : SW_HIDE);

    g.progressRect = {m + Scale(8), g.ribbonRect.bottom + Scale(3), width - m - Scale(8), g.ribbonRect.bottom + Scale(6)};
    g.statusRect = {0, height - Scale(28), width, height};

    // Activity log and its expander
    int top = g.ribbonRect.bottom + Scale(10);
    int bottom = g.statusRect.top;
    int logHeight = g.showLog ? std::clamp((bottom - top) / 4, Scale(80), Scale(200)) : 0;
    g.logCard = {m, bottom - logHeight, width - m, bottom};
    int expanderW = Scale(34) + TextWidth(g.semibold, L"Activity") + Scale(12);
    int expanderTop = g.logCard.top - Scale(32);
    MoveWindow(g.expander, m, expanderTop + Scale(2), expanderW, Scale(28), FALSE);
    if (g.showLog) {
        int inset = Scale(6);
        MoveWindow(g.log, g.logCard.left + inset, g.logCard.top + inset, g.logCard.right - g.logCard.left - 2 * inset,
                   std::max(0, logHeight - 2 * inset), FALSE);
    }
    ShowWindow(g.log, g.showLog ? SW_SHOWNA : SW_HIDE);

    // Instance list and details pane
    int contentBottom = std::max(top + Scale(60), expanderTop);
    int detailsW = g.showDetails && width > Scale(760) ? Scale(300) : 0;
    g.listCard = {m, top, width - m - (detailsW ? detailsW + m : 0), contentBottom};
    g.detailsCard = detailsW ? RECT{width - m - detailsW, top, width - m, contentBottom} : RECT{};
    int pad = Scale(4);
    MoveWindow(g.list, g.listCard.left + pad, g.listCard.top + pad, g.listCard.right - g.listCard.left - 2 * pad,
               g.listCard.bottom - g.listCard.top - 2 * pad, FALSE);

    RedrawWindow(g.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

// ---------------------------------------------------------------------------
// Painting

void PaintDetails(HDC dc, const RECT& card) {
    const auto& c = ui::Colors();
    int pad = Scale(18);
    RECT area = {card.left + pad, card.top + pad, card.right - pad, card.bottom - pad};
    int saved = SaveDC(dc);
    IntersectClipRect(dc, card.left + 1, card.top + 1, card.right - 1, card.bottom - 1);
    SetBkMode(dc, TRANSPARENT);

    const Distro* d = Selected();
    if (!d) {
        SelectObject(dc, g.font);
        SetTextColor(dc, c.textSecondary);
        RECT r = {area.left, area.top + Scale(24), area.right, area.bottom};
        DrawTextW(dc, g.loaded ? L"Select an instance to see its details." : L"Loading instances...", -1, &r,
                  DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        RestoreDC(dc, saved);
        return;
    }

    int y = area.top;
    SelectObject(dc, g.title);
    SetTextColor(dc, c.text);
    RECT title = {area.left, y, area.right, y + Scale(30)};
    DrawTextW(dc, d->name.c_str(), -1, &title, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    y += Scale(32);

    // State line: dot, state, version, default
    ui::FillCircle(dc, static_cast<float>(area.left + Scale(4)), static_cast<float>(y + Scale(10)), Scale(8) / 2.0f, StateColor(*d));
    std::wstring line = StateName(*d) + L"  ·  WSL " + std::to_wstring(d->version);
    if (d->isDefault) line += L"  ·  Default";
    SelectObject(dc, g.font);
    SetTextColor(dc, c.textSecondary);
    RECT state = {area.left + Scale(16), y, area.right, y + Scale(20)};
    DrawTextW(dc, line.c_str(), -1, &state, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    y += Scale(32);

    if (d->isProtected) {
        const wchar_t* message = L"Protected. WSL Manager won't change, stop or remove this instance.";
        int textLeft = area.left + Scale(36);
        RECT measure = {textLeft, 0, area.right - Scale(12), 0};
        DrawTextW(dc, message, -1, &measure, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        int barH = (measure.bottom - measure.top) + Scale(20);
        RECT bar = {area.left, y, area.right, y + barH};
        ui::FillRound(dc, bar, Scale(4), c.infoBack);
        RECT icon = {area.left + Scale(8), y, area.left + Scale(30), y + barH};
        ui::DrawGlyph(dc, g.iconSmall, glyph::Shield, icon, c.accentText);
        SelectObject(dc, g.font);
        SetTextColor(dc, c.text);
        RECT text = {textLeft, y + Scale(10), area.right - Scale(12), bar.bottom - Scale(10)};
        DrawTextW(dc, message, -1, &text, DT_WORDBREAK | DT_NOPREFIX);
        y = bar.bottom + Scale(16);
    }

    RECT divider = {area.left, y, area.right, y + 1};
    FillRect(dc, &divider, ui::Brush(c.divider));
    y += Scale(14);

    std::wstring os = Trim(d->flavor + L" " + d->osVersion);
    std::wstring vhd = d->vhdPath.empty() ? L"None (WSL 1 stores files directly)"
                                          : fs::path(d->vhdPath).filename().wstring();
    std::pair<const wchar_t*, std::wstring> properties[] = {
        {L"Operating system", os.empty() ? L"Unknown" : os},
        {L"Disk size", d->diskBytes ? FormatBytes(d->diskBytes) : L"Unknown"},
        {L"Storage folder", d->basePath},
        {L"Virtual disk", vhd},
        {L"Default user ID", std::to_wstring(d->defaultUid)},
        {L"Registry ID", d->id},
    };
    for (const auto& [label, value] : properties) {
        if (y > area.bottom) break;
        SelectObject(dc, g.smallFont);
        SetTextColor(dc, c.textSecondary);
        RECT l = {area.left, y, area.right, y + Scale(16)};
        DrawTextW(dc, label, -1, &l, DT_SINGLELINE | DT_NOPREFIX);
        y += Scale(17);
        SelectObject(dc, g.font);
        SetTextColor(dc, c.text);
        RECT v = {area.left, y, area.right, y};
        DrawTextW(dc, value.c_str(), -1, &v, DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
        v.right = area.right;
        DrawTextW(dc, value.c_str(), -1, &v, DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
        y = v.bottom + Scale(12);
    }
    RestoreDC(dc, saved);
}

void PaintProgress(HDC dc) {
    if (!g.busy) return;
    const auto& c = ui::Colors();
    int width = g.progressRect.right - g.progressRect.left;
    int segment = std::max(Scale(60), width * 3 / 10);
    int x = g.progressRect.left - segment + static_cast<int>((width + segment) * g.progressPhase);
    int saved = SaveDC(dc);
    IntersectClipRect(dc, g.progressRect.left, g.progressRect.top, g.progressRect.right, g.progressRect.bottom);
    RECT bar = {x, g.progressRect.top, x + segment, g.progressRect.bottom};
    ui::FillRound(dc, bar, Scale(2), c.accentText);
    RestoreDC(dc, saved);
}

void Paint(HDC dc, const RECT& client) {
    const auto& c = ui::Colors();
    FillRect(dc, &client, ui::Brush(c.chrome));
    SetBkMode(dc, TRANSPARENT);

    // Ribbon card, group captions and dividers
    ui::FillRound(dc, g.ribbonRect, Scale(8), c.card, c.border);
    SelectObject(dc, g.smallFont);
    SetTextColor(dc, c.textSecondary);
    for (size_t i = 0; i < g.groupLabels.size(); ++i) {
        RECT r = g.groupLabels[i].rect;
        DrawTextW(dc, g.groupLabels[i].text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (i + 1 < g.groupLabels.size()) {
            int x = r.right + Scale(8);
            RECT line = {x, g.ribbonRect.top + Scale(10), x + 1, g.ribbonRect.bottom - Scale(10)};
            FillRect(dc, &line, ui::Brush(c.divider));
        }
    }
    PaintProgress(dc);

    ui::FillRound(dc, g.listCard, Scale(8), c.card, c.border);
    if (g.detailsCard.right > g.detailsCard.left) {
        ui::FillRound(dc, g.detailsCard, Scale(8), c.card, c.border);
        PaintDetails(dc, g.detailsCard);
    }
    if (g.showLog) ui::FillRound(dc, g.logCard, Scale(8), c.card, c.border);

    SelectObject(dc, g.font);
    SetTextColor(dc, c.textSecondary);
    RECT status = g.statusRect;
    status.left += Scale(14);
    status.right -= Scale(14);
    DrawTextW(dc, g.statusText.c_str(), -1, &status, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

// Ribbon buttons, tabs and the activity expander are real BUTTON controls (so
// they work with screen readers and get hot tracking) painted here.
LRESULT DrawRibbonButton(NMCUSTOMDRAW* cd, const RibbonButton& b) {
    const auto& c = ui::Colors();
    HDC dc = cd->hdc;
    RECT r = cd->rc;
    bool hot = cd->uItemState & CDIS_HOT;
    bool pressed = cd->uItemState & CDIS_SELECTED;
    bool disabled = cd->uItemState & CDIS_DISABLED;
    bool checked = IsCommandChecked(b.item->id);

    FillRect(dc, &r, ui::Brush(c.card));
    COLORREF fill = CLR_NONE;
    if (checked)
        fill = disabled ? c.hover : pressed ? c.pressed : hot ? ui::Blend(c.checked, c.text, 0.05) : c.checked;
    else if (!disabled && pressed)
        fill = c.pressed;
    else if (!disabled && hot)
        fill = c.hover;
    if (fill != CLR_NONE) ui::FillRound(dc, r, Scale(4), fill);

    COLORREF glyphColor = disabled                       ? c.textDisabled
                          : b.item->tint == Tint::Danger  ? c.danger
                          : b.item->tint == Tint::Neutral ? c.text
                                                          : c.accentText;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? c.textDisabled : c.text);
    if (b.compact) {
        RECT icon = {r.left + Scale(4), r.top, r.left + Scale(26), r.bottom};
        ui::DrawGlyph(dc, g.iconSmall, b.item->glyph, icon, glyphColor);
        SelectObject(dc, g.font);
        SetTextColor(dc, disabled ? c.textDisabled : c.text);
        RECT label = {r.left + Scale(30), r.top, r.right - Scale(4), r.bottom};
        DrawTextW(dc, b.item->label, -1, &label, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    } else {
        RECT icon = {r.left, r.top + Scale(6), r.right, r.top + Scale(40)};
        ui::DrawGlyph(dc, g.iconLarge, b.item->glyph, icon, glyphColor);
        SelectObject(dc, g.font);
        SetTextColor(dc, disabled ? c.textDisabled : c.text);
        RECT label = {r.left + Scale(2), r.top + Scale(42), r.right - Scale(2), r.bottom - Scale(4)};
        DrawTextW(dc, b.item->label, -1, &label, DT_SINGLELINE | DT_CENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    return CDRF_SKIPDEFAULT;
}

LRESULT DrawTab(NMCUSTOMDRAW* cd, bool selected) {
    const auto& c = ui::Colors();
    HDC dc = cd->hdc;
    RECT r = cd->rc;
    FillRect(dc, &r, ui::Brush(c.chrome));
    if (cd->uItemState & (CDIS_HOT | CDIS_SELECTED))
        ui::FillRound(dc, r, Scale(4), (cd->uItemState & CDIS_SELECTED) ? c.pressed : c.chromeHover);

    wchar_t text[32];
    GetWindowTextW(cd->hdr.hwndFrom, text, 32);
    HFONT font = selected ? g.semibold : g.font;
    SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c.text);
    DrawTextW(dc, text, -1, &r, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    if (selected) {
        int w = TextWidth(g.semibold, text) - Scale(4);
        int cx = (r.left + r.right) / 2;
        RECT bar = {cx - w / 2, r.bottom - Scale(4), cx + w / 2, r.bottom - Scale(1)};
        ui::FillRound(dc, bar, Scale(2), c.accentText);
    }
    return CDRF_SKIPDEFAULT;
}

LRESULT DrawExpander(NMCUSTOMDRAW* cd) {
    const auto& c = ui::Colors();
    HDC dc = cd->hdc;
    RECT r = cd->rc;
    FillRect(dc, &r, ui::Brush(c.chrome));
    if (cd->uItemState & (CDIS_HOT | CDIS_SELECTED))
        ui::FillRound(dc, r, Scale(4), (cd->uItemState & CDIS_SELECTED) ? c.pressed : c.chromeHover);
    RECT icon = {r.left + Scale(4), r.top, r.left + Scale(28), r.bottom};
    HFONT chevron = ui::CreateIconFont(Scale(12));
    ui::DrawGlyph(dc, chevron, g.showLog ? glyph::ChevronDown : glyph::ChevronUp, icon, c.text);
    DeleteObject(chevron);
    SelectObject(dc, g.semibold);
    SetTextColor(dc, c.text);
    RECT label = {r.left + Scale(32), r.top, r.right, r.bottom};
    DrawTextW(dc, L"Activity", -1, &label, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    return CDRF_SKIPDEFAULT;
}

LRESULT OnButtonCustomDraw(NMCUSTOMDRAW* cd) {
    if (cd->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    HWND button = cd->hdr.hwndFrom;
    if (button == g.tabFile) return DrawTab(cd, false);
    if (button == g.tabHome) return DrawTab(cd, g.tab == Tab::Home);
    if (button == g.tabView) return DrawTab(cd, g.tab == Tab::View);
    if (button == g.expander) return DrawExpander(cd);
    if (const RibbonButton* b = FindButton(button)) return DrawRibbonButton(cd, *b);
    return CDRF_DODEFAULT;
}

LRESULT OnListCustomDraw(NMLVCUSTOMDRAW* cd) {
    const auto& c = ui::Colors();
    switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT:
            return CDRF_NOTIFYSUBITEMDRAW;
        case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
            size_t index = static_cast<size_t>(cd->nmcd.lItemlParam);
            cd->clrText = c.text;
            if (index < g.distros.size()) {
                const Distro& d = g.distros[index];
                if (cd->iSubItem == ColState) cd->clrText = StateColor(d) == c.textSecondary ? c.text : StateColor(d);
                if (cd->iSubItem == ColLocation || cd->iSubItem == ColOs) cd->clrText = c.textSecondary;
            }
            return CDRF_NEWFONT;
        }
    }
    return CDRF_DODEFAULT;
}

// The header is a child of the list, so its custom-draw notifications go to the
// list. The dark header style draws its text too dark; recolour it.
LRESULT CALLBACK ListSubclass(HWND list, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (msg == WM_NOTIFY) {
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->code == NM_CUSTOMDRAW && header->hwndFrom == ListView_GetHeader(list)) {
            auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(lParam);
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                SetTextColor(cd->hdc, ui::Colors().textSecondary);
                return CDRF_DODEFAULT;
            }
        }
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(list, ListSubclass, 1);
    }
    return DefSubclassProc(list, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Window setup

HWND MakeButton(const wchar_t* text, UINT id) {
    return CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, g.window,
                           reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), g.instance, nullptr);
}

void AddTip(HWND control, const wchar_t* text) {
    TTTOOLINFOW info{};
    info.cbSize = sizeof(info);
    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = g.window;
    info.uId = reinterpret_cast<UINT_PTR>(control);
    info.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(g.tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
}

void CreateControls(HWND window) {
    g.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window, nullptr,
                                g.instance, nullptr);

    g.tabFile = MakeButton(L"File", IDM_TAB_FILE);
    g.tabHome = MakeButton(L"Home", IDM_TAB_HOME);
    g.tabView = MakeButton(L"View", IDM_TAB_VIEW);
    AddTip(g.tabFile, L"File (Alt+F)");

    for (const auto& group : Ribbon()) {
        for (const auto& item : group.items) {
            HWND button = MakeButton(item.label, item.id);
            g.ribbon.push_back({&item, group.tab, button, !item.large});
            AddTip(button, item.tip);
        }
    }

    g.list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                             0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kListId)), g.instance,
                             nullptr);
    ListView_SetExtendedListViewStyle(g.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SetWindowSubclass(g.list, ListSubclass, 1, 0);
    for (int i = 0; i < static_cast<int>(std::size(kColumns)); ++i) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        column.pszText = const_cast<wchar_t*>(kColumns[i].title);
        column.cx = Scale(kColumns[i].width);
        column.fmt = kColumns[i].format;
        ListView_InsertColumn(g.list, i, &column);
    }

    g.expander = MakeButton(L"Activity", IDM_VIEW_LOG);
    AddTip(g.expander, L"Show or hide the activity log.");
    g.log = CreateWindowExW(0, WC_EDITW, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                                ES_AUTOVSCROLL,
                            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kLogId)), g.instance,
                            nullptr);
    SendMessageW(g.log, EM_SETLIMITTEXT, 0, 0);
    ApplyFonts();
}

void ShowContextMenu(POINT screen) {
    if (screen.x == -1 && screen.y == -1) {  // keyboard: anchor to the selected row
        int index = ListView_GetNextItem(g.list, -1, LVNI_SELECTED);
        RECT rect{};
        if (index >= 0) ListView_GetItemRect(g.list, index, &rect, LVIR_LABEL);
        screen = {rect.left, rect.bottom};
        ClientToScreen(g.list, &screen);
    }
    HMENU menu = GetSubMenu(g.menus, 1);  // the "Instance" menu
    SetMenuDefaultItem(menu, IDM_SHELL, FALSE);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, screen.x, screen.y, 0, g.window, nullptr);
}

LRESULT CALLBACK WndProc(HWND window, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g.window = window;
            g.dpi = GetDpiForWindow(window);
            g.menus = LoadMenuW(g.instance, MAKEINTRESOURCEW(IDR_MAINMENU));
            g.showDetails = ReadGuiSetting(L"DetailsPane", 1) != 0;
            g.showLog = ReadGuiSetting(L"ActivityLog", 1) != 0;
            CreateFonts();
            CreateControls(window);
            ApplyTheme();
            SetWindowPos(window, nullptr, 0, 0, Scale(1180), Scale(720), SWP_NOMOVE | SWP_NOZORDER);
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

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(window, &ps);
            RECT client;
            GetClientRect(window, &client);
            HDC buffer = nullptr;
            HPAINTBUFFER paint = BeginBufferedPaint(dc, &client, BPBF_COMPATIBLEBITMAP, nullptr, &buffer);
            if (paint) {
                Paint(buffer, client);
                EndBufferedPaint(paint, TRUE);
            } else {
                Paint(dc, client);
            }
            EndPaint(window, &ps);
            return 0;
        }

        case WM_SETFOCUS:
            SetFocus(g.list);
            return 0;

        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize = {Scale(720), Scale(520)};
            return 0;
        }

        case WM_DPICHANGED: {
            UINT oldDpi = g.dpi;
            g.dpi = HIWORD(wParam);
            CreateFonts();
            ApplyFonts();
            ApplyTheme();  // rebuilds the status images at the new size
            for (int i = 0; i < static_cast<int>(std::size(kColumns)); ++i)
                ListView_SetColumnWidth(g.list, i, MulDiv(ListView_GetColumnWidth(g.list, i), g.dpi, oldDpi));
            const RECT* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            return 0;
        }

        case WM_SETTINGCHANGE:
            if (lParam && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0 &&
                ui::RefreshTheme())
                ApplyTheme();
            break;

        case WM_COMMAND: {
            UINT id = LOWORD(wParam);
            // A clicked ribbon button takes the focus; give it back to the list so
            // keyboard shortcuts keep working on the selection.
            if (lParam && HIWORD(wParam) == BN_CLICKED && id != IDM_TAB_FILE) SetFocus(g.list);
            OnCommand(id);
            return 0;
        }

        case WM_INITMENUPOPUP: {
            HMENU menu = reinterpret_cast<HMENU>(wParam);
            int count = GetMenuItemCount(menu);
            for (int i = 0; i < count; ++i) {
                UINT id = GetMenuItemID(menu, i);
                if (id != static_cast<UINT>(-1) && id != 0)
                    EnableMenuItem(menu, id, MF_BYCOMMAND | (IsCommandEnabled(id) ? MF_ENABLED : MF_GRAYED));
            }
            CheckMenuItem(menu, IDM_PROTECT, MF_BYCOMMAND | (IsCommandChecked(IDM_PROTECT) ? MF_CHECKED : MF_UNCHECKED));
            return 0;
        }

        case WM_CONTEXTMENU:
            if (reinterpret_cast<HWND>(wParam) == g.list && Selected()) {
                ShowContextMenu({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
                return 0;
            }
            break;

        case WM_CTLCOLORSTATIC:  // the read-only log
        case WM_CTLCOLOREDIT: {
            const auto& c = ui::Colors();
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, c.text);
            SetBkColor(dc, c.card);
            return reinterpret_cast<LRESULT>(ui::Brush(c.card));
        }

        case WM_NOTIFY: {
            auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->code == NM_CUSTOMDRAW && header->hwndFrom != g.list)
                return OnButtonCustomDraw(reinterpret_cast<NMCUSTOMDRAW*>(lParam));
            if (header->hwndFrom != g.list) break;
            switch (header->code) {
                case NM_CUSTOMDRAW:
                    return OnListCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(lParam));
                case LVN_ITEMCHANGED:
                    UpdateCommandState();
                    break;
                case LVN_GETEMPTYMARKUP: {
                    auto* markup = reinterpret_cast<NMLVEMPTYMARKUP*>(lParam);
                    markup->dwFlags = EMF_CENTERED;
                    wcscpy_s(markup->szMarkup, g.loaded ? L"No WSL instances yet. Click New to create one."
                                                        : L"Loading instances...");
                    return TRUE;
                }
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
            } else if (wParam == kProgressTimer) {
                g.progressPhase += 0.008;
                if (g.progressPhase >= 1) g.progressPhase = 0;
                RECT r = g.progressRect;
                InflateRect(&r, 0, 1);
                InvalidateRect(window, &r, FALSE);
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
                bool first = !g.loaded;
                g.loaded = true;
                g.distros = std::move(result->distros);
                PopulateList(first ? L"" : selectName);
                if (!g.busy) {
                    size_t running = std::count_if(g.distros.begin(), g.distros.end(),
                                                   [](const Distro& d) { return d.running; });
                    SetStatus(std::to_wstring(g.distros.size()) + L" instances, " + std::to_wstring(running) +
                              L" running");
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
            if (g.menus) DestroyMenu(g.menus);
            if (g.stateImages) ImageList_Destroy(g.stateImages);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, msg, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    BufferedPaintInit();
    g.instance = instance;

    DWORD mode = ReadGuiSetting(L"Theme", 0);
    ui::InitTheme(mode <= 2 ? static_cast<ui::ThemeMode>(mode) : ui::ThemeMode::System);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    HWND window = CreateWindowExW(0, kWindowClass, kAppTitle, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                  CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, instance, nullptr);
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
    BufferedPaintUnInit();
    ui::ShutdownTheme();
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
