// The printf family the port links against, with C99 length modifiers rewritten for
// the Vita's newlib (see vita_printf_format.h). Linked with --wrap for each function
// (CMakeLists.txt), so every caller in the program, the game's own reports included,
// comes through here; formats without those modifiers go to newlib unchanged.

#include "vita_printf_format.h"

#include <cstdarg>
#include <cstdio>
#include <string>

static_assert(sizeof(std::ptrdiff_t) == sizeof(int), "dropping t assumes ptrdiff_t is as wide as int");
static_assert(sizeof(std::intmax_t) == sizeof(long long), "j -> ll assumes intmax_t is long long");

namespace {
// The format to hand newlib: the original, or the rewritten copy in `storage`.
const char* Fixed(const char* format, std::string& storage) {
  return VitaPrintf::RewriteC99Format(format, storage) ? storage.c_str() : format;
}
} // namespace

extern "C" {
int __real_vfprintf(FILE* stream, const char* format, va_list args);
int __real_vsnprintf(char* buffer, size_t size, const char* format, va_list args);
int __real_vsprintf(char* buffer, const char* format, va_list args);

int __wrap_vfprintf(FILE* stream, const char* format, va_list args) {
  std::string storage;
  return __real_vfprintf(stream, Fixed(format, storage), args);
}

int __wrap_vprintf(const char* format, va_list args) { return __wrap_vfprintf(stdout, format, args); }

int __wrap_vsnprintf(char* buffer, size_t size, const char* format, va_list args) {
  std::string storage;
  return __real_vsnprintf(buffer, size, Fixed(format, storage), args);
}

int __wrap_vsprintf(char* buffer, const char* format, va_list args) {
  std::string storage;
  return __real_vsprintf(buffer, Fixed(format, storage), args);
}

int __wrap_fprintf(FILE* stream, const char* format, ...) {
  va_list args;
  va_start(args, format);
  const int result = __wrap_vfprintf(stream, format, args);
  va_end(args);
  return result;
}

int __wrap_printf(const char* format, ...) {
  va_list args;
  va_start(args, format);
  const int result = __wrap_vfprintf(stdout, format, args);
  va_end(args);
  return result;
}

int __wrap_snprintf(char* buffer, size_t size, const char* format, ...) {
  va_list args;
  va_start(args, format);
  const int result = __wrap_vsnprintf(buffer, size, format, args);
  va_end(args);
  return result;
}

int __wrap_sprintf(char* buffer, const char* format, ...) {
  va_list args;
  va_start(args, format);
  const int result = __wrap_vsprintf(buffer, format, args);
  va_end(args);
  return result;
}
}
