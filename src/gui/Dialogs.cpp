#include "Dialogs.h"

#include "resource.h"

#include <commctrl.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <filesystem>
#include <memory>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace wslmgr;
namespace fs = std::filesystem;

namespace {

constexpr wchar_t kAppTitle[] = L"WSL Manager";
constexpr UINT WM_APP_ONLINE = WM_APP + 10;

// ---------------------------------------------------------------------------
// Small Win32 helpers

std::wstring GetText(HWND dlg, int id) {
    HWND control = GetDlgItem(dlg, id);
    int len = GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(control, text.data(), len + 1);
    text.resize(static_cast<size_t>(len));
    return text;
}

int ComboAdd(HWND dlg, int id, const std::wstring& text, LPARAM data) {
    int index = static_cast<int>(SendDlgItemMessageW(dlg, id, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str())));
    SendDlgItemMessageW(dlg, id, CB_SETITEMDATA, index, data);
    return index;
}

int ComboSelection(HWND dlg, int id) { return static_cast<int>(SendDlgItemMessageW(dlg, id, CB_GETCURSEL, 0, 0)); }

LPARAM ComboSelectionData(HWND dlg, int id) {
    int index = ComboSelection(dlg, id);
    return index < 0 ? -1 : SendDlgItemMessageW(dlg, id, CB_GETITEMDATA, index, 0);
}

void Enable(HWND dlg, int id, bool enabled) { EnableWindow(GetDlgItem(dlg, id), enabled); }

bool IsChecked(HWND dlg, int id) { return IsDlgButtonChecked(dlg, id) == BST_CHECKED; }

void ShowError(HWND dlg, const std::wstring& message, int focusId) {
    MessageBoxW(dlg, message.c_str(), kAppTitle, MB_OK | MB_ICONWARNING);
    if (focusId) SetFocus(GetDlgItem(dlg, focusId));
}

template <typename State>
State* GetState(HWND dlg) {
    return reinterpret_cast<State*>(GetWindowLongPtrW(dlg, DWLP_USER));
}

// ---------------------------------------------------------------------------
// Shell file dialogs

void SetInitialFolder(IFileDialog* dialog, const std::wstring& path) {
    std::error_code ec;
    fs::path folder(path);
    while (!folder.empty() && !fs::is_directory(folder, ec)) {
        fs::path parent = folder.parent_path();
        if (parent == folder) return;
        folder = parent;
    }
    if (folder.empty()) return;
    ComPtr<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item))))
        dialog->SetFolder(item.Get());
}

std::optional<std::wstring> ResultPath(IFileDialog* dialog) {
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) return std::nullopt;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
    std::wstring result = path;
    CoTaskMemFree(path);
    return result;
}

std::optional<std::wstring> PickOpenFile(HWND owner, const wchar_t* title, const std::wstring& initial,
                                         const COMDLG_FILTERSPEC* filters, UINT filterCount) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return std::nullopt;
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    dialog->SetTitle(title);
    dialog->SetFileTypes(filterCount, filters);
    SetInitialFolder(dialog.Get(), initial);
    if (FAILED(dialog->Show(owner))) return std::nullopt;
    return ResultPath(dialog.Get());
}

std::optional<std::wstring> PickSaveFile(HWND owner, const wchar_t* title, const std::wstring& initial,
                                         BackupFormat format) {
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return std::nullopt;
    COMDLG_FILTERSPEC filters[] = {{L"Backup", L"*.tar"}, {L"All files", L"*.*"}};
    switch (format) {
        case BackupFormat::Vhd: filters[0] = {L"Virtual disk (*.vhdx)", L"*.vhdx"}; break;
        case BackupFormat::Tar: filters[0] = {L"Tar archive (*.tar)", L"*.tar"}; break;
        case BackupFormat::TarGz: filters[0] = {L"Gzip tar archive (*.tar.gz)", L"*.tar.gz;*.tgz"}; break;
        case BackupFormat::TarXz: filters[0] = {L"XZ tar archive (*.tar.xz)", L"*.tar.xz;*.txz"}; break;
    }
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
    dialog->SetTitle(title);
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetDefaultExtension(FormatExtension(format) + 1);
    SetInitialFolder(dialog.Get(), fs::path(initial).parent_path().wstring());
    dialog->SetFileName(fs::path(initial).filename().c_str());
    if (FAILED(dialog->Show(owner))) return std::nullopt;
    return ResultPath(dialog.Get());
}

// A folder picked with Browse is used as-is if it is empty or new; otherwise the
// instance gets its own subfolder inside it, so its virtual disk can't collide.
std::wstring FolderForInstance(const std::wstring& picked, const std::wstring& name) {
    std::error_code ec;
    if (name.empty() || !fs::is_directory(picked, ec) || fs::is_empty(picked, ec)) return picked;
    return (fs::path(picked) / name).wstring();
}

// ---------------------------------------------------------------------------
// Backup format + file controls, shared by the Backup and Remove dialogs

struct FormatChoice {
    BackupFormat format;
    const wchar_t* label;
};
constexpr FormatChoice kFormats[] = {
    {BackupFormat::Vhd, L"vhdx - disk image (fastest)"},
    {BackupFormat::Tar, L"tar - uncompressed archive"},
    {BackupFormat::TarGz, L"tar.gz - compressed archive"},
    {BackupFormat::TarXz, L"tar.xz - smallest, slowest"},
};

void InitBackupControls(HWND dlg, const Distro& distro) {
    BackupFormat preferred = DefaultBackupFormat(distro);
    for (const auto& choice : kFormats) {
        if (choice.format == BackupFormat::Vhd && distro.version != 2) continue;
        int index = ComboAdd(dlg, IDC_FORMAT, choice.label, static_cast<LPARAM>(choice.format));
        if (choice.format == preferred) SendDlgItemMessageW(dlg, IDC_FORMAT, CB_SETCURSEL, index, 0);
    }
    SetDlgItemTextW(dlg, IDC_FILE, DefaultBackupPath(distro.name, preferred).c_str());
}

BackupFormat SelectedFormat(HWND dlg) { return static_cast<BackupFormat>(ComboSelectionData(dlg, IDC_FORMAT)); }

// Returns true if the message was a backup-control command it handled.
bool HandleBackupCommand(HWND dlg, WORD id, WORD code) {
    if (id == IDC_FORMAT && code == CBN_SELCHANGE) {
        std::wstring file = Trim(GetText(dlg, IDC_FILE));
        if (!file.empty()) SetDlgItemTextW(dlg, IDC_FILE, WithBackupExtension(file, SelectedFormat(dlg)).c_str());
        return true;
    }
    if (id == IDC_FILEBROWSE && code == BN_CLICKED) {
        if (auto file = PickSaveFile(dlg, L"Save Backup As", GetText(dlg, IDC_FILE), SelectedFormat(dlg))) {
            SetDlgItemTextW(dlg, IDC_FILE, file->c_str());
            // Keep the format in step with an extension typed in the save dialog.
            BackupFormat typed = FormatFromPath(*file);
            int count = static_cast<int>(SendDlgItemMessageW(dlg, IDC_FORMAT, CB_GETCOUNT, 0, 0));
            for (int i = 0; i < count; ++i) {
                if (static_cast<BackupFormat>(SendDlgItemMessageW(dlg, IDC_FORMAT, CB_GETITEMDATA, i, 0)) == typed)
                    SendDlgItemMessageW(dlg, IDC_FORMAT, CB_SETCURSEL, i, 0);
            }
        }
        return true;
    }
    return false;
}

bool ReadBackupControls(HWND dlg, BackupRequest& request) {
    request.format = SelectedFormat(dlg);
    request.file = Trim(GetText(dlg, IDC_FILE));
    if (request.file.empty()) {
        ShowError(dlg, L"Enter the file to save the backup to.", IDC_FILE);
        return false;
    }
    std::error_code ec;
    request.file = fs::absolute(request.file, ec).wstring();
    if (fs::is_directory(request.file, ec)) {
        ShowError(dlg, L"The backup file name is a folder. Enter a file name.", IDC_FILE);
        return false;
    }
    if (fs::exists(request.file, ec)) {
        std::wstring question = L"The file " + request.file + L" already exists.\n\nOverwrite it?";
        if (MessageBoxW(dlg, question.c_str(), kAppTitle, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// New instance dialog

struct OnlineResult {
    std::vector<OnlineDistro> distros;
    std::wstring error;
};

std::vector<OnlineDistro> g_onlineCache;  // filled once per run; the catalog rarely changes

struct CreateState {
    const std::vector<Distro>* existing = nullptr;
    std::wstring cloneSource;
    CreateRequest result;
    bool nameEdited = false;
    bool locationEdited = false;
    int programmatic = 0;  // >0 while we set edit text ourselves (suppresses "user edited")
};

CreateRequest::Source CurrentSource(HWND dlg) {
    if (IsChecked(dlg, IDC_SRC_BACKUP)) return CreateRequest::Source::Backup;
    if (IsChecked(dlg, IDC_SRC_CLONE)) return CreateRequest::Source::Clone;
    return CreateRequest::Source::Clean;
}

void SetTextQuietly(HWND dlg, CreateState* state, int id, const std::wstring& text) {
    ++state->programmatic;
    SetDlgItemTextW(dlg, id, text.c_str());
    --state->programmatic;
}

std::wstring SelectedCloneSource(HWND dlg) {
    int index = ComboSelection(dlg, IDC_CLONESRC);
    if (index < 0) return {};
    int len = static_cast<int>(SendDlgItemMessageW(dlg, IDC_CLONESRC, CB_GETLBTEXTLEN, index, 0));
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    SendDlgItemMessageW(dlg, IDC_CLONESRC, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<size_t>(len));
    return text;
}

const OnlineDistro* SelectedOnline(HWND dlg) {
    LPARAM index = ComboSelectionData(dlg, IDC_ONLINE);
    if (index < 0 || static_cast<size_t>(index) >= g_onlineCache.size()) return nullptr;
    return &g_onlineCache[static_cast<size_t>(index)];
}

std::wstring MakeValidName(std::wstring base, const std::vector<Distro>& existing) {
    for (wchar_t& c : base) {
        bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'.' ||
                  c == L'_' || c == L'-';
        if (!ok) c = L'-';
    }
    if (base.empty()) return base;
    auto taken = [&](const std::wstring& name) {
        for (const auto& d : existing)
            if (EqualsIgnoreCase(d.name, name)) return true;
        return false;
    };
    if (!taken(base)) return base;
    for (int n = 2;; ++n) {
        std::wstring candidate = base + L"-" + std::to_wstring(n);
        if (!taken(candidate)) return candidate;
    }
}

void SuggestLocation(HWND dlg, CreateState* state) {
    if (state->locationEdited) return;
    std::wstring name = Trim(GetText(dlg, IDC_NAME));
    SetTextQuietly(dlg, state, IDC_LOCATION, name.empty() ? GetDefaultRoot() : DefaultLocationFor(name));
}

void SuggestName(HWND dlg, CreateState* state) {
    if (state->nameEdited) return;
    std::wstring base;
    switch (CurrentSource(dlg)) {
        case CreateRequest::Source::Clean:
            if (const OnlineDistro* d = SelectedOnline(dlg)) base = d->name;
            break;
        case CreateRequest::Source::Backup: {
            std::wstring file = Trim(GetText(dlg, IDC_BACKUPFILE));
            if (!file.empty()) base = StripBackupExtension(fs::path(file).filename().wstring());
            break;
        }
        case CreateRequest::Source::Clone: {
            std::wstring source = SelectedCloneSource(dlg);
            if (!source.empty()) base = source + L"-copy";
            break;
        }
    }
    SetTextQuietly(dlg, state, IDC_NAME, MakeValidName(base, *state->existing));
    SuggestLocation(dlg, state);
}

void UpdateSourceControls(HWND dlg) {
    auto source = CurrentSource(dlg);
    Enable(dlg, IDC_ONLINE, source == CreateRequest::Source::Clean && !g_onlineCache.empty());
    Enable(dlg, IDC_WEBDL, source == CreateRequest::Source::Clean);
    Enable(dlg, IDC_BACKUPFILE, source == CreateRequest::Source::Backup);
    Enable(dlg, IDC_BACKUPBROWSE, source == CreateRequest::Source::Backup);
    Enable(dlg, IDC_CLONESRC, source == CreateRequest::Source::Clone);
    Enable(dlg, IDC_VERSION, source != CreateRequest::Source::Clone);
}

void FillOnlineCombo(HWND dlg) {
    SendDlgItemMessageW(dlg, IDC_ONLINE, CB_RESETCONTENT, 0, 0);
    int select = 0;
    for (size_t i = 0; i < g_onlineCache.size(); ++i) {
        const auto& d = g_onlineCache[i];
        std::wstring label = d.friendlyName == d.name ? d.name : d.name + L"  (" + d.friendlyName + L")";
        int index = ComboAdd(dlg, IDC_ONLINE, label, static_cast<LPARAM>(i));
        if (d.name == L"Ubuntu") select = index;
    }
    SendDlgItemMessageW(dlg, IDC_ONLINE, CB_SETCURSEL, select, 0);
}

void StartOnlineLoad(HWND dlg) {
    SendDlgItemMessageW(dlg, IDC_ONLINE, CB_RESETCONTENT, 0, 0);
    ComboAdd(dlg, IDC_ONLINE, L"Loading available distributions...", -1);
    SendDlgItemMessageW(dlg, IDC_ONLINE, CB_SETCURSEL, 0, 0);
    std::thread([dlg] {
        auto result = std::make_unique<OnlineResult>();
        try {
            result->distros = ListOnlineDistros();
        } catch (const WslError& e) {
            result->error = e.message();
        }
        if (PostMessageW(dlg, WM_APP_ONLINE, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
    }).detach();
}

bool AcceptCreate(HWND dlg, CreateState* state) {
    CreateRequest r;
    r.source = CurrentSource(dlg);
    r.name = Trim(GetText(dlg, IDC_NAME));
    try {
        ValidateNewName(r.name);
    } catch (const WslError& e) {
        ShowError(dlg, e.message(), IDC_NAME);
        return false;
    }
    r.location = Trim(GetText(dlg, IDC_LOCATION));
    if (r.location.empty()) {
        ShowError(dlg, L"Choose a storage location for the instance.", IDC_LOCATION);
        return false;
    }
    r.version = ComboSelectionData(dlg, IDC_VERSION) == 1 ? 1 : 2;

    switch (r.source) {
        case CreateRequest::Source::Clean: {
            const OnlineDistro* d = SelectedOnline(dlg);
            if (!d) {
                ShowError(dlg, L"Choose a distribution to install. The list may still be loading.", IDC_ONLINE);
                return false;
            }
            r.distro = d->name;
            r.webDownload = IsChecked(dlg, IDC_WEBDL);
            break;
        }
        case CreateRequest::Source::Backup: {
            r.backupFile = Trim(GetText(dlg, IDC_BACKUPFILE));
            std::error_code ec;
            if (r.backupFile.empty() || !fs::is_regular_file(r.backupFile, ec)) {
                ShowError(dlg, L"Choose an existing backup file to restore.", IDC_BACKUPFILE);
                return false;
            }
            break;
        }
        case CreateRequest::Source::Clone:
            r.cloneSource = SelectedCloneSource(dlg);
            if (r.cloneSource.empty()) {
                ShowError(dlg, L"Choose the instance to copy.", IDC_CLONESRC);
                return false;
            }
            break;
    }
    state->result = std::move(r);
    return true;
}

INT_PTR CALLBACK CreateDialogProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = GetState<CreateState>(dlg);
    switch (msg) {
        case WM_INITDIALOG: {
            state = reinterpret_cast<CreateState*>(lParam);
            SetWindowLongPtrW(dlg, DWLP_USER, lParam);

            ComboAdd(dlg, IDC_VERSION, L"2", 2);
            ComboAdd(dlg, IDC_VERSION, L"1", 1);
            SendDlgItemMessageW(dlg, IDC_VERSION, CB_SETCURSEL, 0, 0);

            int cloneIndex = -1;
            for (const auto& d : *state->existing) {
                int index = ComboAdd(dlg, IDC_CLONESRC, d.name, 0);
                if (EqualsIgnoreCase(d.name, state->cloneSource)) cloneIndex = index;
            }
            SendDlgItemMessageW(dlg, IDC_CLONESRC, CB_SETCURSEL, cloneIndex < 0 ? 0 : cloneIndex, 0);
            if (state->existing->empty()) Enable(dlg, IDC_SRC_CLONE, false);

            if (g_onlineCache.empty())
                StartOnlineLoad(dlg);
            else
                FillOnlineCombo(dlg);

            CheckRadioButton(dlg, IDC_SRC_CLEAN, IDC_SRC_CLONE, cloneIndex >= 0 ? IDC_SRC_CLONE : IDC_SRC_CLEAN);
            UpdateSourceControls(dlg);
            SuggestName(dlg, state);
            return TRUE;
        }

        case WM_APP_ONLINE: {
            std::unique_ptr<OnlineResult> result(reinterpret_cast<OnlineResult*>(lParam));
            if (result->error.empty()) {
                g_onlineCache = std::move(result->distros);
                FillOnlineCombo(dlg);
            } else {
                SendDlgItemMessageW(dlg, IDC_ONLINE, CB_RESETCONTENT, 0, 0);
                std::wstring firstLine = result->error.substr(0, result->error.find(L'\n'));
                ComboAdd(dlg, IDC_ONLINE, L"Could not load the catalog: " + firstLine, -1);
                SendDlgItemMessageW(dlg, IDC_ONLINE, CB_SETCURSEL, 0, 0);
            }
            UpdateSourceControls(dlg);
            SuggestName(dlg, state);
            return TRUE;
        }

        case WM_COMMAND: {
            WORD id = LOWORD(wParam);
            WORD code = HIWORD(wParam);
            switch (id) {
                case IDC_SRC_CLEAN:
                case IDC_SRC_BACKUP:
                case IDC_SRC_CLONE:
                    if (code == BN_CLICKED) {
                        CheckRadioButton(dlg, IDC_SRC_CLEAN, IDC_SRC_CLONE, id);
                        UpdateSourceControls(dlg);
                        SuggestName(dlg, state);
                    }
                    return TRUE;
                case IDC_ONLINE:
                case IDC_CLONESRC:
                    if (code == CBN_SELCHANGE) SuggestName(dlg, state);
                    return TRUE;
                case IDC_BACKUPFILE:
                    if (code == EN_CHANGE) SuggestName(dlg, state);
                    return TRUE;
                case IDC_NAME:
                    if (code == EN_CHANGE && state->programmatic == 0) {
                        state->nameEdited = !Trim(GetText(dlg, IDC_NAME)).empty();
                        SuggestLocation(dlg, state);
                    }
                    return TRUE;
                case IDC_LOCATION:
                    if (code == EN_CHANGE && state->programmatic == 0)
                        state->locationEdited = !Trim(GetText(dlg, IDC_LOCATION)).empty();
                    return TRUE;
                case IDC_BACKUPBROWSE: {
                    static constexpr COMDLG_FILTERSPEC kFilters[] = {
                        {L"WSL backups", L"*.tar;*.tar.gz;*.tgz;*.tar.xz;*.txz;*.vhdx;*.vhd"},
                        {L"All files", L"*.*"}};
                    std::wstring initial = Trim(GetText(dlg, IDC_BACKUPFILE));
                    if (initial.empty()) initial = GetDefaultRoot() + L"\\Backups";
                    if (auto file = PickOpenFile(dlg, L"Choose Backup to Restore", initial, kFilters, 2))
                        SetDlgItemTextW(dlg, IDC_BACKUPFILE, file->c_str());
                    return TRUE;
                }
                case IDC_LOCBROWSE: {
                    std::wstring current = Trim(GetText(dlg, IDC_LOCATION));
                    if (auto folder = PickFolder(dlg, L"Choose Storage Location", current.empty() ? GetDefaultRoot() : current))
                        SetDlgItemTextW(dlg, IDC_LOCATION,
                                        FolderForInstance(*folder, Trim(GetText(dlg, IDC_NAME))).c_str());
                    return TRUE;
                }
                case IDOK:
                    if (AcceptCreate(dlg, state)) EndDialog(dlg, IDOK);
                    return TRUE;
                case IDCANCEL:
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;
            }
            break;
        }
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Backup, move, remove and text dialogs

struct BackupState {
    const Distro* distro;
    BackupRequest result;
};

INT_PTR CALLBACK BackupDialogProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = GetState<BackupState>(dlg);
    switch (msg) {
        case WM_INITDIALOG:
            state = reinterpret_cast<BackupState*>(lParam);
            SetWindowLongPtrW(dlg, DWLP_USER, lParam);
            SetDlgItemTextW(dlg, IDC_INFO, state->distro->name.c_str());
            InitBackupControls(dlg, *state->distro);
            return TRUE;
        case WM_COMMAND:
            if (HandleBackupCommand(dlg, LOWORD(wParam), HIWORD(wParam))) return TRUE;
            if (LOWORD(wParam) == IDOK) {
                if (ReadBackupControls(dlg, state->result)) EndDialog(dlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                EndDialog(dlg, IDCANCEL);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

struct MoveState {
    const Distro* distro;
    std::wstring result;
};

INT_PTR CALLBACK MoveDialogProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = GetState<MoveState>(dlg);
    switch (msg) {
        case WM_INITDIALOG: {
            state = reinterpret_cast<MoveState*>(lParam);
            SetWindowLongPtrW(dlg, DWLP_USER, lParam);
            const Distro& d = *state->distro;
            SetDlgItemTextW(dlg, IDC_INFO, d.name.c_str());
            SetDlgItemTextW(dlg, IDC_CURRENT, d.basePath.c_str());
            std::wstring suggestion = DefaultLocationFor(d.name);
            if (!EqualsIgnoreCase(NormalizeFolder(suggestion), NormalizeFolder(d.basePath)))
                SetDlgItemTextW(dlg, IDC_LOCATION, suggestion.c_str());
            return TRUE;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_LOCBROWSE: {
                    std::wstring current = Trim(GetText(dlg, IDC_LOCATION));
                    if (auto folder = PickFolder(dlg, L"Choose New Storage Location",
                                                 current.empty() ? GetDefaultRoot() : current))
                        SetDlgItemTextW(dlg, IDC_LOCATION, FolderForInstance(*folder, state->distro->name).c_str());
                    return TRUE;
                }
                case IDOK: {
                    std::wstring location = Trim(GetText(dlg, IDC_LOCATION));
                    if (location.empty()) {
                        ShowError(dlg, L"Choose the folder to move the instance to.", IDC_LOCATION);
                        return TRUE;
                    }
                    if (EqualsIgnoreCase(NormalizeFolder(location), NormalizeFolder(state->distro->basePath))) {
                        ShowError(dlg, L"The instance is already stored in that folder.", IDC_LOCATION);
                        return TRUE;
                    }
                    state->result = location;
                    EndDialog(dlg, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

struct RemoveState {
    const Distro* distro;
    RemoveRequest result;
};

void UpdateRemoveControls(HWND dlg) {
    bool backup = IsChecked(dlg, IDC_BACKUPFIRST);
    for (int id : {IDC_FORMATLABEL, IDC_FORMAT, IDC_FILELABEL, IDC_FILE, IDC_FILEBROWSE}) Enable(dlg, id, backup);
}

INT_PTR CALLBACK RemoveDialogProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = GetState<RemoveState>(dlg);
    switch (msg) {
        case WM_INITDIALOG: {
            state = reinterpret_cast<RemoveState*>(lParam);
            SetWindowLongPtrW(dlg, DWLP_USER, lParam);
            const Distro& d = *state->distro;
            std::wstring info = L"This permanently deletes '" + d.name + L"'";
            if (d.diskBytes) info += L" and its " + FormatBytes(d.diskBytes) + L" virtual disk";
            info += L" from " + d.basePath + L". This cannot be undone.";
            SetDlgItemTextW(dlg, IDC_INFO, info.c_str());
            CheckDlgButton(dlg, IDC_BACKUPFIRST, BST_CHECKED);
            CheckDlgButton(dlg, IDC_DELETEFOLDER, BST_CHECKED);
            InitBackupControls(dlg, d);
            UpdateRemoveControls(dlg);
            return TRUE;
        }
        case WM_COMMAND:
            if (HandleBackupCommand(dlg, LOWORD(wParam), HIWORD(wParam))) return TRUE;
            switch (LOWORD(wParam)) {
                case IDC_BACKUPFIRST:
                    UpdateRemoveControls(dlg);
                    return TRUE;
                case IDOK: {
                    RemoveRequest& r = state->result;
                    r.backupFirst = IsChecked(dlg, IDC_BACKUPFIRST);
                    r.deleteFolder = IsChecked(dlg, IDC_DELETEFOLDER);
                    if (r.backupFirst && !ReadBackupControls(dlg, r.backup)) return TRUE;
                    if (!r.backupFirst) {
                        std::wstring question = L"Remove '" + state->distro->name + L"' without a backup?";
                        if (MessageBoxW(dlg, question.c_str(), kAppTitle, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) !=
                            IDYES)
                            return TRUE;
                    }
                    EndDialog(dlg, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

struct TextState {
    std::wstring title, prompt, initial, result;
};

INT_PTR CALLBACK TextDialogProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = GetState<TextState>(dlg);
    switch (msg) {
        case WM_INITDIALOG:
            state = reinterpret_cast<TextState*>(lParam);
            SetWindowLongPtrW(dlg, DWLP_USER, lParam);
            SetWindowTextW(dlg, state->title.c_str());
            SetDlgItemTextW(dlg, IDC_PROMPT, state->prompt.c_str());
            SetDlgItemTextW(dlg, IDC_TEXT, state->initial.c_str());
            SendDlgItemMessageW(dlg, IDC_TEXT, EM_SETSEL, 0, -1);
            return TRUE;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                state->result = Trim(GetText(dlg, IDC_TEXT));
                if (state->result.empty()) {
                    ShowError(dlg, L"Enter a value.", IDC_TEXT);
                    return TRUE;
                }
                EndDialog(dlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                EndDialog(dlg, IDCANCEL);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

template <typename State>
bool RunDialog(HWND owner, int id, DLGPROC proc, State& state) {
    return DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(id), owner, proc,
                           reinterpret_cast<LPARAM>(&state)) == IDOK;
}

}  // namespace

std::optional<CreateRequest> ShowCreateDialog(HWND owner, const std::vector<Distro>& existing,
                                              const std::wstring& cloneSource) {
    CreateState state;
    state.existing = &existing;
    state.cloneSource = cloneSource;
    if (!RunDialog(owner, IDD_CREATE, CreateDialogProc, state)) return std::nullopt;
    return state.result;
}

std::optional<BackupRequest> ShowBackupDialog(HWND owner, const Distro& distro) {
    BackupState state{&distro, {}};
    if (!RunDialog(owner, IDD_BACKUP, BackupDialogProc, state)) return std::nullopt;
    return state.result;
}

std::optional<std::wstring> ShowMoveDialog(HWND owner, const Distro& distro) {
    MoveState state{&distro, {}};
    if (!RunDialog(owner, IDD_MOVE, MoveDialogProc, state)) return std::nullopt;
    return state.result;
}

std::optional<RemoveRequest> ShowRemoveDialog(HWND owner, const Distro& distro) {
    RemoveState state{&distro, {}};
    if (!RunDialog(owner, IDD_REMOVE, RemoveDialogProc, state)) return std::nullopt;
    return state.result;
}

std::optional<std::wstring> ShowTextDialog(HWND owner, const std::wstring& title, const std::wstring& prompt,
                                           const std::wstring& initial) {
    TextState state{title, prompt, initial, {}};
    if (!RunDialog(owner, IDD_TEXT, TextDialogProc, state)) return std::nullopt;
    return state.result;
}

std::optional<std::wstring> PickFolder(HWND owner, const std::wstring& title, const std::wstring& initial) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return std::nullopt;
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(title.c_str());
    SetInitialFolder(dialog.Get(), initial);
    if (FAILED(dialog->Show(owner))) return std::nullopt;
    return ResultPath(dialog.Get());
}
