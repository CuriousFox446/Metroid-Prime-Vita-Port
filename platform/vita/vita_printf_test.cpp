// Host check of VitaPrintf::RewriteC99Format (vita_printf_format.h). From this directory:
//   g++ -std=c++20 -Wall -Wextra vita_printf_test.cpp -o vita_printf_test && ./vita_printf_test
// On a 64-bit host the rewritten formats are not printf-safe (size_t is 64-bit
// there); only the text is checked.

#include "vita_printf_format.h"

#include <cstdio>

static int g_failures = 0;

static void Expect(const char* in, const char* want) {
  std::string out = "untouched";
  const bool changed = VitaPrintf::RewriteC99Format(in, out);
  const std::string got = changed ? out : std::string(in);
  if (got != want) {
    std::printf("FAIL \"%s\" -> \"%s\", want \"%s\"\n", in, got.c_str(), want);
    ++g_failures;
  }
  if (!changed && out != "untouched") {
    std::printf("FAIL \"%s\": unchanged but wrote the output\n", in);
    ++g_failures;
  }
}

int main() {
  Expect("plain text", "plain text");
  Expect("%d %s %lld %hu %5.2f %%", "%d %s %lld %hu %5.2f %%");
  Expect("its last %zu files read in %lld ms", "its last %u files read in %lld ms");
  Expect("%s: %zu bytes in %.3f ms", "%s: %u bytes in %.3f ms");
  Expect("%02zx|%-8zd|%*zu", "%02x|%-8d|%*u");
  Expect("%td %jd %ju %hhu %hhx", "%d %lld %llu %hu %hx");
  Expect("100%% done, %zu left", "100%% done, %u left");
  Expect("%%zu is literal", "%%zu is literal");
  Expect("%%zu stays, %zu goes", "%%zu stays, %u goes");
  Expect("%%%zu", "%%%u");
  Expect("trailing %", "trailing %");
  Expect("%z", "%");
  std::printf(g_failures == 0 ? "vita_printf_test: all passed\n" : "vita_printf_test: %d failed\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
