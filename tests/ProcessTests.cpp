#include "TestSupport.h"

#include "core/Process.h"

#include <shellapi.h>

using namespace wslmgr;

namespace {

// Parses `QuoteArg(arg)` back the way a child process would.
std::wstring RoundTrip(const std::wstring& arg) {
    std::wstring commandLine = L"program.exe " + QuoteArg(arg);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(commandLine.c_str(), &argc);
    std::wstring parsed = argc == 2 ? argv[1] : L"<wrong argument count: " + std::to_wstring(argc) + L">";
    LocalFree(argv);
    return parsed;
}

// Runs fake_wsl.exe --emit <hex of bytes>, which writes `bytes` verbatim to stdout.
ProcessResult Emit(const std::string& bytes, const OutputFn& onLine) {
    std::wstring hex;
    for (unsigned char c : bytes) {
        wchar_t pair[3];
        swprintf_s(pair, L"%02X", c);
        hex += pair;
    }
    return RunProcess(test::FakeWslPath().wstring(), {L"--emit", hex}, onLine);
}

}  // namespace

TEST(QuoteArg, LeavesSimpleArgumentsAlone) {
    EXPECT_EQ(QuoteArg(L"simple"), L"simple");
    EXPECT_EQ(QuoteArg(L"C:\\path\\file.txt"), L"C:\\path\\file.txt");
}

TEST(QuoteArg, QuotesWhenNeeded) {
    EXPECT_EQ(QuoteArg(L""), L"\"\"");
    EXPECT_EQ(QuoteArg(L"a b"), L"\"a b\"");
    EXPECT_EQ(QuoteArg(L"say \"hi\""), L"\"say \\\"hi\\\"\"");
    EXPECT_EQ(QuoteArg(L"C:\\dir with space\\"), L"\"C:\\dir with space\\\\\"");
}

TEST(QuoteArg, RoundTripsThroughCommandLineParsing) {
    for (const std::wstring& arg : {std::wstring(L""), std::wstring(L"plain"), std::wstring(L"two words"),
                                    std::wstring(L"tab\there"), std::wstring(L"quote\"inside"),
                                    std::wstring(L"back\\\\\"slashes"), std::wstring(L"trailing\\ \\"),
                                    std::wstring(L"C:\\Program Files\\x\\"), std::wstring(L"\\\\server\\share")}) {
        EXPECT_EQ(RoundTrip(arg), arg);
    }
}

TEST(RunProcess, SplitsLinesAndKeepsAnUnterminatedLastLine) {
    test::Captured captured;
    ProcessResult result = Emit("first\r\nsecond\nthird", captured.Fn());
    EXPECT_EQ(result.exitCode, 0u);
    EXPECT_EQ(result.output, L"first\nsecond\nthird\n");
    EXPECT_EQ(captured.lines, (std::vector<std::wstring>{L"first", L"second", L"third"}));
    EXPECT_TRUE(captured.progress.empty());
}

TEST(RunProcess, ReportsCarriageReturnLinesAsProgress) {
    test::Captured captured;
    ProcessResult result = Emit("10%\r20%\rdone\r\n", captured.Fn());
    EXPECT_EQ(captured.progress, (std::vector<std::wstring>{L"10%", L"20%"}));
    EXPECT_EQ(captured.lines, (std::vector<std::wstring>{L"done"}));
    EXPECT_EQ(result.output, L"done\n");  // progress is not part of the collected output
}

TEST(RunProcess, TrailingCarriageReturnEndsAnOrdinaryLine) {
    test::Captured captured;
    Emit("last\r", captured.Fn());
    EXPECT_EQ(captured.lines, (std::vector<std::wstring>{L"last"}));
}

TEST(RunProcess, ToleratesUtf16OutputAndSkipsBlankLines) {
    test::Captured captured;
    ProcessResult result = Emit(std::string("h\0i\0\r\0\n\0\r\0\n\0", 12), captured.Fn());
    EXPECT_EQ(captured.lines, (std::vector<std::wstring>{L"hi"}));
    EXPECT_EQ(result.output, L"hi\n\n");
}

TEST(RunProcess, DecodesUtf8) {
    test::Captured captured;
    Emit("caf\xC3\xA9\n", captured.Fn());
    EXPECT_EQ(captured.lines, (std::vector<std::wstring>{L"caf\u00e9"}));
}

TEST(RunProcess, WorksWithoutACallbackAndReportsTheExitCode) {
    ProcessResult result = RunProcess(test::FakeWslPath().wstring(), {L"--no-such-command"}, nullptr);
    EXPECT_EQ(result.exitCode, 0xFFFFFFFFu);
    EXPECT_NE(result.output.find(L"Invalid command line argument"), std::wstring::npos);
}

TEST(RunProcess, ThrowsWhenTheProgramCannotStart) {
    try {
        RunProcess(L"C:\\does\\not\\exist.exe", {}, nullptr);
        FAIL() << "expected a WslError";
    } catch (const WslError& e) {
        EXPECT_EQ(e.message().rfind(L"Could not start C:\\does\\not\\exist.exe: ", 0), 0u) << e.message();
    }
}

TEST(LaunchInNewConsole, StartsTheProgram) {
    EXPECT_NO_THROW(LaunchInNewConsole(test::FakeWslPath().wstring(), {L"--emit", L""}));
}

TEST(LaunchInNewConsole, ThrowsWhenTheProgramCannotStart) {
    EXPECT_THROW(LaunchInNewConsole(L"C:\\does\\not\\exist.exe", {}), WslError);
}
