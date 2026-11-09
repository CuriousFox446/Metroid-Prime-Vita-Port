// The entities' Think and PreThink by class: the profile report's [think] lines (see
// port_profile.h). The zone profiler times "SM: Think (all entities)" as one; this says which
// classes its 4 ms a tick go to in a heavy room (0.42.0 test 11).

#include "port_profile.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace PortThinkProfile {
struct Type {
  const std::type_info* info = nullptr;
  uint64_t ns[kPassCount] = {};
  uint32_t calls[kPassCount] = {};
  uint32_t sequence[kPassCount] = {}; // calls this report window, for sampling
};

namespace {
constexpr uint32_t kExactCalls = 32;
constexpr uint32_t kSampleEvery = 8;

// By the type_info's address, open addressing: the game has a few hundred entity classes. Game
// thread only, as Think is.
constexpr size_t kSlots = 1024;
Type sTypes[kSlots];

Type* find(const std::type_info* info) noexcept {
  size_t i = (reinterpret_cast<uintptr_t>(info) >> 3) & (kSlots - 1);
  for (size_t n = 0; n < kSlots; ++n, i = (i + 1) & (kSlots - 1)) {
    if (sTypes[i].info == info) {
      return &sTypes[i];
    }
    if (sTypes[i].info == nullptr) {
      sTypes[i].info = info;
      return &sTypes[i];
    }
  }
  return nullptr;
}

// An Itanium-mangled class name as written: "13CScriptEffect" is CScriptEffect; a nested one
// stays mangled.
const char* readable(const char* mangled) noexcept {
  const char* p = mangled;
  while (*p >= '0' && *p <= '9') {
    ++p;
  }
  return p != mangled && *p != '\0' ? p : mangled;
}
} // namespace

Scope::Scope(const std::type_info& type, Pass pass) noexcept {
  if (!vita_profile::Enabled()) {
    return;
  }
  Type* entry = find(&type);
  if (entry == nullptr) {
    return;
  }
  m_type = entry;
  m_pass = pass;
  ++entry->calls[pass];
  const uint32_t n = entry->sequence[pass]++;
  m_weight = n < kExactCalls ? 1 : (n % kSampleEvery == 0 ? kSampleEvery : 0);
  if (m_weight != 0) {
    m_start = vita_profile::NowNs();
  }
}

Scope::~Scope() {
  if (m_type != nullptr && m_weight != 0) {
    m_type->ns[m_pass] += (vita_profile::NowNs() - m_start) * m_weight;
  }
}

void Report(uint32_t frames, void (*write)(const char* line)) {
  if (frames == 0 || !vita_profile::Enabled()) {
    return;
  }
  struct Row {
    const Type* type;
    Pass pass;
  };
  std::vector<Row> rows;
  for (const Type& type : sTypes) {
    for (int pass = 0; type.info != nullptr && pass < kPassCount; ++pass) {
      if (type.calls[pass] != 0) {
        rows.push_back({&type, static_cast<Pass>(pass)});
      }
    }
  }
  std::sort(rows.begin(), rows.end(),
            [](const Row& a, const Row& b) { return a.type->ns[a.pass] > b.type->ns[b.pass]; });
  constexpr size_t kShown = 14;
  char line[192];
  for (size_t i = 0; i < std::min(rows.size(), kShown); ++i) {
    const Row& row = rows[i];
    std::snprintf(line, sizeof(line), "[think] %8.3f ms %7.2f calls  %s %s\n",
                  static_cast<double>(row.type->ns[row.pass]) / 1e6 / frames,
                  static_cast<double>(row.type->calls[row.pass]) / frames, row.pass == kThink ? "Think" : "PreThink",
                  readable(row.type->info->name()));
    write(line);
  }
  for (Type& type : sTypes) {
    for (int pass = 0; pass < kPassCount; ++pass) {
      type.ns[pass] = 0;
      type.calls[pass] = 0;
      type.sequence[pass] = 0;
    }
  }
}
} // namespace PortThinkProfile
