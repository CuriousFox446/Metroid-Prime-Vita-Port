#pragma once

// The PS Vita's newlib is built without C99 printf formats (_WANT_IO_C99_FORMATS):
// it does not know the length modifiers z, t, j or hh. A "%zu" prints "zu" and
// consumes no argument, so every argument after it is read one slot off, and a
// later %s takes a number for a string (a crash on hardware). vita_printf.cpp
// passes every format through RewriteC99Format first.
//
// On the Vita size_t and ptrdiff_t are as wide as int, so z and t are dropped;
// intmax_t is long long, so j becomes ll (which newlib supports); hh becomes h.
// Header-only so the host test can check it.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace VitaPrintf {

// Rewrites `format` into `out` and returns true when it uses one of the modifiers;
// returns false, leaving `out` alone, when it can be used as it is.
inline bool RewriteC99Format(const char* format, std::string& out) {
  bool needed = false;
  for (const char* p = format; *p != '\0'; ++p) {
    if (*p != '%') {
      continue;
    }
    ++p;
    if (*p == '%') {
      continue; // "%%"
    }
    while (*p != '\0' && std::string_view("-+ #0'123456789.*").find(*p) != std::string_view::npos) {
      ++p;
    }
    if (*p == 'z' || *p == 't' || *p == 'j' || (p[0] == 'h' && p[1] == 'h')) {
      needed = true;
      break;
    }
    if (*p == '\0') {
      break;
    }
  }
  if (!needed) {
    return false;
  }
  out.clear();
  out.reserve(std::char_traits<char>::length(format) + 8);
  for (const char* p = format; *p != '\0';) {
    if (*p != '%') {
      out += *p++;
      continue;
    }
    out += *p++; // '%'
    if (*p == '%') {
      out += *p++; // "%%": a literal percent sign, not a conversion
      continue;
    }
    while (*p != '\0' && std::string_view("-+ #0'123456789.*").find(*p) != std::string_view::npos) {
      out += *p++;
    }
    if (*p == 'z' || *p == 't') {
      ++p;
    } else if (*p == 'j') {
      out += "ll";
      ++p;
    } else if (p[0] == 'h' && p[1] == 'h') {
      out += 'h';
      p += 2;
    }
    // The length modifiers newlib knows (h, l, ll, L) and the conversion, or "%%",
    // are copied by the plain path on the next characters.
  }
  return true;
}

static_assert(sizeof(std::size_t) == sizeof(int) || UINTPTR_MAX > UINT32_MAX,
              "dropping z assumes size_t is as wide as int (true on the Vita)");

} // namespace VitaPrintf
