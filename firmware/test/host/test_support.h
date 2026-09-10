// Minimal assertion helpers for the host tests.
//
// Deliberately not a test framework: three pure C files with no dependencies
// are exactly the case where a framework costs more (a submodule, a build path,
// a version to keep current) than the twenty lines it replaces. If these tests
// ever need fixtures or parameterization, that is the moment to reconsider —
// not now.
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
static const char *g_current_test = "";

#define CHECK(cond)                                                  \
  do {                                                               \
    if (!(cond)) {                                                   \
      fprintf(stderr, "  FAIL %s:%d [%s]: %s\n", __FILE__, __LINE__, \
              g_current_test, #cond);                                \
      g_failures++;                                                  \
    }                                                                \
  } while (0)

#define CHECK_EQ_INT(actual, expected)                                \
  do {                                                                \
    const long a_ = (long)(actual);                                   \
    const long e_ = (long)(expected);                                 \
    if (a_ != e_) {                                                   \
      fprintf(stderr, "  FAIL %s:%d [%s]: %s == %ld, expected %ld\n", \
              __FILE__, __LINE__, g_current_test, #actual, a_, e_);   \
      g_failures++;                                                   \
    }                                                                 \
  } while (0)

#define CHECK_EQ_STR(actual, expected)                                      \
  do {                                                                      \
    if (strcmp((actual), (expected)) != 0) {                                \
      fprintf(stderr, "  FAIL %s:%d [%s]: %s == \"%s\", expected \"%s\"\n", \
              __FILE__, __LINE__, g_current_test, #actual, (actual),        \
              (expected));                                                  \
      g_failures++;                                                         \
    }                                                                       \
  } while (0)

#define RUN(test_fn)           \
  do {                         \
    g_current_test = #test_fn; \
    test_fn();                 \
  } while (0)

static inline int test_summary(const char *suite) {
  if (g_failures == 0) {
    printf("%s: ok\n", suite);
    return EXIT_SUCCESS;
  }
  fprintf(stderr, "%s: %d failure(s)\n", suite, g_failures);
  return EXIT_FAILURE;
}
