#pragma once

// Runs the wslmgr command line (argv[0] is the program name) and returns the
// process exit code. Output goes to the standard handles; errors are printed,
// never thrown.
int RunCli(int argc, wchar_t** argv);
