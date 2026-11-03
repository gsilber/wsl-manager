#include "TestSupport.h"

#include <objbase.h>

int wmain(int argc, wchar_t** argv) {
    // Before anything else, make sure nothing in this process can reach the real
    // wsl.exe or the real WSL / WslManager registry keys.
    test::InstallSafeEnvironment();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
