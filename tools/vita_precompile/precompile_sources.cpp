// PS Vita: the generated programs' sources for the material records a Vita session saved on its
// card (ux0:data/MetroidPrime/vita_programs/materials.bin), made on the PC with this tree's own
// generator (extern/aurora/lib/gx/vita_tev.cpp, tev_record_program), so the Vita's compiler can
// compile them here (precompile.py) rather than on the Vita. Built by build.sh.
//
//   precompile_sources <materials.bin> <out dir>
//   precompile_sources --self-test [lava|lean [out dir]]
//
// Writes <key>.vcg and <key>.fcg (the vertex and fragment programs; key as the card names a
// program's binary, 16 hex digits) for each program the records make, once each, and manifest.txt
// (one line a program: key, the two sources' lengths). Records of another build's layout, and
// materials no program draws, are counted and left out. --self-test makes a textured, vertex
// coloured, fogged material's record and prints the program it makes again; with `lava`, the
// bump-mapped lava's (CFluidPlaneCPU::RenderSetup, kFT_Lava: an emboss coordinate), with `lean` the
// first material's program made lean (a particle's, 0.46.0 test 46), and with an out dir its
// sources and manifest there too, for precompile.py to compile.

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
bool program_ready(const Program*) noexcept { return false; }
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

// The bump-mapped lava's state as CFluidPlaneCPU::RenderSetup leaves it (kFT_Lava with a bump map):
// three textures from the position, the bump map from the position and again through an emboss
// coordinate (GX_TG_BUMP3 from set 3), the emboss difference in register 0 scaling the colour.
void lava_state(aurora::gx::GXState& s) {
  using namespace aurora::gx;
  s.numTevStages = 6;
  s.numTexGens = 5;
  s.numChans = 2;
  const auto stage = [&s](int i, TevPass<GXTevColorArg, GX_CC_ZERO> color, GXTexCoordID coord, GXTexMapID map,
                          GXChannelID channel) -> TevStage& {
    TevStage& st = s.tevStages[i];
    st.colorPass = color;
    st.texCoordId = coord;
    st.texMapId = map;
    st.channelId = channel;
    return st;
  };
  stage(0, {GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_RASC}, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0).kcSel =
      GX_TEV_KCSEL_K0;
  stage(1, {GX_CC_ZERO, GX_CC_TEXC, GX_CC_CPREV, GX_CC_RASC}, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR0A0);
  stage(2, {GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_CPREV}, GX_TEXCOORD2, GX_TEXMAP2, GX_COLOR_NULL);
  TevStage& half = stage(3, {GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_HALF}, GX_TEXCOORD3, GX_TEXMAP3, GX_COLOR_NULL);
  half.colorOp.clamp = false;
  half.colorOp.outReg = GX_TEVREG0;
  TevStage& sub = stage(4, {GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_C0}, GX_TEXCOORD4, GX_TEXMAP3, GX_COLOR_NULL);
  sub.colorOp.op = GX_TEV_SUB;
  sub.colorOp.outReg = GX_TEVREG0;
  TevStage& last = stage(5, {GX_CC_ZERO, GX_CC_CPREV, GX_CC_C0, GX_CC_ZERO}, GX_TEXCOORD_NULL, GX_TEXMAP_NULL,
                         GX_COLOR_NULL);
  last.colorOp.scale = GX_CS_SCALE_2;
  last.alphaPass = {GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST};
  last.kaSel = GX_TEV_KASEL_K0_A;
  for (int i = 0; i < 4; ++i) {
    s.tcgs[i].type = GX_TG_MTX2x4;
    s.tcgs[i].src = GX_TG_POS;
  }
  // As GXSetTexCoordGen2 writes an emboss generator: its src GX_TG_TEX0 (row 5), read from set 3.
  s.tcgs[4].type = GX_TG_BUMP3;
  s.tcgs[4].src = GX_TG_TEX0;
  s.tcgs[4].embossSrc = 3;
  // Channel 0 lit by no light (its raster colour black), channel 1 by the bump light (no stage
  // reads it).
  for (const int part : {GX_COLOR0, GX_ALPHA0, GX_COLOR1, GX_ALPHA1}) {
    ColorChannelConfig& cfg = s.colorChannelConfig[part];
    cfg.lightingEnabled = true;
    cfg.matSrc = part == GX_COLOR0 || part == GX_ALPHA0 ? GX_SRC_VTX : GX_SRC_REG;
    cfg.ambSrc = GX_SRC_REG;
    cfg.diffFn = GX_DF_CLAMP;
    cfg.attnFn = GX_AF_SPOT;
  }
  s.colorChannelState[GX_COLOR1].lightMask.set(3);
  s.colorChannelState[GX_ALPHA1].lightMask.set(3);
}

// A record's flags byte (vita_tev.cpp's MaterialRecord: after its u32 format and two stage counts);
// bit 0, its program made lean (tev_program's `lean`).
constexpr size_t kRecordFlags = 6;

int self_test(bool lava, bool lean, const char* outDir) {
  using namespace aurora::gx;
  GXState& s = g_gxState;
  if (lava) {
    lava_state(s);
  } else {
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
  }
  std::vector<uint8_t> record = vita::tev_capture_material();
  if (lean && record.size() > kRecordFlags) {
    record[kRecordFlags] |= 1;
  }
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
  if (outDir != nullptr) {
    char name[17];
    std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(key));
    const std::filesystem::path dir{outDir};
    std::filesystem::create_directories(dir);
    std::ofstream manifest{dir / "manifest.txt"};
    manifest << name << ' ' << vertexSource.size() << ' ' << fragmentSource.size() << '\n';
    if (!write_file(dir / (std::string{name} + ".vcg"), vertexSource) ||
        !write_file(dir / (std::string{name} + ".fcg"), fragmentSource) || !manifest) {
      std::fprintf(stderr, "cannot write the sources to %s\n", outDir);
      return 1;
    }
  }
  return s.numTevStages == 0 && s.fog.type == GX_FOG_NONE ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "--self-test") == 0) {
    const bool lava = argc >= 3 && std::strcmp(argv[2], "lava") == 0;
    const bool lean = argc >= 3 && std::strcmp(argv[2], "lean") == 0;
    if (argc > (lava || lean ? 4 : 2)) {
      std::fprintf(stderr, "usage: %s --self-test [lava|lean [out dir]]\n", argv[0]);
      return 2;
    }
    return self_test(lava, lean, (lava || lean) && argc == 4 ? argv[3] : nullptr);
  }
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <materials.bin> <out dir> | --self-test [lava|lean [out dir]]\n", argv[0]);
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
