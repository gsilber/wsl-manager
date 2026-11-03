#include "TestSupport.h"

#include <array>
#include <chrono>
#include <memory>
#include <thread>

using namespace wslmgr;
namespace fs = std::filesystem;

namespace {

// Asserts that `action` throws a WslError whose message contains `text`.
template <typename Action>
void ExpectError(Action&& action, std::wstring_view text) {
    try {
        action();
        ADD_FAILURE() << "expected a WslError containing: " << WideToUtf8(text);
    } catch (const WslError& e) {
        EXPECT_NE(e.message().find(text), std::wstring::npos) << WideToUtf8(e.message());
    }
}

// Holds a file open without sharing, the way the WSL VM holds an attached disk.
class DiskLock {
public:
    explicit DiskLock(const fs::path& path)
        : handle_(CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr)) {}
    ~DiskLock() { Release(); }
    void Release() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    bool Held() const { return handle_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE handle_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Pure helpers

TEST(Formats, ArgumentsAndExtensions) {
    EXPECT_STREQ(FormatArg(BackupFormat::Vhd), L"vhd");
    EXPECT_STREQ(FormatArg(BackupFormat::Tar), L"tar");
    EXPECT_STREQ(FormatArg(BackupFormat::TarGz), L"tar.gz");
    EXPECT_STREQ(FormatArg(BackupFormat::TarXz), L"tar.xz");
    EXPECT_STREQ(FormatExtension(BackupFormat::Vhd), L".vhdx");
    EXPECT_STREQ(FormatExtension(BackupFormat::Tar), L".tar");
    EXPECT_STREQ(FormatExtension(BackupFormat::TarGz), L".tar.gz");
    EXPECT_STREQ(FormatExtension(BackupFormat::TarXz), L".tar.xz");
}

TEST(Formats, Parse) {
    EXPECT_EQ(ParseFormat(L"vhd"), BackupFormat::Vhd);
    EXPECT_EQ(ParseFormat(L"VHDX"), BackupFormat::Vhd);
    EXPECT_EQ(ParseFormat(L"tar"), BackupFormat::Tar);
    EXPECT_EQ(ParseFormat(L"tar.gz"), BackupFormat::TarGz);
    EXPECT_EQ(ParseFormat(L"tgz"), BackupFormat::TarGz);
    EXPECT_EQ(ParseFormat(L"tar.xz"), BackupFormat::TarXz);
    EXPECT_EQ(ParseFormat(L"txz"), BackupFormat::TarXz);
    EXPECT_EQ(ParseFormat(L"zip"), std::nullopt);
}

TEST(Formats, FromPath) {
    EXPECT_EQ(FormatFromPath(L"a.vhdx"), BackupFormat::Vhd);
    EXPECT_EQ(FormatFromPath(L"a.VHD"), BackupFormat::Vhd);
    EXPECT_EQ(FormatFromPath(L"a.tar.gz"), BackupFormat::TarGz);
    EXPECT_EQ(FormatFromPath(L"a.tgz"), BackupFormat::TarGz);
    EXPECT_EQ(FormatFromPath(L"a.tar.xz"), BackupFormat::TarXz);
    EXPECT_EQ(FormatFromPath(L"a.txz"), BackupFormat::TarXz);
    EXPECT_EQ(FormatFromPath(L"a.tar"), BackupFormat::Tar);
    EXPECT_EQ(FormatFromPath(L"no-extension"), BackupFormat::Tar);
}

TEST(Formats, Extensions) {
    EXPECT_EQ(StripBackupExtension(L"Ubuntu-20260101.tar.gz"), L"Ubuntu-20260101");
    EXPECT_EQ(StripBackupExtension(L"x.TXZ"), L"x");
    EXPECT_EQ(StripBackupExtension(L"x.vhdx"), L"x");
    EXPECT_EQ(StripBackupExtension(L"x.zip"), L"x.zip");
    EXPECT_EQ(WithBackupExtension(L"D:\\b\\x.tar", BackupFormat::Vhd), L"D:\\b\\x.vhdx");
    EXPECT_EQ(WithBackupExtension(L"D:\\b\\x", BackupFormat::TarXz), L"D:\\b\\x.tar.xz");
}

TEST(Distros, StateNames) {
    Distro d;
    EXPECT_EQ(StateName(d), L"Stopped");
    d.running = true;
    EXPECT_EQ(StateName(d), L"Running");
    d.registryState = 3;
    EXPECT_EQ(StateName(d), L"Installing");
    d.registryState = 4;
    EXPECT_EQ(StateName(d), L"Uninstalling");
    d.registryState = 5;
    EXPECT_EQ(StateName(d), L"Converting");
    d.registryState = 99;
    EXPECT_EQ(StateName(d), L"Unknown");
}

TEST(Distros, DefaultBackupFormat) {
    Distro d;
    EXPECT_EQ(DefaultBackupFormat(d), BackupFormat::Vhd);
    d.version = 1;
    EXPECT_EQ(DefaultBackupFormat(d), BackupFormat::Tar);
}

TEST(Paths, NormalizeFolder) {
    EXPECT_EQ(NormalizeFolder(L"C:\\WSL\\Ubuntu\\"), L"C:\\WSL\\Ubuntu");
    EXPECT_EQ(NormalizeFolder(L"  C:\\WSL//  "), L"C:\\WSL");
    EXPECT_EQ(NormalizeFolder(L"\\\\?\\D:\\WSL\\x"), L"D:\\WSL\\x");
    EXPECT_EQ(NormalizeFolder(L"C:\\"), L"C:\\");
    EXPECT_EQ(NormalizeFolder(L"C:\\a\\..\\b"), L"C:\\b");
    EXPECT_EQ(NormalizeFolder(L"relative"), (fs::current_path() / L"relative").wstring());
    EXPECT_THROW(NormalizeFolder(L"   "), WslError);
}

TEST(Paths, UnresolvablePathsAreKeptAsGiven) {
    std::wstring tooLong = L"C:\\" + std::wstring(40000, L'a');  // beyond what Windows can resolve
    EXPECT_EQ(NormalizeFolder(tooLong), tooLong);
}

TEST(Paths, WslExeDefaultsToTheSystemCopy) {
    Environment saved = CurrentEnvironment();
    CurrentEnvironment().wslExe.clear();
    std::wstring path = WslExePath();
    CurrentEnvironment() = saved;

    wchar_t system[MAX_PATH];
    UINT len = GetSystemDirectoryW(system, MAX_PATH);
    EXPECT_EQ(path, std::wstring(system, len) + L"\\wsl.exe");
    EXPECT_EQ(WslExePath(), test::FakeWslPath().wstring());  // what every test uses
}

// ---------------------------------------------------------------------------
// Settings

class SettingsTest : public test::WslTest {};

TEST_F(SettingsTest, DefaultRootFallsBackToTheUserProfile) {
    wchar_t profile[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    EXPECT_EQ(GetDefaultRoot(), std::wstring(profile, len) + L"\\WSL");
}

TEST_F(SettingsTest, DefaultRootWithoutAUsableProfileUsesDriveC) {
    wchar_t profile[MAX_PATH];
    GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    SetEnvironmentVariableW(L"USERPROFILE", nullptr);
    std::wstring missing = GetDefaultRoot();
    SetEnvironmentVariableW(L"USERPROFILE", (L"C:\\" + std::wstring(300, L'p')).c_str());
    std::wstring tooLong = GetDefaultRoot();
    SetEnvironmentVariableW(L"USERPROFILE", profile);
    EXPECT_EQ(missing, L"C:\\WSL");
    EXPECT_EQ(tooLong, L"C:\\WSL");
}

TEST_F(SettingsTest, AnEmptyStoredRootIsIgnored) {
    std::wstring key = CurrentEnvironment().settingsKey;
    RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), L"DefaultRoot", REG_SZ, L"", sizeof(wchar_t));
    EXPECT_TRUE(EndsWithIgnoreCase(GetDefaultRoot(), L"\\WSL"));
}

TEST_F(SettingsTest, SetDefaultRootIsRememberedAndUsedForDefaults) {
    SetDefaultRoot(Path(L"store\\").wstring());
    EXPECT_EQ(GetDefaultRoot(), Path(L"store").wstring());
    EXPECT_EQ(DefaultLocationFor(L"Dev"), Path(L"store\\Dev").wstring());

    std::wstring backup = DefaultBackupPath(L"Dev", BackupFormat::TarGz);
    EXPECT_EQ(fs::path(backup).parent_path(), Path(L"store\\Backups"));
    std::wstring file = fs::path(backup).filename().wstring();
    EXPECT_EQ(file.rfind(L"Dev-", 0), 0u);
    EXPECT_TRUE(EndsWithIgnoreCase(file, L".tar.gz"));
    EXPECT_EQ(file.size(), std::wstring(L"Dev-YYYYMMDD-HHMMSS.tar.gz").size());
}

TEST_F(SettingsTest, RegistryWriteFailuresAreReported) {
    CurrentEnvironment().settingsKey += L"\\" + std::wstring(300, L'x');  // longer than a key name may be
    ExpectError([&] { SetDefaultRoot(Path(L"x").wstring()); }, L"Could not write to the registry");
}

// ---------------------------------------------------------------------------
// Listing

class ListTest : public test::WslTest {};

TEST_F(ListTest, NoLxssKeyMeansNoInstances) {
    CurrentEnvironment().lxssKey += L"\\missing";
    EXPECT_TRUE(ListDistros().empty());
}

TEST_F(ListTest, ReadsInstancesFromTheRegistry) {
    std::wstring ubuntu = AddDistro({.name = L"ubuntu", .diskContent = "12345", .uid = 1000});
    AddDistro({.name = L"Alpine", .version = 1, .flavor = L"", .osVersion = L"", .longPathPrefix = false});
    AddDistro({.name = L"Middle", .state = 3});
    MakeDefault(ubuntu);

    auto distros = ListDistros();
    ASSERT_EQ(distros.size(), 3u);
    EXPECT_EQ(distros[0].name, L"Alpine");  // sorted case-insensitively
    EXPECT_EQ(distros[1].name, L"Middle");
    EXPECT_EQ(distros[2].name, L"ubuntu");

    const Distro& u = distros[2];
    EXPECT_EQ(u.id, ubuntu);
    EXPECT_EQ(u.version, 2);
    EXPECT_TRUE(u.isDefault);
    EXPECT_EQ(u.defaultUid, 1000u);
    EXPECT_EQ(u.basePath, Path(L"instances\\ubuntu").wstring());  // \\?\ prefix removed
    EXPECT_EQ(u.vhdPath, Path(L"instances\\ubuntu\\ext4.vhdx").wstring());
    EXPECT_EQ(u.diskBytes, 5u);
    EXPECT_EQ(u.flavor, L"ubuntu");
    EXPECT_EQ(u.osVersion, L"24.04");

    const Distro& a = distros[0];
    EXPECT_EQ(a.version, 1);
    EXPECT_FALSE(a.isDefault);
    EXPECT_TRUE(a.vhdPath.empty());
    EXPECT_EQ(a.diskBytes, 0u);
    EXPECT_EQ(a.basePath, Path(L"instances\\Alpine").wstring());
    EXPECT_EQ(distros[1].registryState, 3u);
}

TEST_F(ListTest, ToleratesIncompleteEntries) {
    AddDistro({.name = L"NoFlags", .createDisk = false, .includeFlags = false});
    AddDistro({.name = L"NoBase", .basePath = L"-"});
    RegSetKeyValueW(HKEY_CURRENT_USER, DistroKey(L"{nameless}").c_str(), L"State", REG_DWORD,
                    std::array<BYTE, 4>{1, 0, 0, 0}.data(), 4);
    RegSetKeyValueW(HKEY_CURRENT_USER, DistroKey(L"{blank-name}").c_str(), L"DistributionName", REG_SZ, L"",
                    sizeof(wchar_t));

    auto distros = ListDistros();
    ASSERT_EQ(distros.size(), 2u);
    EXPECT_EQ(distros[0].name, L"NoBase");
    EXPECT_TRUE(distros[0].basePath.empty());
    EXPECT_TRUE(distros[0].vhdPath.empty());
    EXPECT_EQ(distros[1].name, L"NoFlags");
    EXPECT_EQ(distros[1].version, 2);  // WSL 2 assumed when Flags is missing
    EXPECT_EQ(distros[1].diskBytes, 0u);  // disk file missing
}

TEST_F(ListTest, ReadsValuesLongerThanTheInitialBuffer) {
    std::wstring longFlavor(400, L'f');
    AddDistro({.name = L"Long", .flavor = longFlavor});
    auto distro = FindDistro(L"Long");
    ASSERT_TRUE(distro);
    EXPECT_EQ(distro->flavor, longFlavor);
}

TEST_F(ListTest, ReportsRunningInstances) {
    AddDistro({.name = L"Busy", .running = true});
    AddDistro({.name = L"Idle"});
    auto distros = ListDistros(true);
    EXPECT_TRUE(distros[0].running);
    EXPECT_FALSE(distros[1].running);
    EXPECT_FALSE(ListDistros(false)[0].running);  // not queried
}

TEST_F(ListTest, NothingRunning) {
    AddDistro({.name = L"Idle"});
    EXPECT_FALSE(ListDistros(true)[0].running);
    EXPECT_TRUE(Called(L"--list\t--running\t--quiet"));
}

TEST_F(ListTest, FindDistroIgnoresCase) {
    AddDistro({.name = L"Ubuntu", .running = true});
    ASSERT_TRUE(FindDistro(L"UBUNTU"));
    EXPECT_FALSE(FindDistro(L"ubuntu")->running);
    EXPECT_TRUE(FindDistro(L"ubuntu", true)->running);
    EXPECT_FALSE(FindDistro(L"Debian"));
}

TEST_F(ListTest, OnlineCatalog) {
    auto online = ListOnlineDistros();
    ASSERT_EQ(online.size(), 2u);
    EXPECT_EQ(online[0].name, L"Ubuntu");
    EXPECT_EQ(online[0].friendlyName, L"Ubuntu");
    EXPECT_EQ(online[1].name, L"Debian");
    EXPECT_EQ(online[1].friendlyName, L"Debian GNU/Linux");
}

TEST_F(ListTest, OnlineCatalogEntryWithoutDescription) {
    SetFake(L"FAKE_WSL_ONLINE", L"bare");
    auto online = ListOnlineDistros();
    ASSERT_EQ(online.size(), 3u);
    EXPECT_EQ(online[2].name, L"Bare-Distro");
    EXPECT_EQ(online[2].friendlyName, L"Bare-Distro");
}

TEST_F(ListTest, OnlineCatalogErrors) {
    SetFake(L"FAKE_WSL_ONLINE", L"empty");
    ExpectError([] { ListOnlineDistros(); }, L"Could not read the list of available distributions");
    SetFake(L"FAKE_WSL_FAIL", L"--online");
    ExpectError([] { ListOnlineDistros(); }, L"wsl.exe --list failed:\nSimulated failure.");
}

TEST_F(ListTest, OnlineCatalogInUtf16) {
    SetFake(L"FAKE_WSL_UTF16", L"1");
    EXPECT_EQ(ListOnlineDistros().size(), 2u);
}

// ---------------------------------------------------------------------------
// Names

class NameTest : public test::WslTest {};

TEST_F(NameTest, Validation) {
    AddDistro({.name = L"Taken"});
    EXPECT_NO_THROW(ValidateNewName(L"Good.Name_1-x"));
    ExpectError([] { ValidateNewName(L""); }, L"cannot be empty");
    ExpectError([] { ValidateNewName(std::wstring(65, L'a')); }, L"64 characters or fewer");
    ExpectError([] { ValidateNewName(L"bad/name"); }, L"may only contain");
    ExpectError([] { ValidateNewName(L"sp ace"); }, L"may only contain");
    ExpectError([] { ValidateNewName(L"tilde~"); }, L"may only contain");       // above 'z'
    ExpectError([] { ValidateNewName(L"café"); }, L"may only contain");  // non-ASCII
    ExpectError([] { ValidateNewName(L"at@"); }, L"may only contain");         // between digits and letters
    ExpectError([] { ValidateNewName(L"TAKEN"); }, L"already exists");
}

// ---------------------------------------------------------------------------
// Creating instances

class CreateTest : public test::WslTest {};

TEST_F(CreateTest, CleanInstall) {
    test::Captured out;
    SetFake(L"FAKE_WSL_PROGRESS", L"1");
    InstallClean({L"Ubuntu", L"Fresh", Path(L"new\\Fresh").wstring(), 1, true}, out.Fn());

    EXPECT_TRUE(Called(L"--install\tUbuntu\t--name\tFresh\t--location\t" + Path(L"new\\Fresh").wstring() +
                       L"\t--no-launch\t--version\t1\t--web-download"));
    auto distro = FindDistro(L"Fresh");
    ASSERT_TRUE(distro);
    EXPECT_EQ(distro->version, 1);
    EXPECT_TRUE(out.Has(L"Installed 'Fresh'"));
    EXPECT_FALSE(out.progress.empty());
}

TEST_F(CreateTest, CleanInstallWithoutWebDownload) {
    InstallClean({L"Debian", L"Deb", Path(L"Deb").wstring()}, nullptr);
    EXPECT_FALSE(Called(L"--web-download"));
    EXPECT_TRUE(fs::exists(Path(L"Deb\\ext4.vhdx")));
}

TEST_F(CreateTest, CleanInstallNotYetRegistered) {
    test::Captured out;
    SetFake(L"FAKE_WSL_NO_REGISTER", L"1");
    InstallClean({L"Ubuntu", L"Later", Path(L"Later").wstring()}, out.Fn());
    EXPECT_TRUE(out.Has(L"not registered yet"));
}

TEST_F(CreateTest, CleanInstallErrors) {
    ExpectError([&] { InstallClean({L"", L"X", Path(L"X").wstring()}, nullptr); }, L"No distribution was selected");

    SetFake(L"FAKE_WSL_FAIL", L"--install");
    ExpectError([&] { InstallClean({L"Ubuntu", L"X", Path(L"X").wstring()}, nullptr); }, L"wsl.exe --install failed");
    EXPECT_FALSE(fs::exists(Path(L"X")));  // the empty folder it created is cleaned up
}

TEST_F(CreateTest, TargetFolderChecks) {
    AddDistro({.name = L"Existing"});
    WriteFile(Path(L"afile"), "x");
    WriteFile(Path(L"hasdisk\\ext4.vhdx"), "x");
    auto install = [&](const fs::path& where) { InstallClean({L"Ubuntu", L"New", where.wstring()}, nullptr); };

    ExpectError([&] { install(Path(L"afile")); }, L"is a file, not a folder");
    ExpectError([&] { install(Path(L"instances\\Existing\\")); }, L"already holds the instance 'Existing'");
    ExpectError([&] { install(Path(L"hasdisk")); }, L"already contains a virtual disk");
    ExpectError([&] { install(Path(L"afile\\sub")); }, L"Could not create the folder");
}

TEST_F(CreateTest, RestoreTarBackupWithMetadata) {
    WriteFile(Path(L"b\\dev.tar"), "tar");
    WriteFile(Path(L"b\\dev.tar.wslmeta"),
              "# comment\n\nnot a setting\ndefault_uid=1000\nflavor=debian\nos_version=12\n");
    test::Captured out;
    RestoreBackup(L"Dev", Path(L"Dev").wstring(), Path(L"b\\dev.tar").wstring(), 1, out.Fn());

    EXPECT_TRUE(Called(L"--import\tDev\t" + Path(L"Dev").wstring() + L"\t" + Path(L"b\\dev.tar").wstring() +
                       L"\t--version\t1"));
    auto distro = FindDistro(L"Dev");
    ASSERT_TRUE(distro);
    EXPECT_EQ(distro->defaultUid, 1000u);
    EXPECT_EQ(distro->flavor, L"debian");
    EXPECT_EQ(distro->osVersion, L"12");
    EXPECT_TRUE(out.Has(L"Default user restored (uid 1000)"));
    EXPECT_TRUE(out.Has(L"Restored 'Dev'"));
}

TEST_F(CreateTest, RestoreVhdBackupWithoutMetadata) {
    WriteFile(Path(L"dev.vhdx"), "disk");
    test::Captured out;
    RestoreBackup(L"Dev", Path(L"Dev").wstring(), Path(L"dev.vhdx").wstring(), 2, out.Fn());
    EXPECT_TRUE(Called(L"dev.vhdx\t--vhd"));
    EXPECT_EQ(FindDistro(L"Dev")->defaultUid, 0u);
    EXPECT_FALSE(out.Has(L"Default user restored"));
}

TEST_F(CreateTest, RestoreErrors) {
    ExpectError([&] { RestoreBackup(L"Dev", Path(L"Dev").wstring(), Path(L"missing.tar").wstring(), 2, nullptr); },
                L"The backup file was not found");

    WriteFile(Path(L"dev.tar"), "tar");
    SetFake(L"FAKE_WSL_FAIL", L"--import");
    ExpectError([&] { RestoreBackup(L"Dev", Path(L"Dev").wstring(), Path(L"dev.tar").wstring(), 2, nullptr); },
                L"wsl.exe --import failed");
    EXPECT_FALSE(fs::exists(Path(L"Dev")));
}

TEST_F(CreateTest, RestoreThatDoesNotRegisterSkipsDetails) {
    WriteFile(Path(L"dev.tar"), "tar");
    WriteFile(Path(L"dev.tar.wslmeta"), "default_uid=1000\n");
    SetFake(L"FAKE_WSL_NO_REGISTER", L"1");
    test::Captured out;
    RestoreBackup(L"Dev", Path(L"Dev").wstring(), Path(L"dev.tar").wstring(), 2, out.Fn());
    EXPECT_FALSE(out.Has(L"Default user restored"));
}

TEST_F(CreateTest, CloneCopiesTheDiskDirectly) {
    AddDistro({.name = L"Src", .diskContent = "source disk", .running = true, .uid = 1000});
    test::Captured out;
    Clone(L"Src", L"Copy", Path(L"Copy").wstring(), out.Fn());

    EXPECT_TRUE(Called(L"--terminate\tSrc"));
    EXPECT_TRUE(Called(L"--import\tCopy\t" + Path(L"Copy").wstring() + L"\t" +
                       Path(L"instances\\Src\\ext4.vhdx").wstring() + L"\t--vhd"));
    EXPECT_FALSE(Called(L"--export"));
    EXPECT_EQ(ReadFile(Path(L"Copy\\ext4.vhdx")), "source disk");
    auto copy = FindDistro(L"Copy");
    ASSERT_TRUE(copy);
    EXPECT_EQ(copy->defaultUid, 1000u);
    EXPECT_EQ(copy->flavor, L"ubuntu");
    EXPECT_EQ(copy->osVersion, L"24.04");
    EXPECT_TRUE(out.Has(L"Stopping Src"));
    EXPECT_TRUE(out.Has(L"Created 'Copy' as a copy of 'Src'"));
}

TEST_F(CreateTest, CloneFallsBackToExportWhenTheDirectCopyFails) {
    AddDistro({.name = L"Src", .diskContent = "source disk"});
    SetFake(L"FAKE_WSL_FAIL", L"ext4.vhdx\t--vhd");
    test::Captured out;
    Clone(L"Src", L"Copy", Path(L"Copy").wstring(), out.Fn());

    EXPECT_TRUE(out.Has(L"Direct disk copy failed"));
    EXPECT_TRUE(Called(L"--export\tSrc\t" + Path(L"Copy\\wslmgr-clone-temp.vhdx").wstring() + L"\t--format\tvhd"));
    EXPECT_TRUE(Called(L"wslmgr-clone-temp.vhdx\t--vhd"));
    EXPECT_FALSE(fs::exists(Path(L"Copy\\wslmgr-clone-temp.vhdx")));  // temp removed
    EXPECT_EQ(ReadFile(Path(L"Copy\\ext4.vhdx")), "source disk");
}

TEST_F(CreateTest, CloneOfWsl1UsesATarExport) {
    AddDistro({.name = L"Old", .version = 1});
    Clone(L"Old", L"Copy", Path(L"Copy").wstring(), nullptr);
    EXPECT_TRUE(Called(L"--export\tOld\t" + Path(L"Copy\\wslmgr-clone-temp.tar").wstring() + L"\t--format\ttar"));
    EXPECT_TRUE(Called(L"wslmgr-clone-temp.tar\t--version\t1"));
    EXPECT_EQ(FindDistro(L"Copy")->version, 1);
}

TEST_F(CreateTest, CloneWhenTheSourceDiskIsMissing) {
    AddDistro({.name = L"Src", .createDisk = false});
    SetFake(L"FAKE_WSL_FAIL", L"--export");  // the fallback is the only path left
    ExpectError([&] { Clone(L"Src", L"Copy", Path(L"Copy").wstring(), nullptr); }, L"wsl.exe --export failed");
    EXPECT_FALSE(Called(L"--vhd"));
    EXPECT_FALSE(fs::exists(Path(L"Copy")));
}

TEST_F(CreateTest, CloneOfAnInstanceWithoutAKnownLocation) {
    AddDistro({.name = L"Lost", .basePath = L"-"});  // WSL 2, but no BasePath, so no disk path
    ExpectError([&] { Clone(L"Lost", L"Copy", Path(L"Copy").wstring(), nullptr); }, L"wsl.exe --export failed");
    EXPECT_FALSE(Called(L"\t--vhd"));
}

TEST_F(CreateTest, CloneErrors) {
    ExpectError([&] { Clone(L"Nope", L"Copy", Path(L"Copy").wstring(), nullptr); },
                L"There is no WSL instance named 'Nope'");
    AddDistro({.name = L"Src"});
    ExpectError([&] { Clone(L"Src", L"src", Path(L"Copy").wstring(), nullptr); }, L"already exists");
}

TEST_F(CreateTest, CloneWaitsForTheDiskToBeReleased) {
    AddDistro({.name = L"Src"});
    auto lock = std::make_unique<DiskLock>(Path(L"instances\\Src\\ext4.vhdx"));
    ASSERT_TRUE(lock->Held());
    std::thread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        lock->Release();
    });
    test::Captured out;
    Clone(L"Src", L"Copy", Path(L"Copy").wstring(), out.Fn());
    release.join();

    EXPECT_TRUE(out.Has(L"Waiting for WSL to release the virtual disk"));
    EXPECT_FALSE(out.progress.empty());
    EXPECT_TRUE(FindDistro(L"Copy"));
}

TEST_F(CreateTest, CloneGivesUpWhenTheDiskStaysInUse) {
    AddDistro({.name = L"Src"});
    DiskLock lock(Path(L"instances\\Src\\ext4.vhdx"));
    CurrentEnvironment().diskReleaseTimeoutMs = 100;
    ExpectError([&] { Clone(L"Src", L"Copy", Path(L"Copy").wstring(), nullptr); }, L"is still in use");
    EXPECT_FALSE(fs::exists(Path(L"Copy")));
}

// ---------------------------------------------------------------------------
// Backups

class BackupTest : public test::WslTest {};

TEST_F(BackupTest, VhdBackupWithMetadata) {
    AddDistro({.name = L"Dev", .diskContent = "0123456789", .running = true, .uid = 1000});
    test::Captured out;
    Backup(L"Dev", Path(L"backups\\dev.vhdx").wstring(), BackupFormat::Vhd, out.Fn());

    EXPECT_TRUE(Called(L"--export\tDev\t" + Path(L"backups\\dev.vhdx").wstring() + L"\t--format\tvhd"));
    EXPECT_EQ(ReadFile(Path(L"backups\\dev.vhdx")), "0123456789");
    std::string meta = ReadFile(Path(L"backups\\dev.vhdx.wslmeta"));
    EXPECT_NE(meta.find("name=Dev\n"), std::string::npos);
    EXPECT_NE(meta.find("wsl_version=2\n"), std::string::npos);
    EXPECT_NE(meta.find("default_uid=1000\n"), std::string::npos);
    EXPECT_NE(meta.find("flavor=ubuntu\n"), std::string::npos);
    EXPECT_NE(meta.find("os_version=24.04\n"), std::string::npos);
    EXPECT_NE(meta.find("format=vhd\n"), std::string::npos);
    EXPECT_NE(meta.find("created="), std::string::npos);
    EXPECT_TRUE(out.Has(L"Stopping Dev"));
    EXPECT_TRUE(out.Has(L"(10 B)"));
}

TEST_F(BackupTest, TarBackupOfWsl1) {
    AddDistro({.name = L"Old", .version = 1});
    Backup(L"Old", Path(L"old.tar.xz").wstring(), BackupFormat::TarXz, nullptr);
    EXPECT_TRUE(Called(L"--format\ttar.xz"));
    EXPECT_EQ(ReadFile(Path(L"old.tar.xz")), "tar:Old");
}

TEST_F(BackupTest, MissingOutputIsReportedAsZeroBytes) {
    AddDistro({.name = L"Dev"});
    SetFake(L"FAKE_WSL_NO_FILES", L"1");
    test::Captured out;
    Backup(L"Dev", Path(L"dev.tar").wstring(), BackupFormat::Tar, out.Fn());
    EXPECT_TRUE(out.Has(L"(0 B)"));
}

TEST_F(BackupTest, Errors) {
    AddDistro({.name = L"Old", .version = 1});
    AddDistro({.name = L"Dev"});
    WriteFile(Path(L"afile"), "x");

    ExpectError([&] { Backup(L"Nope", Path(L"x.tar").wstring(), BackupFormat::Tar, nullptr); }, L"no WSL instance");
    ExpectError([&] { Backup(L"Old", Path(L"x.vhdx").wstring(), BackupFormat::Vhd, nullptr); },
                L"only available for WSL 2");
    ExpectError([&] { Backup(L"Dev", L"  ", BackupFormat::Tar, nullptr); }, L"No backup file was given");
    ExpectError([&] { Backup(L"Dev", Path(L"afile\\sub\\x.tar").wstring(), BackupFormat::Tar, nullptr); },
                L"Could not create the folder");

    SetFake(L"FAKE_WSL_FAIL", L"--export");
    SetFake(L"FAKE_WSL_FAIL_MSG", L"-");  // fail without printing anything
    ExpectError([&] { Backup(L"Dev", Path(L"x.tar").wstring(), BackupFormat::Tar, nullptr); },
                L"wsl.exe --export failed:\nwsl.exe exited with code 0xFFFFFFFF.");
}

// ---------------------------------------------------------------------------
// Removing, moving, renaming

class ManageTest : public test::WslTest {};

TEST_F(ManageTest, RemoveDeletesTheEmptyFolder) {
    AddDistro({.name = L"Gone"});
    test::Captured out;
    Remove(L"Gone", true, out.Fn());
    EXPECT_TRUE(Called(L"--unregister\tGone"));
    EXPECT_FALSE(FindDistro(L"Gone"));
    EXPECT_FALSE(fs::exists(Path(L"instances\\Gone")));
    EXPECT_TRUE(out.Has(L"Removed the empty folder"));
}

TEST_F(ManageTest, RemoveKeepsTheFolderWhenAskedOrNotEmpty) {
    AddDistro({.name = L"Keep"});
    AddDistro({.name = L"Shared"});
    WriteFile(Path(L"instances\\Shared\\notes.txt"), "mine");
    Remove(L"Keep", false, nullptr);
    test::Captured out;
    Remove(L"Shared", true, out.Fn());
    EXPECT_TRUE(fs::exists(Path(L"instances\\Keep")));
    EXPECT_TRUE(fs::exists(Path(L"instances\\Shared\\notes.txt")));
    EXPECT_FALSE(out.Has(L"Removed the empty folder"));
    ExpectError([] { Remove(L"Keep", true, nullptr); }, L"no WSL instance");
}

TEST_F(ManageTest, RemoveWhenTheFolderIsAlreadyGoneOrUnknown) {
    AddDistro({.name = L"Vanished"});
    fs::remove_all(Path(L"instances\\Vanished"));
    AddDistro({.name = L"Lost", .basePath = L"-"});
    test::Captured out;
    Remove(L"Vanished", true, out.Fn());
    Remove(L"Lost", true, out.Fn());
    EXPECT_FALSE(out.Has(L"Removed the empty folder"));
    EXPECT_FALSE(FindDistro(L"Vanished"));
    EXPECT_FALSE(FindDistro(L"Lost"));
}

TEST_F(ManageTest, MoveRelocatesTheDisk) {
    AddDistro({.name = L"Dev", .diskContent = "disk"});
    test::Captured out;
    Move(L"Dev", Path(L"elsewhere\\Dev").wstring(), out.Fn());

    EXPECT_TRUE(Called(L"--manage\tDev\t--move\t" + Path(L"elsewhere\\Dev").wstring()));
    EXPECT_EQ(FindDistro(L"Dev")->basePath, Path(L"elsewhere\\Dev").wstring());
    EXPECT_EQ(ReadFile(Path(L"elsewhere\\Dev\\ext4.vhdx")), "disk");
    EXPECT_FALSE(fs::exists(Path(L"instances\\Dev")));
    EXPECT_TRUE(out.Has(L"Removed the empty folder"));
    EXPECT_TRUE(out.Has(L"Moved 'Dev'"));
}

TEST_F(ManageTest, MoveLeavesANonEmptyOldFolder) {
    AddDistro({.name = L"Dev"});
    WriteFile(Path(L"instances\\Dev\\other.txt"), "x");
    test::Captured out;
    Move(L"Dev", Path(L"new").wstring(), out.Fn());
    EXPECT_TRUE(fs::exists(Path(L"instances\\Dev\\other.txt")));
    EXPECT_FALSE(out.Has(L"Removed the empty folder"));
}

TEST_F(ManageTest, MoveErrors) {
    AddDistro({.name = L"Dev"});
    ExpectError([&] { Move(L"Dev", Path(L"instances\\Dev\\").wstring(), nullptr); }, L"is already stored in");

    SetFake(L"FAKE_WSL_FAIL", L"--move");
    ExpectError([&] { Move(L"Dev", Path(L"target").wstring(), nullptr); }, L"wsl.exe --manage failed");
    EXPECT_FALSE(fs::exists(Path(L"target")));
}

TEST_F(ManageTest, MoveOfAnInstanceWithoutAKnownLocation) {
    AddDistro({.name = L"Lost", .basePath = L"-"});
    ExpectError([&] { Move(L"Lost", Path(L"target").wstring(), nullptr); }, L"wsl.exe --manage failed");
}

TEST_F(ManageTest, MoveDoesNotWaitForAMissingDisk) {
    AddDistro({.name = L"NoDisk", .createDisk = false});
    ExpectError([&] { Move(L"NoDisk", Path(L"target").wstring(), nullptr); }, L"wsl.exe --manage failed");
}

TEST_F(ManageTest, Rename) {
    std::wstring id = AddDistro({.name = L"Old", .running = true});
    AddDistro({.name = L"Other"});
    test::Captured out;
    Rename(L"Old", L"New", out.Fn());
    EXPECT_EQ(RegString(DistroKey(id), L"DistributionName"), L"New");
    EXPECT_TRUE(out.Has(L"Stopping Old"));
    EXPECT_TRUE(out.Has(L"keep the old name"));

    Rename(L"New", L"NEW", nullptr);  // changing only the case is allowed
    EXPECT_EQ(RegString(DistroKey(id), L"DistributionName"), L"NEW");

    ExpectError([] { Rename(L"NEW", L"other", nullptr); }, L"already exists");
    ExpectError([] { Rename(L"NEW", L"bad name", nullptr); }, L"may only contain");
    ExpectError([] { Rename(L"Missing", L"x", nullptr); }, L"no WSL instance");
}

TEST_F(ManageTest, SetDefaultAndDefaultUser) {
    AddDistro({.name = L"Dev"});
    test::Captured out;
    SetDefault(L"Dev", out.Fn());
    EXPECT_TRUE(FindDistro(L"Dev")->isDefault);
    EXPECT_TRUE(out.Has(L"'Dev' is now the default instance"));

    SetDefaultUser(L"Dev", L" alice ", out.Fn());
    EXPECT_TRUE(Called(L"--manage\tDev\t--set-default-user\talice"));
    EXPECT_EQ(FindDistro(L"Dev")->defaultUid, 1000u);

    ExpectError([] { SetDefaultUser(L"Dev", L"  ", nullptr); }, L"No user name was given");
    ExpectError([] { SetDefaultUser(L"Dev", L"nobody", nullptr); }, L"The user was not found");
}

TEST_F(ManageTest, StopAndShutdown) {
    AddDistro({.name = L"A", .running = true});
    AddDistro({.name = L"B", .running = true});
    test::Captured out;
    Terminate(L"A", out.Fn());
    EXPECT_FALSE(FindDistro(L"A", true)->running);
    EXPECT_TRUE(FindDistro(L"B", true)->running);
    EXPECT_TRUE(out.Has(L"Stopped 'A'"));

    Shutdown(out.Fn());
    EXPECT_FALSE(FindDistro(L"B", true)->running);
    EXPECT_TRUE(out.Has(L"WSL has been shut down"));
}

TEST_F(ManageTest, OpenShellLaunchesWsl) {
    AddDistro({.name = L"Dev"});
    OpenShell(L"Dev");
    for (int i = 0; i < 100 && !FindDistro(L"Dev", true)->running; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(Called(L"--distribution\tDev\t--cd\t~"));
    ExpectError([] { OpenShell(L"Missing"); }, L"no WSL instance");
}
