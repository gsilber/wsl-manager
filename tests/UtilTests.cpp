#include "TestSupport.h"

#include "core/Util.h"

#include <cwctype>

using namespace wslmgr;

TEST(Util, Utf8RoundTrip) {
    EXPECT_EQ(Utf8ToWide(""), L"");
    EXPECT_EQ(WideToUtf8(L""), "");
    std::wstring text = L"h\u00e9llo \u2713 \U0001F600";
    EXPECT_EQ(Utf8ToWide(WideToUtf8(text)), text);
    EXPECT_EQ(WideToUtf8(L"\u00e9"), "\xC3\xA9");
}

TEST(Util, Trim) {
    EXPECT_EQ(Trim(L"  a b \r\n"), L"a b");
    EXPECT_EQ(Trim(L"\tx"), L"x");
    EXPECT_EQ(Trim(L" \t\r\n "), L"");
    EXPECT_EQ(Trim(L""), L"");
}

TEST(Util, CaseInsensitiveComparisons) {
    EXPECT_TRUE(EqualsIgnoreCase(L"Ubuntu", L"UBUNTU"));
    EXPECT_FALSE(EqualsIgnoreCase(L"Ubuntu", L"Debian"));
    EXPECT_FALSE(EqualsIgnoreCase(L"Ubuntu", L"Ubuntu2"));
    EXPECT_TRUE(EndsWithIgnoreCase(L"backup.TAR.GZ", L".tar.gz"));
    EXPECT_FALSE(EndsWithIgnoreCase(L"backup.tar", L".tar.gz"));
    EXPECT_FALSE(EndsWithIgnoreCase(L"gz", L".tar.gz"));
}

TEST(Util, FormatBytes) {
    EXPECT_EQ(FormatBytes(0), L"0 B");
    EXPECT_EQ(FormatBytes(1023), L"1023 B");
    EXPECT_EQ(FormatBytes(1536), L"1.5 KB");
    EXPECT_EQ(FormatBytes(5ull << 20), L"5.0 MB");
    EXPECT_EQ(FormatBytes(3ull << 30), L"3.0 GB");
    EXPECT_EQ(FormatBytes(2ull << 40), L"2.0 TB");
    EXPECT_EQ(FormatBytes(5000ull << 40), L"5000.0 TB");  // TB is the largest unit
}

TEST(Util, TimestampFormat) {
    std::wstring stamp = Timestamp();
    ASSERT_EQ(stamp.size(), 15u);
    for (size_t i = 0; i < stamp.size(); ++i) {
        if (i == 8)
            EXPECT_EQ(stamp[i], L'-');
        else
            EXPECT_TRUE(iswdigit(stamp[i])) << "position " << i;
    }
}

TEST(Util, StripLongPathPrefix) {
    EXPECT_EQ(StripLongPathPrefix(L"\\\\?\\D:\\WSL\\Ubuntu"), L"D:\\WSL\\Ubuntu");
    EXPECT_EQ(StripLongPathPrefix(L"\\\\?\\UNC\\server\\share\\wsl"), L"\\\\server\\share\\wsl");
    EXPECT_EQ(StripLongPathPrefix(L"C:\\plain"), L"C:\\plain");
    EXPECT_EQ(StripLongPathPrefix(L""), L"");
}

TEST(Util, LastErrorMessage) {
    std::wstring known = LastErrorMessage(ERROR_FILE_NOT_FOUND);
    EXPECT_FALSE(known.empty());
    EXPECT_EQ(known.find(L"Error "), std::wstring::npos);  // a real system message, whatever the locale
    EXPECT_NE(known.back(), L'\n');                       // trimmed

    constexpr unsigned long kUnknown = 0xE0001234;
    EXPECT_EQ(LastErrorMessage(kUnknown), L"Error " + std::to_wstring(kUnknown));
}

TEST(Util, ThrowLastError) {
    SetLastError(ERROR_ACCESS_DENIED);
    try {
        ThrowLastError(L"Opening the thing");  // [[noreturn]]: always throws
    } catch (const WslError& e) {
        EXPECT_EQ(e.message(), L"Opening the thing: " + LastErrorMessage(ERROR_ACCESS_DENIED));
        EXPECT_STREQ(e.what(), "WslError");
    }
}
