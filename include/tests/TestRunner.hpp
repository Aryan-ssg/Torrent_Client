#pragma once

// =============================================================================
// TestRunner.hpp - entry point for the automated test suite
// =============================================================================
//
// The suite lives in src/tests/TestRunner.cpp and is built as its own binary,
// "peerflow-tests". It used to be src/main.cpp back when the project was a pile
// of tests and nothing else; now that there is a real command-line interface
// (src/main.cpp), the two need separate entry points.
//
// runTests() prints a PASS/FAIL line per check and returns the process exit
// code: 0 if everything passed, 1 otherwise. That is all a test "main" needs
// to be, and keeping it this dumb means the suite can be driven by CI, a
// shell script, or the human at a terminal without any extra machinery.
//
// Java parallel: like a JUnit runner's "run all and report" method.
// =============================================================================

// Runs every check, printing results. Returns 0 on success, 1 if any failed.
int runTests();
