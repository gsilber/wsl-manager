#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <string_view>

namespace wslmgr {

// Error raised by any failed WSL management operation. The message is meant
// to be shown to the user as-is.
class WslError : public std::exception {
public:
    explicit WslError(std::wstring message) : message_(std::move(message)) {}
    const std::wstring& message() const noexcept;
    const char* what() const noexcept override;

private:
    std::wstring message_;
};

// Receives one line of output from a long-running operation. `progress` is true
// for transient status lines (terminated by a bare carriage return) that a UI
// should overwrite in place rather than append to a log.
using OutputFn = std::function<void(const std::wstring& line, bool progress)>;

std::wstring Utf8ToWide(std::string_view text);
std::string WideToUtf8(std::wstring_view text);
std::wstring Trim(std::wstring_view text);
bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b);
bool EndsWithIgnoreCase(std::wstring_view text, std::wstring_view suffix);
std::wstring FormatBytes(uint64_t bytes);
std::wstring Timestamp();  // local time as YYYYMMDD-HHMMSS
std::wstring StripLongPathPrefix(std::wstring_view path);
std::wstring LastErrorMessage(unsigned long error);
[[noreturn]] void ThrowLastError(const std::wstring& what);  // throws "<what>: <GetLastError() text>"

}  // namespace wslmgr
