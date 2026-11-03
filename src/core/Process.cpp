#include "Process.h"

#include <windows.h>

#include <memory>

namespace wslmgr {
namespace {

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE h) : h_(h == INVALID_HANDLE_VALUE ? nullptr : h) {}
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    ~UniqueHandle() { reset(); }
    HANDLE get() const { return h_; }
    HANDLE* put() {
        reset();
        return &h_;
    }
    void reset() {
        if (h_) CloseHandle(h_);
        h_ = nullptr;
    }

private:
    HANDLE h_ = nullptr;
};

// Splits raw process output into lines. wsl.exe redraws progress with bare '\r',
// so a line ended by '\r' alone is reported as a progress line.
class LineSplitter {
public:
    LineSplitter(const OutputFn& onLine, std::wstring& collected) : onLine_(onLine), collected_(collected) {}

    void Feed(const char* data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            char c = data[i];
            if (c == '\0') continue;  // tolerate UTF-16 output if WSL_UTF8 is ignored
            if (pendingCr_) {
                pendingCr_ = false;
                if (c == '\n') {
                    Emit(false);
                    continue;
                }
                Emit(true);
            }
            if (c == '\r') {
                pendingCr_ = true;
            } else if (c == '\n') {
                Emit(false);
            } else {
                line_.push_back(c);
            }
        }
    }

    void Finish() {
        if (pendingCr_ || !line_.empty()) Emit(false);
        pendingCr_ = false;
    }

private:
    void Emit(bool progress) {
        std::wstring text = Utf8ToWide(line_);
        line_.clear();
        if (!progress) {
            collected_ += text;
            collected_ += L'\n';
        }
        if (onLine_ && !Trim(text).empty()) onLine_(text, progress);
    }

    const OutputFn& onLine_;
    std::wstring& collected_;
    std::string line_;
    bool pendingCr_ = false;
};

std::wstring BuildCommandLine(const std::wstring& exe, const std::vector<std::wstring>& args) {
    std::wstring cmd = QuoteArg(exe);
    for (const auto& arg : args) {
        cmd += L' ';
        cmd += QuoteArg(arg);
    }
    return cmd;
}

}  // namespace

std::wstring QuoteArg(std::wstring_view arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) return std::wstring(arg);
    std::wstring out = L"\"";
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++backslashes;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        out.push_back(arg[i]);
    }
    out.push_back(L'"');
    return out;
}

ProcessResult RunProcess(const std::wstring& exe, const std::vector<std::wstring>& args, const OutputFn& onLine) {
    // Ask wsl.exe for UTF-8 instead of UTF-16 output. Inherited by the child.
    SetEnvironmentVariableW(L"WSL_UTF8", L"1");

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    UniqueHandle readPipe, writePipe;
    if (!CreatePipe(readPipe.put(), writePipe.put(), &sa, 0)) ThrowLastError(L"Could not create a pipe");
    SetHandleInformation(readPipe.get(), HANDLE_FLAG_INHERIT, 0);

    UniqueHandle nul(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0,
                                 nullptr));

    // Restrict inheritance to exactly these handles so concurrent calls on other
    // threads cannot leak their pipe ends into this child (which would delay EOF).
    HANDLE inherit[] = {nul.get(), writePipe.get()};
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<char> attrBuffer(attrSize);
    auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuffer.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize)) ThrowLastError(L"Could not prepare a process");
    std::unique_ptr<_PROC_THREAD_ATTRIBUTE_LIST, decltype(&DeleteProcThreadAttributeList)> attrsGuard(
        attrs, DeleteProcThreadAttributeList);
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr,
                                   nullptr))
        ThrowLastError(L"Could not prepare a process");

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul.get();
    si.StartupInfo.hStdOutput = writePipe.get();
    si.StartupInfo.hStdError = writePipe.get();
    si.lpAttributeList = attrs;

    std::wstring cmd = BuildCommandLine(exe, args);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                        nullptr, nullptr, &si.StartupInfo, &pi))
        ThrowLastError(L"Could not start " + exe);

    UniqueHandle process(pi.hProcess);
    UniqueHandle thread(pi.hThread);
    writePipe.reset();  // so ReadFile sees EOF once the child exits
    nul.reset();

    ProcessResult result;
    LineSplitter splitter(onLine, result.output);
    char buffer[4096];
    DWORD read = 0;
    // ReadFile fails with ERROR_BROKEN_PIPE once the child has exited and the pipe is drained.
    while (ReadFile(readPipe.get(), buffer, sizeof(buffer), &read, nullptr)) splitter.Feed(buffer, read);
    splitter.Finish();

    WaitForSingleObject(process.get(), INFINITE);
    GetExitCodeProcess(process.get(), &result.exitCode);
    return result;
}

void LaunchInNewConsole(const std::wstring& exe, const std::vector<std::wstring>& args) {
    std::wstring cmd = BuildCommandLine(exe, args);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si,
                        &pi))
        ThrowLastError(L"Could not start " + exe);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

}  // namespace wslmgr
