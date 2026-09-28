// =============================================================================
// TestMain.cpp - the one line that turns the test suite into a program
// =============================================================================
//
// All the actual checks live in TestRunner.cpp as runTests(). This file exists
// only to give the linker a main() to start from, and to turn "some checks
// failed" into a non-zero exit code so `make test` or CI can react to it
// without parsing the output.
//
// Unix convention: exit 0 means success, anything else means failure. 1 for
// "tests failed" is the conventional value, and it is what every CI system
// already understands.
// =============================================================================

#include "tests/TestRunner.hpp"

#include <iostream>

int main() {
    int result = runTests();
    if (result != 0) {
        std::cout << "\n*** TEST SUITE FAILED ***\n";
    }
    return result;
}
