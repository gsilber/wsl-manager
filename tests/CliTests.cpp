#include "TestSupport.h"

#include "cli/Cli.h"

using namespace wslmgr;
namespace fs = std::filesystem;

namespace {

bool Contains(const std::wstring& text, std::wstring_view part) { return text.find(part) != std::wstring::npos; }

}  // namespace

class CliTest : public test::WslTest {
protected:
    void SetUp() override {
        WslTest::SetUp();
        SetDefaultRoot(Path(L"root").wstring());
    }
};

// ---------------------------------------------------------------------------
// General

TEST_F(CliTest, HelpAndUnknownCommands) {
    auto none = RunCli({});
    EXPECT_EQ(none.exitCode, 1);
    EXPECT_TRUE(Contains(none.out, L"Usage: wslmgr <command>"));

    for (const wchar_t* flag : {L"help", L"--help", L"-h", L"/?"}) {
        auto help = RunCli({flag});
        EXPECT_EQ(help.exitCode, 0) << WideToUtf8(flag);
        EXPECT_TRUE(Contains(help.out, L"Usage: wslmgr <command>"));
    }

    auto unknown = RunCli({L"frobnicate"});
    EXPECT_EQ(unknown.exitCode, 1);
    EXPECT_EQ(unknown.err, L"error: Unknown command 'frobnicate'. Run 'wslmgr help' for usage.\n");
}

TEST_F(CliTest, OptionErrors) {
    auto unknownOption = RunCli({L"list", L"--bogus"});
    EXPECT_EQ(unknownOption.exitCode, 1);
    EXPECT_TRUE(Contains(unknownOption.err, L"Unknown option --bogus"));

    auto missingValue = RunCli({L"create", L"x", L"--distro"});
    EXPECT_TRUE(Contains(missingValue.err, L"Option --distro needs a value."));

    auto wrongCount = RunCli({L"info"});
    EXPECT_TRUE(Contains(wrongCount.err, L"Usage: wslmgr info <name>"));
}

// ---------------------------------------------------------------------------
// Listing

TEST_F(CliTest, ListWhenEmpty) {
    auto result = RunCli({L"list"});
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(result.out, L"No WSL instances are installed.\n");
}

TEST_F(CliTest, ListShowsInstances) {
    std::wstring id = AddDistro({.name = L"Ubuntu", .diskContent = std::string(2048, 'x')});
    AddDistro({.name = L"Old", .version = 1, .running = true});
    AddDistro({.name = L"docker-desktop"});
    MakeDefault(id);
    auto spaces = [](size_t n) { return std::wstring(n, L' '); };

    auto result = RunCli({L"ls"});
    EXPECT_EQ(result.exitCode, 0);
    // Name column is 16 wide here ("docker-desktop" + 2).
    EXPECT_TRUE(Contains(result.out, L"  NAME" + spaces(12) + L"STATE     WSL  PROTECTED  SIZE       LOCATION\n"));
    EXPECT_TRUE(Contains(result.out, L"  Old" + spaces(13) + L"Running   1    " + spaces(11) + L"-" + spaces(10) +
                                         Path(L"instances\\Old").wstring()));
    EXPECT_TRUE(Contains(result.out, L"  docker-desktop  Stopped   2    yes" + spaces(8) + L"4 B" + spaces(8)));
    EXPECT_TRUE(Contains(result.out, L"* Ubuntu" + spaces(10) + L"Stopped   2    " + spaces(11) + L"2.0 KB     "));
    EXPECT_TRUE(Contains(result.out, L"Default storage folder: " + Path(L"root").wstring()));
}

TEST_F(CliTest, ListOnline) {
    auto result = RunCli({L"list", L"--online"});
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(result.out, L"NAME    DESCRIPTION\nUbuntu  Ubuntu\nDebian  Debian GNU/Linux\n");
}

TEST_F(CliTest, Info) {
    std::wstring id = AddDistro({.name = L"Dev", .diskContent = "abc", .uid = 1000});
    MakeDefault(id);
    auto result = RunCli({L"info", L"dev"});
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_TRUE(Contains(result.out, L"Name:          Dev\n"));
    EXPECT_TRUE(Contains(result.out, L"State:         Stopped\n"));
    EXPECT_TRUE(Contains(result.out, L"WSL version:   2\n"));
    EXPECT_TRUE(Contains(result.out, L"Default:       yes\n"));
    EXPECT_TRUE(Contains(result.out, L"Protected:     no\n"));
    EXPECT_TRUE(Contains(result.out, L"OS:            ubuntu 24.04\n"));
    EXPECT_TRUE(Contains(result.out, L"Default UID:   1000\n"));
    EXPECT_TRUE(Contains(result.out, L"Virtual disk:  " + Path(L"instances\\Dev\\ext4.vhdx").wstring() + L" (3 B)\n"));
    EXPECT_TRUE(Contains(result.out, L"Registry ID:   " + id));

    AddDistro({.name = L"Old", .version = 1});
    auto old = RunCli({L"info", L"Old"});
    EXPECT_TRUE(Contains(old.out, L"Default:       no\n"));
    EXPECT_FALSE(Contains(old.out, L"Virtual disk"));

    auto missing = RunCli({L"info", L"Nope"});
    EXPECT_EQ(missing.exitCode, 1);
    EXPECT_EQ(missing.err, L"error: There is no WSL instance named 'Nope'.\n");
}

// ---------------------------------------------------------------------------
// Creating

TEST_F(CliTest, CreateCleanInstall) {
    auto result = RunCli({L"create", L"Fresh", L"--distro", L"Debian", L"--web-download", L"--version", L"1"});
    EXPECT_EQ(result.exitCode, 0) << WideToUtf8(result.err);
    EXPECT_TRUE(Called(L"--install\tDebian\t--name\tFresh\t--location\t" + Path(L"root\\Fresh").wstring() +
                       L"\t--no-launch\t--version\t1\t--web-download"));
    EXPECT_TRUE(Contains(result.out, L"  Installed 'Fresh'"));
}

TEST_F(CliTest, CreateDefaultsToWsl2) {
    auto result = RunCli({L"create", L"Fresh", L"--distro", L"Ubuntu"});
    EXPECT_EQ(result.exitCode, 0) << WideToUtf8(result.err);
    EXPECT_TRUE(Called(L"--no-launch\t--version\t2"));
}

TEST_F(CliTest, CreateFromBackupAtAChosenLocation) {
    WriteFile(Path(L"dev.tar"), "tar");
    auto result = RunCli({L"create", L"Dev", L"--from-backup", Path(L"dev.tar").wstring(), L"--location",
                          Path(L"here").wstring(), L"--version", L"2"});
    EXPECT_EQ(result.exitCode, 0) << WideToUtf8(result.err);
    EXPECT_EQ(FindDistro(L"Dev")->basePath, Path(L"here").wstring());
}

TEST_F(CliTest, CreateByCloning) {
    AddDistro({.name = L"Src"});
    auto result = RunCli({L"create", L"Copy", L"--clone", L"Src"});
    EXPECT_EQ(result.exitCode, 0) << WideToUtf8(result.err);
    EXPECT_TRUE(FindDistro(L"Copy"));
}

TEST_F(CliTest, CreateCloneOfARunningInstanceAsks) {
    AddDistro({.name = L"Src", .running = true});

    auto declined = RunCli({L"create", L"Copy", L"--clone", L"Src"}, "n\n");
    EXPECT_EQ(declined.exitCode, 1);
    EXPECT_TRUE(Contains(declined.out, L"'Src' is running and will be stopped. Continue? [y/N] "));
    EXPECT_TRUE(Contains(declined.err, L"Cancelled. Pass --yes"));

    auto accepted = RunCli({L"create", L"Copy", L"--clone", L"Src"}, "y\r\n");
    EXPECT_EQ(accepted.exitCode, 0) << WideToUtf8(accepted.err);

    AddDistro({.name = L"Src2", .running = true});
    auto forced = RunCli({L"create", L"Copy2", L"--clone", L"Src2", L"-y"});
    EXPECT_EQ(forced.exitCode, 0) << WideToUtf8(forced.err);
}

TEST_F(CliTest, CreateArgumentErrors) {
    EXPECT_TRUE(Contains(RunCli({L"create", L"X"}).err, L"Specify exactly one of"));
    EXPECT_TRUE(Contains(RunCli({L"create", L"X", L"--distro", L"a", L"--clone", L"b"}).err, L"Specify exactly one of"));
    EXPECT_TRUE(Contains(RunCli({L"create", L"X", L"--distro", L"a", L"--version", L"3"}).err,
                         L"--version must be 1 or 2."));
    EXPECT_TRUE(Contains(RunCli({L"create"}).err, L"Usage: wslmgr create <name>"));
}

// ---------------------------------------------------------------------------
// Backups

TEST_F(CliTest, BackupToTheDefaultLocation) {
    AddDistro({.name = L"Dev"});
    auto result = RunCli({L"backup", L"Dev"});
    EXPECT_EQ(result.exitCode, 0) << WideToUtf8(result.err);
    ASSERT_TRUE(fs::exists(Path(L"root\\Backups")));
    auto entries = std::vector<fs::path>(fs::directory_iterator(Path(L"root\\Backups")), {});
    ASSERT_EQ(entries.size(), 2u);  // backup + metadata
    EXPECT_TRUE(Called(L"--format\tvhd"));
}

TEST_F(CliTest, BackupFormatsAndOverwriting) {
    AddDistro({.name = L"Dev"});
    auto byExtension = RunCli({L"backup", L"Dev", L"--file", Path(L"b.tar.xz").wstring()});
    EXPECT_EQ(byExtension.exitCode, 0);
    EXPECT_TRUE(Called(L"--format\ttar.xz"));

    auto exists = RunCli({L"backup", L"Dev", L"--file", Path(L"b.tar.xz").wstring()});
    EXPECT_EQ(exists.exitCode, 1);
    EXPECT_TRUE(Contains(exists.err, L"already exists. Pass --force to overwrite it."));

    auto forced = RunCli({L"backup", L"Dev", L"--file", Path(L"b.tar.xz").wstring(), L"--format", L"tgz", L"--force"});
    EXPECT_EQ(forced.exitCode, 0);
    EXPECT_TRUE(Called(L"--format\ttar.gz"));

    auto badFormat = RunCli({L"backup", L"Dev", L"--format", L"zip"});
    EXPECT_TRUE(Contains(badFormat.err, L"Unknown format 'zip'"));
}

// ---------------------------------------------------------------------------
// Removing

TEST_F(CliTest, RemoveAsksFirst) {
    AddDistro({.name = L"Dev", .diskContent = "1234"});
    auto declined = RunCli({L"remove", L"Dev"});  // no input: treated as "no"
    EXPECT_EQ(declined.exitCode, 1);
    EXPECT_TRUE(Contains(declined.out, L"Permanently delete 'Dev' and its virtual disk (4 B)? No backup will be made."));
    EXPECT_TRUE(Contains(declined.err, L"Cancelled. Pass --yes to remove without asking."));
    EXPECT_TRUE(FindDistro(L"Dev"));

    auto accepted = RunCli({L"rm", L"Dev", L"--delete-folder"}, "yes\n");
    EXPECT_EQ(accepted.exitCode, 0);
    EXPECT_FALSE(FindDistro(L"Dev"));
    EXPECT_FALSE(fs::exists(Path(L"instances\\Dev")));
}

TEST_F(CliTest, PromptsWithoutAnyStandardInputMeanNo) {
    AddDistro({.name = L"Dev"});
    auto result = RunCli({L"remove", L"Dev"}, nullptr);
    EXPECT_EQ(result.exitCode, 1);
    EXPECT_TRUE(Contains(result.err, L"Cancelled."));
    EXPECT_TRUE(FindDistro(L"Dev"));
}

TEST_F(CliTest, RemoveWithBackup) {
    AddDistro({.name = L"Old", .version = 1});
    auto result = RunCli({L"remove", L"Old", L"--backup", L"--file", Path(L"old.tar").wstring()}, "Y\n");
    EXPECT_EQ(result.exitCode, 0) << WideToUtf8(result.err);
    EXPECT_TRUE(Contains(result.out, L"Permanently delete 'Old' and its virtual disk after backing it up?"));
    EXPECT_TRUE(fs::exists(Path(L"old.tar")));

    AddDistro({.name = L"Dev"});
    auto defaults = RunCli({L"remove", L"Dev", L"--backup", L"--yes"});
    EXPECT_EQ(defaults.exitCode, 0) << WideToUtf8(defaults.err);
    EXPECT_TRUE(fs::exists(Path(L"root\\Backups")));
}

// ---------------------------------------------------------------------------
// Other commands

TEST_F(CliTest, MoveAndRename) {
    AddDistro({.name = L"Dev"});
    auto moved = RunCli({L"move", L"Dev", Path(L"moved").wstring()});
    EXPECT_EQ(moved.exitCode, 0) << WideToUtf8(moved.err);
    EXPECT_EQ(FindDistro(L"Dev")->basePath, Path(L"moved").wstring());

    auto renamed = RunCli({L"rename", L"Dev", L"Prod"});
    EXPECT_EQ(renamed.exitCode, 0) << WideToUtf8(renamed.err);
    EXPECT_TRUE(FindDistro(L"Prod"));

    EXPECT_TRUE(Contains(RunCli({L"move", L"Prod"}).err, L"Usage: wslmgr move <name> <folder>"));
    EXPECT_TRUE(Contains(RunCli({L"rename", L"Prod"}).err, L"Usage: wslmgr rename <name> <new-name>"));
}

TEST_F(CliTest, ConfigShowsAndSetsTheDefaultFolder) {
    auto show = RunCli({L"config"});
    EXPECT_EQ(show.out, L"Default storage folder: " + Path(L"root").wstring() + L"\n");
    auto set = RunCli({L"config", L"--default-folder", Path(L"other").wstring()});
    EXPECT_EQ(set.out, L"Default storage folder: " + Path(L"other").wstring() + L"\n");
    EXPECT_EQ(GetDefaultRoot(), Path(L"other").wstring());
}

TEST_F(CliTest, SimpleCommands) {
    AddDistro({.name = L"Dev", .running = true});
    EXPECT_EQ(RunCli({L"set-default", L"Dev"}).exitCode, 0);
    EXPECT_TRUE(FindDistro(L"Dev")->isDefault);
    EXPECT_EQ(RunCli({L"set-user", L"Dev", L"alice"}).exitCode, 0);
    EXPECT_EQ(FindDistro(L"Dev")->defaultUid, 1000u);
    EXPECT_EQ(RunCli({L"stop", L"Dev"}).exitCode, 0);
    EXPECT_FALSE(FindDistro(L"Dev", true)->running);
    EXPECT_EQ(RunCli({L"shell", L"Dev"}).exitCode, 0);
    EXPECT_EQ(RunCli({L"shutdown"}).exitCode, 0);
    EXPECT_TRUE(Called(L"--shutdown"));

    EXPECT_TRUE(Contains(RunCli({L"set-default"}).err, L"Usage: wslmgr set-default <name>"));
    EXPECT_TRUE(Contains(RunCli({L"set-user", L"Dev"}).err, L"Usage: wslmgr set-user"));
    EXPECT_TRUE(Contains(RunCli({L"stop"}).err, L"Usage: wslmgr stop <name>"));
    EXPECT_TRUE(Contains(RunCli({L"shell"}).err, L"Usage: wslmgr shell <name>"));
    EXPECT_TRUE(Contains(RunCli({L"shutdown", L"extra"}).err, L"Usage: wslmgr shutdown"));
}

TEST_F(CliTest, ProtectAndUnprotect) {
    AddDistro({.name = L"Prod"});
    AddDistro({.name = L"docker-desktop"});

    auto protect = RunCli({L"protect", L"prod"});
    EXPECT_EQ(protect.exitCode, 0);
    EXPECT_EQ(protect.out, L"'Prod' is now protected.\n");
    EXPECT_TRUE(Contains(RunCli({L"info", L"Prod"}).out, L"Protected:     yes\n"));

    auto blocked = RunCli({L"remove", L"Prod", L"--yes"});
    EXPECT_EQ(blocked.exitCode, 1);
    EXPECT_TRUE(Contains(blocked.err, L"error: 'Prod' is protected, so WSL Manager won't remove it."));
    EXPECT_TRUE(FindDistro(L"Prod"));

    auto docker = RunCli({L"stop", L"docker-desktop"});  // protected without being asked
    EXPECT_EQ(docker.exitCode, 1);
    EXPECT_TRUE(Contains(docker.err, L"wslmgr unprotect docker-desktop"));

    auto unprotect = RunCli({L"unprotect", L"Prod"});
    EXPECT_EQ(unprotect.out, L"'Prod' is no longer protected.\n");
    EXPECT_EQ(RunCli({L"remove", L"Prod", L"--yes"}).exitCode, 0);

    EXPECT_TRUE(Contains(RunCli({L"protect"}).err, L"Usage: wslmgr protect <name>"));
    EXPECT_TRUE(Contains(RunCli({L"unprotect"}).err, L"Usage: wslmgr unprotect <name>"));
    EXPECT_TRUE(Contains(RunCli({L"protect", L"Nope"}).err, L"There is no WSL instance named 'Nope'."));
}

TEST_F(CliTest, ProgressIsDroppedWhenOutputIsNotAConsole) {
    AddDistro({.name = L"Dev"});
    SetFake(L"FAKE_WSL_PROGRESS", L"1");
    auto result = RunCli({L"backup", L"Dev", L"--file", Path(L"x.tar").wstring()});
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_FALSE(Contains(result.out, L"Exporting: 50%"));
}

// ---------------------------------------------------------------------------
// Console behavior: output through WriteConsole, progress redrawn in place,
// prompts answered from the keyboard.

class CliConsoleTest : public CliTest {
protected:
    // Runs the CLI with whatever standard handles the current ConsoleScope installed.
    int RunOnConsole(std::vector<std::wstring> args) {
        args.insert(args.begin(), L"wslmgr");
        std::vector<wchar_t*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        return ::RunCli(static_cast<int>(argv.size()), argv.data());
    }
};

TEST_F(CliConsoleTest, WritesToTheConsoleAndClearsProgress) {
    AddDistro({.name = L"Dev"});
    SetFake(L"FAKE_WSL_PROGRESS", L"1");
    test::ConsoleScope console;
    EXPECT_EQ(RunOnConsole({L"backup", L"Dev", L"--file", Path(L"x.tar").wstring()}), 0);
    std::wstring text = console.Text();
    EXPECT_TRUE(Contains(text, L"Exporting 'Dev'"));
    EXPECT_TRUE(Contains(text, L"Backup saved:"));
    EXPECT_FALSE(Contains(text, L"Exporting: 50%"));  // progress was overwritten and erased
}

TEST_F(CliConsoleTest, ErrorsClearPendingProgress) {
    AddDistro({.name = L"Dev"});
    SetFake(L"FAKE_WSL_PROGRESS", L"1");
    SetFake(L"FAKE_WSL_FAIL", L"--export");  // prints progress, then fails
    test::ConsoleScope console;
    EXPECT_EQ(RunOnConsole({L"backup", L"Dev", L"--file", Path(L"x.tar").wstring()}), 1);
    std::wstring text = console.Text();
    EXPECT_FALSE(Contains(text, L"Working: 50%"));
    EXPECT_TRUE(Contains(text, L"error: wsl.exe --export failed:"));
}

TEST_F(CliConsoleTest, PromptsReadTheKeyboard) {
    AddDistro({.name = L"Dev"});
    test::ConsoleScope console;
    console.Type(L"y\r");
    EXPECT_EQ(RunOnConsole({L"remove", L"Dev"}), 0);
    EXPECT_TRUE(Contains(console.Text(), L"Permanently delete 'Dev'"));
    EXPECT_FALSE(FindDistro(L"Dev"));
}
