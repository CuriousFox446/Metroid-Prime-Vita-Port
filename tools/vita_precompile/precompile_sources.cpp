// PS Vita: the generated programs' sources for the material records a Vita session saved on its
// card (ux0:data/MetroidPrime/vita_programs/materials.bin), made on the PC with this tree's own
// generator (extern/aurora/lib/gx/vita_tev.cpp, tev_record_program), so the Vita's compiler can
// compile them here (precompile.py) rather than on the Vita. Built by build.sh.
//
//   precompile_sources <materials.bin> <out dir>
//   precompile_sources --self-test
//
// Writes <key>.vcg and <key>.fcg (the vertex and fragment programs; key as the card names a
// program's binary, 16 hex digits) for each program the records make, once each, and manifest.txt
// (one line a program: key, the two sources' lengths). Records of another build's layout, and
// materials no program draws, are counted and left out. --self-test makes a textured, vertex
// coloured, fogged material's record and prints the program it makes again.

#include "gx/vita_tev.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_set>
#include <vector>

// What the generator links against on the Vita, none of it used here.
namespace aurora {
AuroraConfig g_config{};
void log_internal(AuroraLogLevel, const char*, const char*, unsigned int) noexcept {}
namespace gx {
GXState g_gxState{};
}
namespace vita_gl {
std::shared_ptr<Program> make_program(std::string, std::string, uint64_t) noexcept { return nullptr; }
const std::vector<uint8_t>& saved_materials() noexcept {
  static const std::vector<uint8_t> none;
  return none;
}
void save_material(const void*, size_t) noexcept {}
bool program_known(uint64_t) noexcept { return true; }
bool warm_up_room() noexcept { return false; }
void warm_up(uint64_t, std::string, std::string) noexcept {}
} // namespace vita_gl
} // namespace aurora

namespace {
bool write_file(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out{path, std::ios::binary};
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  return static_cast<bool>(out);
}

int self_test() {
  using namespace aurora::gx;
  GXState& s = g_gxState;
  s.numTevStages = 1;
  TevStage& stage = s.tevStages[0];
  stage.colorPass = {GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO};
  stage.alphaPass = {GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO};
  stage.texCoordId = GX_TEXCOORD0;
  stage.texMapId = GX_TEXMAP0;
  stage.channelId = GX_COLOR0A0;
  s.tcgs[0].type = GX_TG_MTX2x4;
  s.tcgs[0].src = GX_TG_TEX0;
  s.vtxDesc[GX_VA_TEX0] = GX_DIRECT;
  s.colorChannelConfig[GX_COLOR0].matSrc = GX_SRC_VTX;
  s.colorChannelConfig[GX_ALPHA0].matSrc = GX_SRC_VTX;
  s.fog.type = GX_FOG_PERSP_LIN;
  const std::vector<uint8_t> record = vita::tev_capture_material();
  // What the record stands in for is put back after: clear it, so a match shows the record made it.
  s = GXState{};
  std::string vertexSource;
  std::string fragmentSource;
  uint64_t key = 0;
  if (!vita::tev_record_program(record.data(), record.size(), vertexSource, fragmentSource, key)) {
    std::fprintf(stderr, "the record made no program\n");
    return 1;
  }
  std::printf("record %zu bytes, key %016llx\n-- vertex --\n%s-- fragment --\n%s", record.size(),
              static_cast<unsigned long long>(key), vertexSource.c_str(), fragmentSource.c_str());
  return s.numTevStages == 0 && s.fog.type == GX_FOG_NONE ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) {
    return self_test();
  }
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <materials.bin> <out dir> | --self-test\n", argv[0]);
    return 2;
  }
  std::ifstream in{argv[1], std::ios::binary};
  if (!in) {
    std::fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  const std::filesystem::path outDir{argv[2]};
  std::filesystem::create_directories(outDir);
  std::ofstream manifest{outDir / "manifest.txt"};

  std::unordered_set<uint64_t> written;
  size_t records = 0;
  size_t skipped = 0;
  size_t at = 0;
  while (bytes.size() - at >= sizeof(uint32_t)) {
    uint32_t length = 0;
    std::memcpy(&length, bytes.data() + at, sizeof(length));
    at += sizeof(length);
    if (length > bytes.size() - at) {
      break; // cut short, as the Vita reads it (index_records)
    }
    ++records;
    std::string vertexSource;
    std::string fragmentSource;
    uint64_t key = 0;
    const bool made = aurora::gx::vita::tev_record_program(bytes.data() + at, length, vertexSource, fragmentSource, key);
    at += length;
    if (!made) {
      ++skipped;
      continue;
    }
    if (!written.insert(key).second) {
      continue;
    }
    char name[17];
    std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(key));
    if (!write_file(outDir / (std::string{name} + ".vcg"), vertexSource) ||
        !write_file(outDir / (std::string{name} + ".fcg"), fragmentSource)) {
      std::fprintf(stderr, "cannot write %s's sources\n", name);
      return 1;
    }
    manifest << name << ' ' << vertexSource.size() << ' ' << fragmentSource.size() << '\n';
  }
  std::printf("%zu records: %zu programs, %zu of another build or not drawable\n", records, written.size(),
              skipped);
  return 0;
}
