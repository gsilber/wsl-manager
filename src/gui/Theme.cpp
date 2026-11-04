#include "Theme.h"

#include "resource.h"

#include <objidl.h>

#include <algorithm>
// gdiplus.h uses unqualified min/max, which NOMINMAX removes.
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <map>
#include <string>

namespace ui {
namespace {

// Light and dark palettes, after the Fluent 2 (Microsoft 365) neutral and brand ramps.
constexpr Palette kLight = {
    .chrome = RGB(0xF3, 0xF3, 0xF3),
    .chromeHover = RGB(0xE6, 0xE6, 0xE6),
    .card = RGB(0xFF, 0xFF, 0xFF),
    .border = RGB(0xE0, 0xE0, 0xE0),
    .divider = RGB(0xE6, 0xE6, 0xE6),
    .text = RGB(0x24, 0x24, 0x24),
    .textSecondary = RGB(0x61, 0x61, 0x61),
    .textDisabled = RGB(0xA8, 0xA8, 0xA8),
    .hover = RGB(0xF0, 0xF0, 0xF0),
    .pressed = RGB(0xE0, 0xE0, 0xE0),
    .checked = RGB(0xDC, 0xEB, 0xF8),
    .accent = RGB(0x0F, 0x6C, 0xBD),
    .accentHover = RGB(0x11, 0x5E, 0xA3),
    .accentPressed = RGB(0x0C, 0x3B, 0x5E),
    .onAccent = RGB(0xFF, 0xFF, 0xFF),
    .accentText = RGB(0x0F, 0x6C, 0xBD),
    .control = RGB(0xFF, 0xFF, 0xFF),
    .controlHover = RGB(0xF5, 0xF5, 0xF5),
    .controlPressed = RGB(0xE0, 0xE0, 0xE0),
    .controlBorder = RGB(0xD1, 0xD1, 0xD1),
    .controlStrongBorder = RGB(0x61, 0x61, 0x61),
    .field = RGB(0xFF, 0xFF, 0xFF),
    .success = RGB(0x10, 0x7C, 0x10),
    .warning = RGB(0xBC, 0x4B, 0x09),
    .danger = RGB(0xC4, 0x2B, 0x1C),
    .infoBack = RGB(0xEB, 0xF3, 0xFC),
};

constexpr Palette kDark = {
    .chrome = RGB(0x1F, 0x1F, 0x1F),
    .chromeHover = RGB(0x2E, 0x2E, 0x2E),
    .card = RGB(0x29, 0x29, 0x29),
    .border = RGB(0x3B, 0x3B, 0x3B),
    .divider = RGB(0x3D, 0x3D, 0x3D),
    .text = RGB(0xFF, 0xFF, 0xFF),
    .textSecondary = RGB(0xAD, 0xAD, 0xAD),
    .textDisabled = RGB(0x6E, 0x6E, 0x6E),
    .hover = RGB(0x38, 0x38, 0x38),
    .pressed = RGB(0x44, 0x44, 0x44),
    .checked = RGB(0x1F, 0x3A, 0x55),
    .accent = RGB(0x11, 0x5E, 0xA3),
    .accentHover = RGB(0x0F, 0x6C, 0xBD),
    .accentPressed = RGB(0x0C, 0x3B, 0x5E),
    .onAccent = RGB(0xFF, 0xFF, 0xFF),
    .accentText = RGB(0x47, 0x9E, 0xF5),
    .control = RGB(0x33, 0x33, 0x33),
    .controlHover = RGB(0x3D, 0x3D, 0x3D),
    .controlPressed = RGB(0x2B, 0x2B, 0x2B),
    .controlBorder = RGB(0x4D, 0x4D, 0x4D),
    .controlStrongBorder = RGB(0xAD, 0xAD, 0xAD),
    .field = RGB(0x1F, 0x1F, 0x1F),
    .success = RGB(0x6C, 0xCB, 0x5F),
    .warning = RGB(0xF7, 0xB9, 0x55),
    .danger = RGB(0xFF, 0x99, 0xA4),
    .infoBack = RGB(0x1A, 0x31, 0x48),
};

// Undocumented uxtheme exports (by ordinal) that Windows' own apps use for dark
// menus and scroll bars. Present since Windows 10 1903; looked up defensively.
enum class AppMode { Default, AllowDark, ForceDark, ForceLight };
using SetPreferredAppModeFn = AppMode(WINAPI*)(AppMode);
using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);
using FlushMenuThemesFn = void(WINAPI*)();
using RefreshImmersiveColorPolicyStateFn = void(WINAPI*)();

SetPreferredAppModeFn g_setPreferredAppMode = nullptr;
AllowDarkModeForWindowFn g_allowDarkModeForWindow = nullptr;
FlushMenuThemesFn g_flushMenuThemes = nullptr;
RefreshImmersiveColorPolicyStateFn g_refreshImmersiveColorPolicyState = nullptr;

ThemeMode g_mode = ThemeMode::System;
bool g_dark = false;
ULONG_PTR g_gdiplusToken = 0;
std::map<COLORREF, HBRUSH> g_brushes;
const wchar_t* g_iconFont = L"Segoe MDL2 Assets";

bool SystemUsesDarkApps() {
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return false;
    return value == 0;
}

bool Evaluate() {
    bool dark = g_mode == ThemeMode::Dark || (g_mode == ThemeMode::System && SystemUsesDarkApps());
    bool changed = dark != g_dark;
    g_dark = dark;
    if (g_setPreferredAppMode) g_setPreferredAppMode(dark ? AppMode::ForceDark : AppMode::ForceLight);
    if (g_refreshImmersiveColorPolicyState) g_refreshImmersiveColorPolicyState();
    return changed;
}

int CALLBACK FontExists(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found) {
    *reinterpret_cast<bool*>(found) = true;
    return 0;
}

Gdiplus::Color ToColor(COLORREF c) { return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }

void RoundPath(Gdiplus::GraphicsPath& path, Gdiplus::RectF r, float radius) {
    float d = std::min({radius * 2, r.Width, r.Height});
    if (d <= 0.5f) {
        path.AddRectangle(r);
        return;
    }
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

void Prepare(Gdiplus::Graphics& g) {
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
}

int ScaleFor(HWND window, int value) { return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96); }

// ---------------------------------------------------------------------------
// Dialogs

struct DialogInfo {
    int footerTop = 0;  // the button band at the bottom is painted in the chrome colour
    int primaryId = IDOK;
    HFONT headingFont = nullptr;
};

void DrawCheckOrRadio(NMCUSTOMDRAW* cd, bool radio, COLORREF background) {
    const Palette& c = Colors();
    HWND button = cd->hdr.hwndFrom;
    HDC dc = cd->hdc;
    RECT r = cd->rc;
    bool hot = cd->uItemState & CDIS_HOT;
    bool pressed = cd->uItemState & CDIS_SELECTED;
    bool disabled = cd->uItemState & CDIS_DISABLED;
    bool cues = cd->uItemState & CDIS_SHOWKEYBOARDCUES;
    bool checked = SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED;

    FillRect(dc, &r, Brush(background));
    int box = ScaleFor(button, 16);
    RECT boxRect = {r.left + 1, (r.top + r.bottom - box) / 2, r.left + 1 + box, (r.top + r.bottom - box) / 2 + box};

    COLORREF accent = disabled ? c.textDisabled : pressed ? c.accentPressed : hot ? c.accentHover : c.accent;
    COLORREF ring = disabled ? c.textDisabled : c.controlStrongBorder;
    COLORREF empty = pressed ? c.controlPressed : hot ? c.controlHover : c.field;
    float cx = (boxRect.left + boxRect.right) / 2.0f, cy = (boxRect.top + boxRect.bottom) / 2.0f;
    if (radio) {
        if (checked) {
            FillCircle(dc, cx, cy, box / 2.0f, accent);
            float dot = box * (pressed ? 0.18f : hot ? 0.27f : 0.23f);
            FillCircle(dc, cx, cy, dot, c.onAccent);
        } else {
            FillCircle(dc, cx, cy, box / 2.0f, ring);
            FillCircle(dc, cx, cy, box / 2.0f - 1.0f, empty);
        }
    } else if (checked) {
        FillRound(dc, boxRect, ScaleFor(button, 3), accent);
        HFONT glyphFont = CreateIconFont(box * 3 / 4);
        DrawGlyph(dc, glyphFont, 0xE73E /* CheckMark */, boxRect, c.onAccent);
        DeleteObject(glyphFont);
    } else {
        FillRound(dc, boxRect, ScaleFor(button, 3), empty, ring);
    }

    wchar_t text[256];
    GetWindowTextW(button, text, 256);
    HGDIOBJ oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(button, WM_GETFONT, 0, 0)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? c.textDisabled : c.text);
    RECT textRect = {boxRect.right + ScaleFor(button, 8), r.top, r.right, r.bottom};
    UINT format = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | (cues ? 0u : DT_HIDEPREFIX);
    DrawTextW(dc, text, -1, &textRect, format);
    if ((cd->uItemState & CDIS_FOCUS) && cues) {
        RECT measured = textRect;
        DrawTextW(dc, text, -1, &measured, format | DT_CALCRECT);
        measured.top = textRect.top + 1;
        measured.bottom = textRect.bottom - 1;
        InflateRect(&measured, 2, 0);
        SetTextColor(dc, c.text);
        DrawFocusRect(dc, &measured);
    }
    SelectObject(dc, oldFont);
}

void DrawPushButton(NMCUSTOMDRAW* cd, bool primary, COLORREF background) {
    const Palette& c = Colors();
    HWND button = cd->hdr.hwndFrom;
    HDC dc = cd->hdc;
    RECT r = cd->rc;
    bool hot = cd->uItemState & CDIS_HOT;
    bool pressed = cd->uItemState & CDIS_SELECTED;
    bool disabled = cd->uItemState & CDIS_DISABLED;
    bool cues = cd->uItemState & CDIS_SHOWKEYBOARDCUES;

    COLORREF fill, border, fore;
    if (disabled) {
        fill = c.control;
        border = c.controlBorder;
        fore = c.textDisabled;
    } else if (primary) {
        fill = pressed ? c.accentPressed : hot ? c.accentHover : c.accent;
        border = fill;
        fore = c.onAccent;
    } else {
        fill = pressed ? c.controlPressed : hot ? c.controlHover : c.control;
        border = c.controlBorder;
        fore = c.text;
    }
    FillRect(dc, &r, Brush(background));
    FillRound(dc, r, ScaleFor(button, 4), fill, border);

    wchar_t text[128];
    GetWindowTextW(button, text, 128);
    HGDIOBJ oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(button, WM_GETFONT, 0, 0)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fore);
    DrawTextW(dc, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER | (cues ? 0u : DT_HIDEPREFIX));
    SelectObject(dc, oldFont);
    if ((cd->uItemState & CDIS_FOCUS) && cues) {
        RECT ring = r;
        InflateRect(&ring, -ScaleFor(button, 2), -ScaleFor(button, 2));
        StrokeRound(dc, ring, ScaleFor(button, 3), primary ? c.onAccent : c.text, 1.0f);
    }
}

LRESULT DrawDialogButton(HWND dialog, NMCUSTOMDRAW* cd, const DialogInfo& info) {
    if (cd->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    HWND button = cd->hdr.hwndFrom;
    RECT inDialog;
    GetWindowRect(button, &inDialog);
    MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&inDialog), 2);
    COLORREF background = inDialog.top >= info.footerTop ? Colors().chrome : Colors().card;

    switch (GetWindowLongW(button, GWL_STYLE) & BS_TYPEMASK) {
        case BS_PUSHBUTTON:
        case BS_DEFPUSHBUTTON:
            DrawPushButton(cd, GetDlgCtrlID(button) == info.primaryId, background);
            return CDRF_SKIPDEFAULT;
        case BS_CHECKBOX:
        case BS_AUTOCHECKBOX:
            DrawCheckOrRadio(cd, false, background);
            return CDRF_SKIPDEFAULT;
        case BS_RADIOBUTTON:
        case BS_AUTORADIOBUTTON:
            DrawCheckOrRadio(cd, true, background);
            return CDRF_SKIPDEFAULT;
    }
    return CDRF_DODEFAULT;
}

bool IsClass(HWND window, const wchar_t* name) {
    wchar_t cls[32];
    return GetClassNameW(window, cls, 32) && _wcsicmp(cls, name) == 0;
}

LRESULT CALLBACK DialogSubclass(HWND dialog, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    auto* info = reinterpret_cast<DialogInfo*>(data);
    const Palette& c = Colors();
    switch (msg) {
        case WM_ERASEBKGND: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            RECT client;
            GetClientRect(dialog, &client);
            RECT body = client, footer = client;
            body.bottom = footer.top = info->footerTop;
            FillRect(dc, &body, Brush(c.card));
            FillRect(dc, &footer, Brush(c.chrome));
            return 1;
        }
        case WM_CTLCOLORDLG:
            return reinterpret_cast<LRESULT>(Brush(c.card));
        case WM_CTLCOLORSTATIC: {  // labels, and read-only or disabled edit boxes
            HDC dc = reinterpret_cast<HDC>(wParam);
            int id = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
            bool hint = id >= IDC_HINT_FIRST && id <= IDC_HINT_LAST;
            SetTextColor(dc, hint ? c.textSecondary : c.text);
            SetBkColor(dc, c.card);
            return reinterpret_cast<LRESULT>(Brush(c.card));
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, c.text);
            SetBkColor(dc, c.field);
            return reinterpret_cast<LRESULT>(Brush(c.field));
        }
        case WM_CTLCOLORBTN:
            return reinterpret_cast<LRESULT>(Brush(c.card));
        case WM_NOTIFY: {
            auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->code == NM_CUSTOMDRAW && IsClass(header->hwndFrom, WC_BUTTONW))
                return DrawDialogButton(dialog, reinterpret_cast<NMCUSTOMDRAW*>(lParam), *info);
            break;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(dialog, DialogSubclass, 1);
            if (info->headingFont) DeleteObject(info->headingFont);
            delete info;
            break;
    }
    return DefSubclassProc(dialog, msg, wParam, lParam);
}

}  // namespace

// ---------------------------------------------------------------------------
// Theme state

void InitTheme(ThemeMode mode) {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr);

    if (HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        g_refreshImmersiveColorPolicyState =
            reinterpret_cast<RefreshImmersiveColorPolicyStateFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(104)));
        g_allowDarkModeForWindow =
            reinterpret_cast<AllowDarkModeForWindowFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(133)));
        g_setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
        g_flushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
    }

    HDC dc = GetDC(nullptr);
    LOGFONTW query{};
    query.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(query.lfFaceName, L"Segoe Fluent Icons");
    bool found = false;
    EnumFontFamiliesExW(dc, &query, FontExists, reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    if (found) g_iconFont = L"Segoe Fluent Icons";

    g_mode = mode;
    g_dark = !SystemUsesDarkApps();  // force Evaluate() to apply the app mode
    Evaluate();
}

void ShutdownTheme() {
    for (auto& [color, brush] : g_brushes) DeleteObject(brush);
    g_brushes.clear();
    if (g_gdiplusToken) Gdiplus::GdiplusShutdown(g_gdiplusToken);
    g_gdiplusToken = 0;
}

void SetThemeMode(ThemeMode mode) {
    g_mode = mode;
    Evaluate();
}

ThemeMode GetThemeMode() { return g_mode; }

bool RefreshTheme() { return Evaluate(); }

bool IsDark() { return g_dark; }

const Palette& Colors() { return g_dark ? kDark : kLight; }

HBRUSH Brush(COLORREF color) {
    auto it = g_brushes.find(color);
    if (it != g_brushes.end()) return it->second;
    HBRUSH brush = CreateSolidBrush(color);
    g_brushes.emplace(color, brush);
    return brush;
}

COLORREF Blend(COLORREF from, COLORREF to, double amount) {
    auto mix = [amount](int a, int b) { return static_cast<BYTE>(a + (b - a) * amount + 0.5); };
    return RGB(mix(GetRValue(from), GetRValue(to)), mix(GetGValue(from), GetGValue(to)),
               mix(GetBValue(from), GetBValue(to)));
}

void ApplyWindowFrame(HWND window, COLORREF caption) {
    BOOL dark = g_dark;
    if (g_allowDarkModeForWindow) g_allowDarkModeForWindow(window, dark);
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    COLORREF text = Colors().text;
    DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR, &text, sizeof(text));
}

void ApplyControlTheme(HWND control, const wchar_t* lightClass, const wchar_t* darkClass) {
    if (!control) return;
    if (g_allowDarkModeForWindow) g_allowDarkModeForWindow(control, g_dark);
    SetWindowTheme(control, g_dark ? darkClass : lightClass, nullptr);
    SendMessageW(control, WM_THEMECHANGED, 0, 0);
}

void FlushMenus() {
    if (g_flushMenuThemes) g_flushMenuThemes();
}

const wchar_t* IconFontName() { return g_iconFont; }

HFONT CreateUiFont(UINT dpi, double points, int weight) {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
    LOGFONTW font = metrics.lfMessageFont;
    if (points > 0) font.lfHeight = -static_cast<LONG>(points * dpi / 72.0 + 0.5);
    font.lfWeight = weight;
    font.lfQuality = CLEARTYPE_QUALITY;
    return CreateFontIndirectW(&font);
}

HFONT CreateIconFont(int pixels) {
    LOGFONTW font{};
    font.lfHeight = -pixels;
    font.lfCharSet = DEFAULT_CHARSET;
    font.lfQuality = ANTIALIASED_QUALITY;
    wcscpy_s(font.lfFaceName, g_iconFont);
    return CreateFontIndirectW(&font);
}

// ---------------------------------------------------------------------------
// Drawing

void FillRound(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border) {
    Gdiplus::Graphics g(dc);
    Prepare(g);
    float w = static_cast<float>(rect.right - rect.left), h = static_cast<float>(rect.bottom - rect.top);
    if (fill != CLR_NONE) {
        Gdiplus::GraphicsPath path;
        RoundPath(path, Gdiplus::RectF(static_cast<float>(rect.left), static_cast<float>(rect.top), w, h),
                  static_cast<float>(radius));
        Gdiplus::SolidBrush brush(ToColor(fill));
        g.FillPath(&brush, &path);
    }
    if (border != CLR_NONE) {
        Gdiplus::GraphicsPath path;
        RoundPath(path, Gdiplus::RectF(rect.left + 0.5f, rect.top + 0.5f, w - 1, h - 1), radius - 0.5f);
        Gdiplus::Pen pen(ToColor(border), 1.0f);
        g.DrawPath(&pen, &path);
    }
}

void StrokeRound(HDC dc, const RECT& rect, int radius, COLORREF color, float width) {
    Gdiplus::Graphics g(dc);
    Prepare(g);
    float inset = width / 2;
    Gdiplus::GraphicsPath path;
    RoundPath(path,
              Gdiplus::RectF(rect.left + inset, rect.top + inset, rect.right - rect.left - width,
                             rect.bottom - rect.top - width),
              radius - inset);
    Gdiplus::Pen pen(ToColor(color), width);
    g.DrawPath(&pen, &path);
}

void FillCircle(HDC dc, float cx, float cy, float radius, COLORREF fill) {
    Gdiplus::Graphics g(dc);
    Prepare(g);
    Gdiplus::SolidBrush brush(ToColor(fill));
    g.FillEllipse(&brush, cx - radius, cy - radius, radius * 2, radius * 2);
}

void DrawGlyph(HDC dc, HFONT font, wchar_t glyph, const RECT& rect, COLORREF color) {
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT r = rect;
    DrawTextW(dc, &glyph, 1, &r, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX | DT_NOCLIP);
    SelectObject(dc, old);
}

// ---------------------------------------------------------------------------
// Dialogs

void ThemeDialog(HWND dialog) {
    auto* info = new DialogInfo;
    RECT client;
    GetClientRect(dialog, &client);
    info->footerTop = client.bottom;
    LRESULT defId = SendMessageW(dialog, DM_GETDEFID, 0, 0);
    if (HIWORD(defId) == DC_HASDEFID) info->primaryId = LOWORD(defId);

    // The footer band holds OK/Cancel, with the same margin above them as below.
    for (int id : {IDOK, IDCANCEL}) {
        if (HWND button = GetDlgItem(dialog, id)) {
            RECT r;
            GetWindowRect(button, &r);
            MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&r), 2);
            info->footerTop = std::min<int>(info->footerTop, r.top - (client.bottom - r.bottom));
        }
    }

    HFONT dialogFont = reinterpret_cast<HFONT>(SendMessageW(dialog, WM_GETFONT, 0, 0));
    LOGFONTW heading{};
    GetObjectW(dialogFont, sizeof(heading), &heading);
    heading.lfWeight = FW_SEMIBOLD;
    heading.lfHeight = heading.lfHeight * 12 / 10;
    info->headingFont = CreateFontIndirectW(&heading);

    for (HWND child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        int id = GetDlgCtrlID(child);
        if (id >= IDC_HEADING_FIRST && id <= IDC_HEADING_LAST)
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(info->headingFont), FALSE);
        if (IsClass(child, WC_EDITW) || IsClass(child, WC_COMBOBOXW))
            ApplyControlTheme(child, nullptr, L"DarkMode_CFD");
        else if (IsClass(child, WC_BUTTONW))
            ApplyControlTheme(child, nullptr, L"DarkMode_Explorer");
    }

    SetWindowSubclass(dialog, DialogSubclass, 1, reinterpret_cast<DWORD_PTR>(info));
    ApplyWindowFrame(dialog, Colors().card);
}

}  // namespace ui
