#pragma once

// Fluent (Microsoft 365 style) look for the Win32 GUI: light and dark palettes,
// anti-aliased drawing helpers, and theming for the dialogs.

#include <windows.h>
#include <commctrl.h>

namespace ui {

enum class ThemeMode : DWORD { System = 0, Light = 1, Dark = 2 };

struct Palette {
    COLORREF chrome;       // window background, title bar, dialog footers
    COLORREF chromeHover;  // hover on chrome (tabs)
    COLORREF card;         // ribbon, list, panes, dialog bodies
    COLORREF border;       // card outlines
    COLORREF divider;
    COLORREF text, textSecondary, textDisabled;
    COLORREF hover, pressed, checked;  // ribbon buttons on a card
    COLORREF accent, accentHover, accentPressed, onAccent;  // filled (primary) controls
    COLORREF accentText;                                    // accent used for text and icons
    COLORREF control, controlHover, controlPressed, controlBorder, controlStrongBorder;
    COLORREF field;  // edit boxes and drop-down lists
    COLORREF success, warning, danger;
    COLORREF infoBack;
};

// Call once before creating any window (the menu theme is fixed per process).
void InitTheme(ThemeMode mode);
void ShutdownTheme();

void SetThemeMode(ThemeMode mode);
ThemeMode GetThemeMode();
// Re-reads Windows' app theme. Returns true if the effective theme changed.
bool RefreshTheme();
bool IsDark();
const Palette& Colors();

HBRUSH Brush(COLORREF color);  // cached; don't delete
COLORREF Blend(COLORREF from, COLORREF to, double amount);

// Dark title bar and a caption colour that matches the window body (Windows 11).
void ApplyWindowFrame(HWND window, COLORREF caption);
// Switches a common control between its light and dark visual styles.
// A null class name means the control's default style.
void ApplyControlTheme(HWND control, const wchar_t* lightClass, const wchar_t* darkClass);
// Makes already-created popup menus pick up a theme change.
void FlushMenus();

// "Segoe Fluent Icons" on Windows 11, "Segoe MDL2 Assets" on Windows 10.
const wchar_t* IconFontName();
HFONT CreateUiFont(UINT dpi, double points, int weight = FW_NORMAL);
HFONT CreateIconFont(int pixels);

// Anti-aliased shapes. Pass CLR_NONE as the border for a fill only.
void FillRound(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border = CLR_NONE);
void StrokeRound(HDC dc, const RECT& rect, int radius, COLORREF color, float width);
void FillCircle(HDC dc, float cx, float cy, float radius, COLORREF fill);
void DrawGlyph(HDC dc, HFONT font, wchar_t glyph, const RECT& rect, COLORREF color);

// Call from WM_INITDIALOG. Paints the dialog, its buttons, check boxes and radio
// buttons in the current theme. Statics with IDs in the IDC_HEADING range become
// section headings; those in the IDC_HINT range use secondary text.
void ThemeDialog(HWND dialog);

// A themed replacement for MessageBoxW. Understands MB_OK, MB_OKCANCEL, MB_YESNO,
// the MB_ICON* flags and MB_DEFBUTTON2, and returns IDOK, IDCANCEL, IDYES or IDNO.
// Escape or the close box answers Cancel, or No when there is no Cancel button.
int ShowMessage(HWND owner, const wchar_t* text, const wchar_t* title, UINT flags);

}  // namespace ui
