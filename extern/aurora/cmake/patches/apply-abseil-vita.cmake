# Abseil on PS Vita (newlib, ELF without a dynamic loader). Run as a FetchContent
# PATCH_COMMAND with -DABSL_SOURCE_DIR=<dir>; applying it twice is harmless.
#  - cctz: newlib's struct tm has no tm_gmtoff/tm_zone. The Vita's clock has no
#    time zone, so the offset is 0 (UTC).
#  - elf_mem_image: needs <link.h> (the vDSO), which the Vita does not have.

function(_absl_vita_patch file from to marker)
  set(path "${ABSL_SOURCE_DIR}/${file}")
  file(READ "${path}" text)
  string(FIND "${text}" "${marker}" already)
  if (NOT already EQUAL -1)
    return()
  endif ()
  string(FIND "${text}" "${from}" at)
  if (at EQUAL -1)
    message(FATAL_ERROR "apply-abseil-vita: '${from}' not found in ${file}")
  endif ()
  string(REPLACE "${from}" "${to}" text "${text}")
  file(WRITE "${path}" "${text}")
endfunction()

_absl_vita_patch(absl/time/internal/cctz/src/time_zone_libc.cc
  "#elif defined(__sun) || defined(_AIX)"
  "#elif defined(__vita__)
// newlib: no tm_gmtoff or tm_zone, and the Vita's clock has no time zone.
auto tm_gmtoff(const std::tm&) -> long { return 0; }
auto tm_zone(const std::tm&) -> const char* { return \"UTC\"; }
#elif defined(__sun) || defined(_AIX)"
  "defined(__vita__)")

_absl_vita_patch(absl/debugging/internal/elf_mem_image.h
  "!defined(__VXWORKS__) && !defined(__hexagon__)"
  "!defined(__VXWORKS__) && !defined(__hexagon__) && !defined(__vita__)"
  "!defined(__vita__)")
