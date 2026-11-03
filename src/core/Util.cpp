#include "Util.h"

#include <windows.h>

#include <cstdio>
#include <iterator>

namespace wslmgr {

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), len);
    return out;
}

std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                  nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), len, nullptr,
                        nullptr);
    return out;
}

std::wstring Trim(std::wstring_view text) {
    constexpr wchar_t kSpace[] = L" \t\r\n";
    size_t first = text.find_first_not_of(kSpace);
    if (first == std::wstring_view::npos) return {};
    size_t last = text.find_last_not_of(kSpace);
    return std::wstring(text.substr(first, last - first + 1));
}

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()),
                                TRUE) == CSTR_EQUAL;
}

bool EndsWithIgnoreCase(std::wstring_view text, std::wstring_view suffix) {
    return text.size() >= suffix.size() && EqualsIgnoreCase(text.substr(text.size() - suffix.size()), suffix);
}

std::wstring FormatBytes(uint64_t bytes) {
    static constexpr const wchar_t* kUnits[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(kUnits)) {
        value /= 1024.0;
        ++unit;
    }
    wchar_t buf[32];
    swprintf_s(buf, unit == 0 ? L"%.0f %s" : L"%.1f %s", value, kUnits[unit]);
    return buf;
}

std::wstring Timestamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[32];
    swprintf_s(buf, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buf;
}

std::wstring StripLongPathPrefix(std::wstring_view path) {
    constexpr std::wstring_view kUnc = L"\\\\?\\UNC\\";
    constexpr std::wstring_view kLocal = L"\\\\?\\";
    if (path.substr(0, kUnc.size()) == kUnc) return L"\\\\" + std::wstring(path.substr(kUnc.size()));
    if (path.substr(0, kLocal.size()) == kLocal) return std::wstring(path.substr(kLocal.size()));
    return std::wstring(path);
}

std::wstring LastErrorMessage(unsigned long error) {
    wchar_t* buffer = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring message = len ? Trim(std::wstring_view(buffer, len)) : L"Error " + std::to_wstring(error);
    LocalFree(buffer);
    return message;
}

}  // namespace wslmgr
