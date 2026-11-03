#pragma once

#include "Util.h"

#include <vector>

namespace wslmgr {

struct ProcessResult {
    unsigned long exitCode = 0;
    std::wstring output;  // all non-progress lines, '\n'-separated
};

// Quotes one argument so CommandLineToArgvW / the MSVC runtime parse it back unchanged.
std::wstring QuoteArg(std::wstring_view arg);

// Runs `exe` hidden, with stdin from NUL and stdout+stderr captured. Each line of
// output is passed to `onLine` (if set) as it arrives. Blocks until exit.
ProcessResult RunProcess(const std::wstring& exe, const std::vector<std::wstring>& args, const OutputFn& onLine);

// Starts `exe` in a new console window and returns immediately.
void LaunchInNewConsole(const std::wstring& exe, const std::vector<std::wstring>& args);

}  // namespace wslmgr
