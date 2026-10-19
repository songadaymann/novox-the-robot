// Shared assert macros for the singing-v2 test files. Each test function takes
// the suite's counters as `int& checks, int& failures`.
#pragma once

#include <cmath>
#include <iostream>

#define V2_CHECK(cond)                                                                  \
  do {                                                                                  \
    ++checks;                                                                           \
    if (!(cond)) {                                                                      \
      ++failures;                                                                       \
      std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond << "\n";      \
    }                                                                                   \
  } while (0)

#define V2_CHECK_NEAR(a, b, eps) V2_CHECK(std::fabs((a) - (b)) <= (eps))

// Entry points, one per file (tests/singing_*_tests.cpp, tests/syllables_tests.cpp).
void testSingingPitchV2(int& checks, int& failures);
void testSingingSegmentV2(int& checks, int& failures);
void testSyllablesV2(int& checks, int& failures);
