#include "vita_tev.hpp"

#include "../internal.hpp"
#include "shader_info.hpp"

#include <fmt/format.h>
#include <xxhash.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

namespace aurora::gx::vita {
namespace {
constexpr Module Log{"aurora::gx::vita"};
using vita_gl::kProgramCoords;
using vita_gl::kSamplers;
using vita_gl::kTexCoords;

// The constants' slots (vita_gl::kProgramConstants): konst colours, TEV registers, the channels'
// material and ambient colours, the alpha compare's references, the fog colour and parameters.
// Sources name them so, uConst[slot], until pack_constants renumbers them to the ones the source
// reads.
constexpr int kConstKonst = 0;
constexpr int kConstReg = 4;
constexpr int kConstMat0 = 8;
constexpr int kConstAlphaRef = 12;
constexpr int kConstFogColor = 13;
constexpr int kConstFogParams = 14;
// Indirect texturing's (Aurora's shader.cpp has them as texcoord_scale, texN_size_bias, ind_mtx): a
// coordinate slot's texel scale (GX's SU scale: its texture's size), a texture slot's size, and the
// indirect matrices, two rows each ((m0.x, m0.y, m1.x, m1.y), (m2.x, m2.y, 2^scaleExp, 0)).
constexpr int kConstTexCoordScale = 15;
constexpr int kConstTexSize = kConstTexCoordScale + vita_gl::kProgramCoords;
constexpr int kConstIndMtx = kConstTexSize + vita_gl::kSamplers;
// A coordinate slot's 2x4 matrix rows, where the fragment program applies it (TevLayout::fragmentMtx).
constexpr int kConstTexMtx = kConstIndMtx + 2 * MaxIndTexMtxs;
static_assert(kConstTexMtx + 2 * vita_gl::kProgramCoords == vita_gl::kProgramConstants);

// Programs by the hash of their source (an unordered_map's elements stay where they are), and by
// the hash of the GX state they were made from.
std::unordered_map<u64, TevProgram> sPrograms;
std::unordered_map<u64, const TevProgram*> sProgramsByState;

// The TEV register (0 PREV, 1-3 REG0-2) an argument reads, or -1.
int color_register(GXTevColorArg arg) noexcept {
  return arg <= GX_CC_A2 ? static_cast<int>(arg) / 2 : -1;
}
int alpha_register(GXTevAlphaArg arg) noexcept { return arg <= GX_CA_A2 ? static_cast<int>(arg) : -1; }

bool reads_texture(const TevStage& stage) noexcept {
  const auto& c = stage.colorPass;
  const auto& a = stage.alphaPass;
  const auto texC = [](GXTevColorArg arg) { return arg == GX_CC_TEXC || arg == GX_CC_TEXA; };
  return texC(c.a) || texC(c.b) || texC(c.c) || texC(c.d) || a.a == GX_CA_TEXA || a.b == GX_CA_TEXA ||
         a.c == GX_CA_TEXA || a.d == GX_CA_TEXA;
}

bool reads_raster(const TevStage& stage) noexcept {
  const auto& c = stage.colorPass;
  const auto& a = stage.alphaPass;
  const auto rasC = [](GXTevColorArg arg) { return arg == GX_CC_RASC || arg == GX_CC_RASA; };
  return rasC(c.a) || rasC(c.b) || rasC(c.c) || rasC(c.d) || a.a == GX_CA_RASA || a.b == GX_CA_RASA ||
         a.c == GX_CA_RASA || a.d == GX_CA_RASA;
}

bool reads_konst(const TevStage& stage) noexcept {
  const auto& c = stage.colorPass;
  const auto& a = stage.alphaPass;
  return c.a == GX_CC_KONST || c.b == GX_CC_KONST || c.c == GX_CC_KONST || c.d == GX_CC_KONST ||
         a.a == GX_CA_KONST || a.b == GX_CA_KONST || a.c == GX_CA_KONST || a.d == GX_CA_KONST;
}

bool bump_channel(const TevStage& stage) noexcept {
  return stage.channelId == GX_ALPHA_BUMP || stage.channelId == GX_ALPHA_BUMPN;
}

// The indirect stage a TEV stage reads, as Aurora's shader_info.cpp has it (its offset matrix, or
// the bump alpha as its raster colour), or -1.
int indirect_stage(const TevStage& stage) noexcept {
  return stage.indTexStage < g_gxState.numIndStages && (stage.indTexMtxId != GX_ITM_OFF || bump_channel(stage))
             ? static_cast<int>(stage.indTexStage)
             : -1;
}

// A stage with a TEV coordinate of its own (GX's indirect unit): its coordinate offset by an
// indirect stage, wrapped, or added to the previous stage's. Made only in a program that reads an
// indirect stage, as Aurora's shaders do.
bool tev_coord(const TevStage& stage) noexcept {
  return stage.indTexMtxId != GX_ITM_OFF || stage.indTexWrapS != GX_ITW_OFF || stage.indTexWrapT != GX_ITW_OFF ||
         stage.indTexAddPrev;
}

char component(GXTevColorChan chan) noexcept { return "rgba"[chan & 3]; }

std::string swizzle(const TevSwap& swap) {
  return {component(swap.red), component(swap.green), component(swap.blue), component(swap.alpha)};
}

// The lights a channel part (GX_COLOR0, GX_COLOR1, GX_ALPHA0, GX_ALPHA1) sums: those of its mask
// when it is lit, else none.
int light_count(int part) noexcept {
  return g_gxState.colorChannelConfig[part].lightingEnabled
             ? static_cast<int>(g_gxState.colorChannelState[part].lightMask.count())
             : 0;
}

// The lights a program sums for a part summing `count`: rounded up to 1, 2, 4 or 8, the rest dark
// (tev_vertex_rows). Each combination of the four parts' counts is a program of its own; exact
// counts made 17 combinations, 257 lit programs, and a session's compiles took 143 s.
int program_lights(int count) noexcept {
  return count <= 2 ? count : count <= 4 ? 4 : 8;
}

// A channel's raster colour (n 0 or 1), as GX lights it: the material colour (the vertex colour
// or the register), times the ambient colour plus the channel's lights, clamped. The vertex
// program sums the lights (vLit, rgb the colour part's, a the alpha part's: lighting_code).
std::string raster(int n) {
  const GXState& s = g_gxState;
  const std::string vertex = n == 0 ? "vRaw" : "float4(1.0)";
  const std::string mat = fmt::format("uConst[{}]", kConstMat0 + 2 * n);
  const std::string amb = fmt::format("uConst[{}]", kConstMat0 + 2 * n + 1);
  const auto part = [&](int chan, const char* sw) {
    const ColorChannelConfig& cfg = s.colorChannelConfig[chan];
    std::string m = fmt::format("{}.{}", cfg.matSrc == GX_SRC_VTX ? vertex : mat, sw);
    if (!cfg.lightingEnabled) {
      return m;
    }
    std::string a = fmt::format("{}.{}", cfg.ambSrc == GX_SRC_VTX ? vertex : amb, sw);
    if (light_count(chan) == 0) {
      return fmt::format("({} * saturate({}))", m, a);
    }
    return fmt::format("({} * saturate({} + vLit{}.{}))", m, a, n, sw);
  };
  return fmt::format("float4({}, {})", part(GX_COLOR0 + n, "rgb"), part(GX_ALPHA0 + n, "a"));
}

std::string konst_color(GXTevKColorSel sel) {
  const u32 v = sel;
  if (v <= GX_TEV_KCSEL_1_8) {
    return fmt::format("float3({:.6f})", static_cast<float>(8 - v) / 8.f);
  }
  if (v >= GX_TEV_KCSEL_K0 && v <= GX_TEV_KCSEL_K3) {
    return fmt::format("uConst[{}].rgb", kConstKonst + (v - GX_TEV_KCSEL_K0));
  }
  if (v >= GX_TEV_KCSEL_K0_R && v <= GX_TEV_KCSEL_K3_A) {
    const u32 comp = (v - GX_TEV_KCSEL_K0_R) / 4;
    const u32 reg = (v - GX_TEV_KCSEL_K0_R) % 4;
    return fmt::format("uConst[{}].{}{}{}", kConstKonst + reg, "rgba"[comp], "rgba"[comp], "rgba"[comp]);
  }
  return "float3(1.0)";
}

std::string konst_alpha(GXTevKAlphaSel sel) {
  const u32 v = sel;
  if (v <= GX_TEV_KASEL_1_8) {
    return fmt::format("{:.6f}", static_cast<float>(8 - v) / 8.f);
  }
  if (v >= GX_TEV_KASEL_K0_R && v <= GX_TEV_KASEL_K3_A) {
    const u32 comp = (v - GX_TEV_KASEL_K0_R) / 4;
    const u32 reg = (v - GX_TEV_KASEL_K0_R) % 4;
    return fmt::format("uConst[{}].{}", kConstKonst + reg, "rgba"[comp]);
  }
  return "1.0";
}

std::string color_arg(GXTevColorArg arg) {
  switch (arg) {
  case GX_CC_CPREV:
    return "R0.rgb";
  case GX_CC_APREV:
    return "R0.aaa";
  case GX_CC_C0:
    return "R1.rgb";
  case GX_CC_A0:
    return "R1.aaa";
  case GX_CC_C1:
    return "R2.rgb";
  case GX_CC_A1:
    return "R2.aaa";
  case GX_CC_C2:
    return "R3.rgb";
  case GX_CC_A2:
    return "R3.aaa";
  case GX_CC_TEXC:
    return "tex.rgb";
  case GX_CC_TEXA:
    return "tex.aaa";
  case GX_CC_RASC:
    return "ras.rgb";
  case GX_CC_RASA:
    return "ras.aaa";
  case GX_CC_ONE:
    return "float3(1.0)";
  case GX_CC_HALF:
    return "float3(0.5)";
  case GX_CC_KONST:
    return "konst.rgb";
  default:
    return "float3(0.0)";
  }
}

std::string alpha_arg(GXTevAlphaArg arg) {
  switch (arg) {
  case GX_CA_APREV:
    return "R0.a";
  case GX_CA_A0:
    return "R1.a";
  case GX_CA_A1:
    return "R2.a";
  case GX_CA_A2:
    return "R3.a";
  case GX_CA_TEXA:
    return "tex.a";
  case GX_CA_RASA:
    return "ras.a";
  case GX_CA_KONST:
    return "konst.a";
  default:
    return "0.0";
  }
}

const char* bias(GXTevBias b) noexcept {
  return b == GX_TB_ADDHALF ? " + 0.5" : b == GX_TB_SUBHALF ? " - 0.5" : "";
}

const char* scale(GXTevScale s) noexcept {
  switch (s) {
  case GX_CS_SCALE_2:
    return " * 2.0";
  case GX_CS_SCALE_4:
    return " * 4.0";
  case GX_CS_DIVIDE_2:
    return " * 0.5";
  default:
    return "";
  }
}

// One combiner's result: d +/- lerp(a, b, c), biased, scaled and clamped as GX's are, or a compare
// (d + c where a > b or a == b, by the component or the packed value the mode names). a, b and c
// are 8-bit in GX (clamped to [0, 1]); d and the result are 11-bit signed unless clamped.
std::string combine(const TevOp& op, const std::string& type, const std::string& a, const std::string& b,
                    const std::string& c, const std::string& d, bool alpha) {
  const std::string zero = type + "(0.0)";
  if (op.op == GX_TEV_ADD || op.op == GX_TEV_SUB) {
    std::string value = fmt::format("(({} {} lerp({}, {}, {})){}){}", d, op.op == GX_TEV_SUB ? "-" : "+", a, b, c,
                                    bias(op.bias), scale(op.scale));
    return op.clamp ? fmt::format("saturate({})", value) : fmt::format("clamp({}, -4.0, 4.0)", value);
  }
  const float epsilon = 0.5f / 255.f;
  const bool equal = (op.op & 1) != 0;
  std::string test;
  switch (op.op) {
  case GX_TEV_COMP_R8_GT:
  case GX_TEV_COMP_R8_EQ:
    test = alpha ? fmt::format("cA.r - cB.r") : fmt::format("{}.r - {}.r", a, b);
    break;
  case GX_TEV_COMP_GR16_GT:
  case GX_TEV_COMP_GR16_EQ:
    test = alpha ? "dot(cA.rg - cB.rg, float2(1.0, 256.0))"
                 : fmt::format("dot({}.rg - {}.rg, float2(1.0, 256.0))", a, b);
    break;
  case GX_TEV_COMP_BGR24_GT:
  case GX_TEV_COMP_BGR24_EQ:
    test = alpha ? "dot(cA.rgb - cB.rgb, float3(1.0, 256.0, 65536.0))"
                 : fmt::format("dot({}.rgb - {}.rgb, float3(1.0, 256.0, 65536.0))", a, b);
    break;
  default: // RGB8 (colour, by component) or A8 (alpha)
    if (equal) {
      return fmt::format("({} + {} * step(abs({} - {}), {}({:.6f})))", d, c, a, b, type, epsilon);
    }
    return fmt::format("({} + {} * step({}({:.6f}), {} - {}))", d, c, type, epsilon, a, b);
  }
  if (equal) {
    return fmt::format("({} + (abs({}) < {:.6f} ? {} : {}))", d, test, epsilon, c, zero);
  }
  return fmt::format("({} + ({} > {:.6f} ? {} : {}))", d, test, epsilon, c, zero);
}

std::string alpha_compare(GXCompare compare, const std::string& ref) {
  switch (compare) {
  case GX_NEVER:
    return "false";
  case GX_LESS:
    return fmt::format("(a8 < {})", ref);
  case GX_EQUAL:
    return fmt::format("(a8 == {})", ref);
  case GX_LEQUAL:
    return fmt::format("(a8 <= {})", ref);
  case GX_GREATER:
    return fmt::format("(a8 > {})", ref);
  case GX_NEQUAL:
    return fmt::format("(a8 != {})", ref);
  case GX_GEQUAL:
    return fmt::format("(a8 >= {})", ref);
  default:
    return "true";
  }
}

// A generator with a post-transform matrix (GX's dual texture matrices).
bool has_post(const TcgConfig& tcg) noexcept { return tcg.postMtx != GX_PTIDENTITY; }

// A generator whose coordinates are projective (sampled with tex2Dproj): a 3x4 matrix's, after a
// post-transform matrix too. A 2x4 one keeps its x and y whatever the post matrix makes of z, as
// Aurora's shaders do (shader.cpp): dividing those by it lost the environment maps (76cab02).
bool projective(const TcgConfig& tcg) noexcept { return tcg.type == GX_TG_MTX3x4; }

const char* coord_type(const TcgConfig& tcg) noexcept { return projective(tcg) ? "float3" : "float2"; }

// A coordinate slot's coordinates in the fragment program (a projective set's divided; one the
// fragment program makes, its own: fragment_body).
std::string fragment_uv(const TevLayout& layout, int coord) {
  if (layout.fragmentMtx[coord]) {
    return fmt::format("uv{}", coord);
  }
  return projective(g_gxState.tcgs[layout.coords[coord]]) ? fmt::format("(vUv{0}.xy / vUv{0}.z)", coord)
                                                          : fmt::format("vUv{}", coord);
}

// An indirect stage's coordinate scale, GX_ITS_1 to GX_ITS_256: 1 / 2^n.
std::string ind_scale(GXIndTexScale scale) {
  return fmt::format("{:.8f}", 1.f / static_cast<float>(1u << std::min<u32>(scale, GX_ITS_256)));
}

// A TEV coordinate's component (texels) wrapped as GX's indirect unit wraps it: modulo 256 to 16,
// or zero (GX_ITW_0), with fmod as Aurora's shaders use WGSL's %.
std::string ind_wrap(GXIndTexWrap wrap, std::string_view value) {
  static constexpr int kSizes[] = {0, 256, 128, 64, 32, 16};
  if (wrap == GX_ITW_OFF) {
    return std::string(value);
  }
  if (wrap >= GX_ITW_0) {
    return "0.0";
  }
  return fmt::format("fmod({}, {}.0)", value, kSizes[wrap]);
}

// The raster colour of a stage on GX_ALPHA_BUMP(N), as Aurora's alpha_bump_sel has it: its indirect
// stage's S, T or U (0-255), the format's low bits masked off (0xF8, 0xE0, 0xF0, 0xF8: kept in
// steps of 8, 32, 16, 8), as 0-1; GX_ALPHA_BUMPN's stretched by 255/248.
std::string bump_alpha(const TevStage& stage) {
  const int ind = indirect_stage(stage);
  if (ind < 0 || stage.indTexAlphaSel == GX_ITBA_OFF || stage.indTexAlphaSel > GX_ITBA_U) {
    return "0.0";
  }
  static constexpr int kSteps[] = {8, 32, 16, 8};
  const int step = kSteps[std::min<u32>(stage.indTexFormat, GX_ITF_3)];
  std::string alpha = fmt::format("(floor(ind{}.{} / {}.0) * {}.0 / 255.0)", ind,
                                  "xyz"[stage.indTexAlphaSel - GX_ITBA_S], step, step);
  return stage.channelId == GX_ALPHA_BUMPN ? fmt::format("({} * (255.0 / 248.0))", alpha) : alpha;
}

// A stage's TEV coordinate (GX's indirect unit, as Aurora's shaders make it): the indirect stage's
// texel, the format's bits and bias, through the matrix to an offset in texels; the stage's own
// coordinate in texels (as it is when nothing offsets or adds to it), wrapped; their sum, or that
// added to the previous stage's. Returns the coordinates the stage samples at ("": its own).
std::string tev_coord_code(const TevLayout& layout, u32 i, std::string& body) {
  const TevStage& stage = g_gxState.tevStages[i];
  const int coord = layout.stageCoord[i];
  const int ind = indirect_stage(stage);
  const bool simple = stage.indTexMtxId == GX_ITM_OFF && !stage.indTexAddPrev;
  std::string offset;
  if (stage.indTexMtxId != GX_ITM_OFF && ind >= 0) {
    static constexpr const char* kShifts[] = {"", " / 8.0", " / 16.0", " / 32.0"};
    const u32 format = std::min<u32>(stage.indTexFormat, GX_ITF_3);
    body += format == GX_ITF_8 ? fmt::format("    float3 ic = ind{};\n", ind)
                               : fmt::format("    float3 ic = floor(ind{}{});\n", ind, kShifts[format]);
    if (const u32 sel = stage.indTexBiasSel; sel != GX_ITB_NONE) {
      body += fmt::format("    ic += float3({}, {}, {}) * {};\n", (sel & 1) != 0 ? "1.0" : "0.0",
                          (sel & 2) != 0 ? "1.0" : "0.0", (sel & 4) != 0 ? "1.0" : "0.0",
                          format == GX_ITF_8 ? "-128.0" : "1.0");
    }
    const u32 mtx = stage.indTexMtxId;
    if (mtx >= GX_ITM_0 && mtx <= GX_ITM_2) {
      // The matrix's two rows: (m0.x, m0.y, m1.x, m1.y) and (m2.x, m2.y, 2^scaleExp, 0).
      const int row = kConstIndMtx + 2 * static_cast<int>(mtx - GX_ITM_0);
      offset = fmt::format("float2(dot(float3(uConst[{0}].xz, uConst[{1}].x), ic), "
                           "dot(float3(uConst[{0}].yw, uConst[{1}].y), ic)) * uConst[{1}].z",
                           row, row + 1);
    } else if (coord >= 0 && ((mtx >= GX_ITM_S0 && mtx <= GX_ITM_S2) || (mtx >= GX_ITM_T0 && mtx <= GX_ITM_T2))) {
      // A dynamic matrix: the stage's coordinate in texels times S or T, scaled.
      const bool dynamicS = mtx <= GX_ITM_S2;
      const int row = kConstIndMtx + 2 * static_cast<int>(mtx - (dynamicS ? GX_ITM_S0 : GX_ITM_T0)) + 1;
      offset = fmt::format("{} * uConst[{}].xy * ic.{} * uConst[{}].z / 256.0", fragment_uv(layout, coord),
                           kConstTexCoordScale + coord, dynamicS ? "x" : "y", row);
    }
  }
  std::string base;
  if (coord >= 0) {
    base = simple ? fragment_uv(layout, coord)
                  : fmt::format("{} * uConst[{}].xy", fragment_uv(layout, coord), kConstTexCoordScale + coord);
    if (stage.indTexWrapS != GX_ITW_OFF || stage.indTexWrapT != GX_ITW_OFF) {
      body += fmt::format("    float2 st = {};\n", base);
      base = fmt::format("float2({}, {})", ind_wrap(stage.indTexWrapS, "st.x"), ind_wrap(stage.indTexWrapT, "st.y"));
    }
  }
  const std::string value = !base.empty() && !offset.empty() ? fmt::format("{} + ({})", base, offset)
                            : !base.empty()                  ? base
                                                             : offset;
  if (value.empty()) {
    return {};
  }
  body += fmt::format("    tevCoord {}= {};\n", stage.indTexAddPrev ? "+" : "", value);
  if (layout.stageMap[i] < 0) {
    return {};
  }
  return simple ? "tevCoord" : fmt::format("(tevCoord / uConst[{}].xy)", kConstTexSize + layout.stageMap[i]);
}

// The fragment program's body: the stages, then the alpha compare.
std::string fragment_body(const TevLayout& layout) {
  const GXState& s = g_gxState;
  std::string body;
  // The slots the fragment program makes: their 2x4 matrix on their mesh set's coordinates.
  for (int c = 0; c < layout.coordCount; ++c) {
    if (layout.fragmentMtx[c]) {
      body += fmt::format("  float2 uv{0} = float2(dot(uConst[{1}], float4(vSet{2}, 1.0, 1.0)), "
                          "dot(uConst[{3}], float4(vSet{2}, 1.0, 1.0)));\n",
                          c, kConstTexMtx + 2 * c, layout.meshSet[c], kConstTexMtx + 2 * c + 1);
    }
  }
  const u32 stages = std::min<u32>(s.numTevStages, MaxTevStages);
  u32 lastOut = 0;
  // A register starts at its value from GX only where something reads it (its constant is then
  // declared); one only written starts at zero.
  bool registerRead[4] = {};
  if (layout.indirect) {
    // Each indirect stage read, sampled once as Aurora's shaders do: its coordinates in texels (the
    // SU scale), scaled by the stage's scale, back to the texture's [0, 1]; the texel's a, b and g
    // (S, T and U) as 0-255. Then the TEV coordinate the stages make, kept for add-previous.
    for (u32 ind = 0; ind < MaxIndStages; ++ind) {
      const int coord = layout.indCoord[ind];
      const int map = layout.indMap[ind];
      if (coord < 0) {
        continue;
      }
      body += fmt::format("  float3 ind{} = 255.0 * tex2D(uTex{}, {} * uConst[{}].xy * float2({}, {}) / uConst[{}].xy).abg;\n",
                          ind, map, fragment_uv(layout, coord), kConstTexCoordScale + coord,
                          ind_scale(s.indStages[ind].scaleS), ind_scale(s.indStages[ind].scaleT), kConstTexSize + map);
    }
    body += "  float2 tevCoord = float2(0.0);\n";
  }
  for (u32 i = 0; i < stages; ++i) {
    const TevStage& stage = s.tevStages[i];
    for (const GXTevColorArg arg : {stage.colorPass.a, stage.colorPass.b, stage.colorPass.c, stage.colorPass.d}) {
      if (const int reg = color_register(arg); reg >= 0) {
        registerRead[reg] = true;
      }
    }
    for (const GXTevAlphaArg arg : {stage.alphaPass.a, stage.alphaPass.b, stage.alphaPass.c, stage.alphaPass.d}) {
      if (const int reg = alpha_register(arg); reg >= 0) {
        registerRead[reg] = true;
      }
    }
    body += fmt::format("  {{ // stage {}\n", i);
    const std::string uv = layout.indirect && tev_coord(stage) ? tev_coord_code(layout, i, body) : std::string();
    if (layout.stageMap[i] >= 0 && !uv.empty()) {
      body += fmt::format("    float4 tex = tex2D(uTex{}, {}).{};\n", layout.stageMap[i], uv,
                          swizzle(s.tevSwapTable[stage.tevSwapTex]));
    } else if (layout.stageMap[i] >= 0) {
      // A projective set whole (tex2Dproj divides); a slot the fragment program makes, its own.
      const int coord = layout.stageCoord[i];
      const bool proj = projective(s.tcgs[layout.coords[coord]]);
      body += fmt::format("    float4 tex = {}(uTex{}, {}).{};\n", proj ? "tex2Dproj" : "tex2D", layout.stageMap[i],
                          proj ? fmt::format("vUv{}", coord) : fragment_uv(layout, coord),
                          swizzle(s.tevSwapTable[stage.tevSwapTex]));
    } else if (reads_texture(stage)) {
      body += "    float4 tex = float4(0.0);\n";
    }
    if (reads_raster(stage) && bump_channel(stage)) {
      body += fmt::format("    float4 ras = float4({});\n", bump_alpha(stage));
    } else if (reads_raster(stage)) {
      int channel = -1;
      switch (stage.channelId) {
      case GX_COLOR0:
      case GX_ALPHA0:
      case GX_COLOR0A0:
        channel = 0;
        break;
      case GX_COLOR1:
      case GX_ALPHA1:
      case GX_COLOR1A1:
        channel = 1;
        break;
      default:
        break;
      }
      if (channel < 0) {
        body += "    float4 ras = float4(0.0);\n";
      } else {
        body += fmt::format("    float4 ras = ({}).{};\n", raster(channel), swizzle(s.tevSwapTable[stage.tevSwapRas]));
      }
    }
    if (reads_konst(stage)) {
      body += fmt::format("    float4 konst = float4({}, {});\n", konst_color(stage.kcSel), konst_alpha(stage.kaSel));
    }
    const auto& cp = stage.colorPass;
    const auto& ap = stage.alphaPass;
    body += fmt::format("    float3 cA = saturate({});\n    float3 cB = saturate({});\n    float3 cC = saturate({});\n"
                        "    float3 cD = {};\n",
                        color_arg(cp.a), color_arg(cp.b), color_arg(cp.c), color_arg(cp.d));
    body += fmt::format("    float aA = saturate({});\n    float aB = saturate({});\n    float aC = saturate({});\n"
                        "    float aD = {};\n",
                        alpha_arg(ap.a), alpha_arg(ap.b), alpha_arg(ap.c), alpha_arg(ap.d));
    body += fmt::format("    float3 color = {};\n", combine(stage.colorOp, "float3", "cA", "cB", "cC", "cD", false));
    body += fmt::format("    float alpha = {};\n", combine(stage.alphaOp, "float", "aA", "aB", "aC", "aD", true));
    const u32 colorOut = stage.colorOp.outReg & 3;
    const u32 alphaOut = stage.alphaOp.outReg & 3;
    body += fmt::format("    R{}.rgb = color;\n    R{}.a = alpha;\n  }}\n", colorOut, alphaOut);
    lastOut = colorOut;
  }
  const AlphaCompare& compare = s.alphaCompare;
  if (compare.comp0 != GX_ALWAYS || compare.comp1 != GX_ALWAYS) {
    body += fmt::format("  float a8 = floor(saturate(R{}.a) * 255.0 + 0.5);\n", lastOut);
    const std::string pass0 = alpha_compare(compare.comp0, "uConst[12].x");
    const std::string pass1 = alpha_compare(compare.comp1, "uConst[12].y");
    std::string pass;
    switch (compare.op) {
    case GX_AOP_OR:
      pass = fmt::format("({} || {})", pass0, pass1);
      break;
    case GX_AOP_XOR:
      pass = fmt::format("({} != {})", pass0, pass1);
      break;
    case GX_AOP_XNOR:
      pass = fmt::format("({} == {})", pass0, pass1);
      break;
    default:
      pass = fmt::format("({} && {})", pass0, pass1);
      break;
    }
    body += fmt::format("  if (!{}) {{\n    discard;\n  }}\n", pass);
  }
  const GXFogType fogType = s.fog.type;
  if (fogType != GX_FOG_NONE) {
    // GX's fog, as Aurora's shaders have it (shader.cpp): from the depth the EFB keeps (the
    // vertex program's clip z and w; the viewport's depth range is in the projection, so GL's
    // window depth is GX's), perspective A / (B - z) or orthographic A * z, less C, then the
    // type's curve, the colour mixed toward the fog's. The range adjustment is not applied.
    body += "  float fogZ = vFog.x / vFog.y * 0.5 + 0.5;\n";
    body += (fogType & 0x08) != 0 ? fmt::format("  float fogF = uConst[{0}].x * fogZ;\n", kConstFogParams)
                                  : fmt::format("  float fogF = uConst[{0}].x / (uConst[{0}].y - fogZ);\n",
                                                kConstFogParams);
    body += fmt::format("  fogF = saturate(fogF - uConst[{}].z);\n", kConstFogParams);
    switch (fogType & 0x07) {
    case GX_FOG_PERSP_EXP:
      body += "  float fog = 1.0 - exp2(-8.0 * fogF);\n";
      break;
    case GX_FOG_PERSP_EXP2:
      body += "  float fog = 1.0 - exp2(-8.0 * fogF * fogF);\n";
      break;
    case GX_FOG_PERSP_REVEXP:
      body += "  float fog = exp2(-8.0 * (1.0 - fogF));\n";
      break;
    case GX_FOG_PERSP_REVEXP2:
      body += "  fogF = 1.0 - fogF;\n  float fog = exp2(-8.0 * fogF * fogF);\n";
      break;
    default:
      body += "  float fog = fogF;\n";
      break;
    }
    body += fmt::format("  R{0}.rgb = lerp(saturate(R{0}.rgb), uConst[{1}].rgb, saturate(fog));\n", lastOut,
                        kConstFogColor);
  }
  body += fmt::format("  return R{};\n", lastOut);
  // The result's register is read at the end (its alpha may be from before the last stage).
  registerRead[lastOut] = true;
  std::string registers;
  for (int reg = 0; reg < 4; ++reg) {
    registers += registerRead[reg] ? fmt::format("  float4 R{} = uConst[{}];\n", reg, kConstReg + reg)
                                   : fmt::format("  float4 R{} = float4(0.0);\n", reg);
  }
  return registers + body;
}

// Renumbers a fragment source's uConst[slot] to the constants it reads, packed in slot order,
// and declares only those: the fewer a program has, the less a draw uploads.
std::string pack_constants(const std::string& source, TevProgram& program) {
  static constexpr std::string_view kTag = "uConst[";
  std::array<int, vita_gl::kProgramConstants> packed;
  packed.fill(-1);
  for (size_t pos = source.find(kTag); pos != std::string::npos; pos = source.find(kTag, pos)) {
    pos += kTag.size();
    const int slot = std::atoi(source.c_str() + pos);
    if (slot >= 0 && slot < vita_gl::kProgramConstants) {
      packed[slot] = 0;
    }
  }
  program.constantCount = 0;
  for (int slot = 0; slot < vita_gl::kProgramConstants; ++slot) {
    if (packed[slot] == 0) {
      packed[slot] = program.constantCount;
      program.constantSlots[program.constantCount++] = static_cast<u8>(slot);
    }
  }
  std::string out;
  out.reserve(source.size());
  size_t last = 0;
  for (size_t pos = source.find(kTag); pos != std::string::npos; pos = source.find(kTag, last)) {
    pos += kTag.size();
    const size_t end = source.find(']', pos);
    out.append(source, last, pos - last);
    out += std::to_string(packed[std::atoi(source.c_str() + pos)]);
    last = end;
  }
  out.append(source, last, std::string::npos);
  return out;
}

// The vertex program's sum of channel part `chan`'s `count` lights (rows from `row`, five a
// light: position, direction, colour, angle and distance attenuation) into `target`, as GX lights
// a vertex and Aurora's shaders do (shader.cpp, lighting_func): `mv` is the view-space position,
// `n` the view-space normal. GX's lights are given in view space.
std::string lighting_code(int chan, int count, int row, const std::string& target) {
  const ColorChannelConfig& cfg = g_gxState.colorChannelConfig[chan];
  const char* sw = chan >= GX_ALPHA0 ? "a" : "rgb";
  std::string code;
  for (int l = 0; l < count; ++l) {
    const int r = row + 5 * l;
    code += fmt::format("  {{ // light {}\n"
                        "    float3 ldir = uVert[{}].xyz - mv;\n"
                        "    float dist2 = dot(ldir, ldir);\n"
                        "    float dist = sqrt(dist2);\n"
                        "    ldir = ldir / dist;\n",
                        l, r);
    switch (cfg.attnFn) {
    case GX_AF_SPOT:
      code += fmt::format("    float cosine = max(0.0, dot(ldir, uVert[{0}].xyz));\n"
                          "    float attn = max(0.0, dot(uVert[{1}].xyz, float3(1.0, cosine, cosine * cosine)) /\n"
                          "                          dot(uVert[{2}].xyz, float3(1.0, dist, dist2)));\n",
                          r + 1, r + 3, r + 4);
      break;
    case GX_AF_SPEC:
      code += fmt::format("    float attn = dot(n, ldir) >= 0.0 ? max(0.0, dot(n, uVert[{0}].xyz)) : 0.0;\n"
                          "    float cosAttn = dot(uVert[{1}].xyz, float3(1.0, attn, attn * attn));\n"
                          "    float distAttn = max(0.0, dot({2}, float3(1.0, attn, attn * attn)));\n"
                          "    attn = max(0.0, cosAttn / distAttn);\n",
                          r + 1, r + 3,
                          cfg.diffFn != GX_DF_NONE ? fmt::format("normalize(uVert[{}].xyz)", r + 4)
                                                   : fmt::format("uVert[{}].xyz", r + 4));
      break;
    default:
      code += "    float attn = 1.0;\n";
      break;
    }
    switch (cfg.diffFn) {
    case GX_DF_SIGN:
      code += "    float diff = dot(ldir, n);\n";
      break;
    case GX_DF_CLAMP:
      code += "    float diff = max(0.0, dot(ldir, n));\n";
      break;
    default:
      code += "    float diff = 1.0;\n";
      break;
    }
    code += fmt::format("    {} += attn * diff * uVert[{}].{};\n  }}\n", target, r + 2, sw);
  }
  return code;
}

// A coordinate generator's source as the vertex program has it, (x, y, z, 1) as GX takes it: a
// vertex attribute's from the mesh set `set` holding it.
std::string texgen_input(const TcgConfig& tcg, int set) {
  if (tcg.src == GX_TG_POS) {
    return "float4(aPos, 1.0)";
  }
  if (tcg.src == GX_TG_NRM) {
    return "float4(aNrm, 1.0)";
  }
  return fmt::format("float4(aUv{}, 1.0, 1.0)", set);
}

const float kIdentityRows[12] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f};

// A coordinate generator's matrix, three rows of four: GX numbers the position matrices first,
// then the texture matrices, three rows apiece; the identity's for GX_IDENTITY.
const float* generator_rows(const TcgConfig& tcg) noexcept {
  const GXState& s = g_gxState;
  if (tcg.mtx != GX_IDENTITY) {
    const u32 index = static_cast<u32>(tcg.mtx) / 3;
    if (index < MaxPnMtx) {
      return reinterpret_cast<const float*>(&s.pnMtx[index].pos);
    }
    if (index - MaxPnMtx < MaxTexMtx) {
      return reinterpret_cast<const float*>(&s.texMtxs[index - MaxPnMtx]);
    }
  }
  return kIdentityRows;
}
} // namespace

bool tev_enabled() noexcept {
  static const bool enabled = [] {
    const char* value = std::getenv("MP_VITA_TEV");
    return value == nullptr || std::strcmp(value, "0") != 0;
  }();
  return enabled;
}

TevLayout tev_layout() noexcept {
  const GXState& s = g_gxState;
  TevLayout layout;
  layout.stageCoord.fill(-1);
  layout.stageMap.fill(-1);
  layout.indCoord.fill(-1);
  layout.indMap.fill(-1);
  layout.meshSet.fill(-1);
  layout.meshSetSource.fill(-1);
  // The mesh set holding vertex attribute `src` (GX_TG_TEXn) for slot `slot`: the set already
  // holding it, else the set of the slot's own index where that is free (as each slot had its own
  // set before sets were shared: those programs keep their sources), else the first free; -1 when
  // the mesh's sets are full.
  const auto mesh_set = [&layout](int slot, int src) -> int {
    int free = -1;
    for (int set = 0; set < kTexCoords; ++set) {
      if (layout.meshSetSource[set] == src) {
        return set;
      }
      if (layout.meshSetSource[set] < 0 && (free < 0 || set == slot)) {
        free = set;
      }
    }
    if (free >= 0) {
      layout.meshSetSource[free] = static_cast<s8>(src);
    }
    return free;
  };
  // A coordinate generator's slot (taken at its first use), or -1 where a program cannot make its
  // coordinates (colours, binormals, another generator or a bump map as the source: not yet) or
  // the slots or the mesh's sets are full.
  const auto coord_slot = [&s, &layout, &mesh_set](u32 id) -> int {
    if (id >= MaxTexCoord) {
      return -1;
    }
    for (int c = 0; c < layout.coordCount; ++c) {
      if (layout.coords[c] == id) {
        return c;
      }
    }
    const TcgConfig& tcg = s.tcgs[id];
    const bool fromTex = tcg.src >= GX_TG_TEX0 && tcg.src <= GX_TG_TEX7;
    if ((tcg.type != GX_TG_MTX2x4 && tcg.type != GX_TG_MTX3x4) ||
        (!fromTex && tcg.src != GX_TG_POS && tcg.src != GX_TG_NRM) ||
        (fromTex && s.vtxDesc[GX_VA_TEX0 + (tcg.src - GX_TG_TEX0)] == GX_NONE) ||
        (has_post(tcg) && (tcg.postMtx - GX_PTTEXMTX0) / 3 >= MaxPTTexMtx) || layout.coordCount == kProgramCoords) {
      return -1;
    }
    const int slot = layout.coordCount;
    if (fromTex) {
      const int set = mesh_set(slot, static_cast<int>(tcg.src));
      if (set < 0) {
        return -1;
      }
      layout.meshSet[slot] = static_cast<s8>(set);
    }
    layout.coords[slot] = static_cast<GXTexCoordID>(id);
    return layout.coordCount++;
  };
  const auto map_slot = [&layout](u32 id) -> int {
    if (id >= MaxTextures) {
      return -1;
    }
    for (int m = 0; m < layout.samplerCount; ++m) {
      if (layout.maps[m] == id) {
        return m;
      }
    }
    if (layout.samplerCount == kSamplers) {
      return -1;
    }
    layout.maps[layout.samplerCount] = static_cast<GXTexMapID>(id);
    return layout.samplerCount++;
  };
  const u32 stages = std::min<u32>(s.numTevStages, MaxTevStages);
  for (u32 i = 0; i < stages; ++i) {
    layout.indirect = layout.indirect || indirect_stage(s.tevStages[i]) >= 0;
  }
  for (u32 i = 0; i < stages; ++i) {
    const TevStage& stage = s.tevStages[i];
    // The indirect stage it reads, sampled once for every stage reading it (fragment_body).
    if (const int ind = indirect_stage(stage); ind >= 0 && layout.indCoord[ind] < 0) {
      const int coord = coord_slot(s.indStages[ind].texCoordId);
      const int map = map_slot(s.indStages[ind].texMapId);
      if (coord < 0 || map < 0) {
        return layout;
      }
      layout.indCoord[ind] = static_cast<s8>(coord);
      layout.indMap[ind] = static_cast<s8>(map);
    }
    // Its own coordinate: sampled with, or made into a TEV coordinate (offset, or carried to a later
    // stage that adds to it).
    const bool baseCoord = stage.texCoordId < MaxTexCoord && stage.texMapId < MaxTextures;
    const bool sampled = reads_texture(stage) && baseCoord;
    if (sampled || (layout.indirect && tev_coord(stage) && baseCoord)) {
      const int coord = coord_slot(stage.texCoordId);
      if (coord < 0) {
        return layout;
      }
      layout.stageCoord[i] = static_cast<s8>(coord);
    }
    if (sampled) {
      const int map = map_slot(stage.texMapId);
      if (map < 0) {
        return layout;
      }
      layout.stageMap[i] = static_cast<s8>(map);
    }
  }
  if (layout.coordCount > kTexCoords) {
    for (int c = 0; c < layout.coordCount; ++c) {
      const TcgConfig& tcg = s.tcgs[layout.coords[c]];
      layout.fragmentMtx[c] = layout.meshSet[c] >= 0 && tcg.type == GX_TG_MTX2x4 && !tcg.normalize && !has_post(tcg);
    }
  }
  layout.supported = true;
  return layout;
}

const TevProgram* tev_program(const TevLayout& layout) noexcept {
  const GXState& s = g_gxState;
  // What the source is made from, hashed: generating it is the costly part, and most materials
  // share a program with another.
  const u32 stages = std::min<u32>(s.numTevStages, MaxTevStages);
  const AlphaCompare& compare = s.alphaCompare;
  // A coordinate set by its generator's type, source (a texture coordinate, the position or the
  // normal), normalisation and whether it has a post-transform matrix; the lights each channel
  // part sums.
  constexpr int kCoordKeys = 5;
  constexpr int kLightKeys = kCoordKeys + kProgramCoords;
  u32 fixed[kLightKeys + 4] = {stages, static_cast<u32>(compare.comp0), static_cast<u32>(compare.comp1),
                               static_cast<u32>(compare.op), static_cast<u32>(s.fog.type)};
  for (int c = 0; c < layout.coordCount; ++c) {
    const TcgConfig& tcg = s.tcgs[layout.coords[c]];
    const u32 source = tcg.src == GX_TG_POS ? 1 : tcg.src == GX_TG_NRM ? 2 : 0;
    fixed[kCoordKeys + c] = (static_cast<u32>(tcg.type) + 1) | source << 8 | (tcg.normalize ? 1u : 0u) << 12 |
                            (has_post(tcg) ? 1u : 0u) << 13;
  }
  for (int part = 0; part < 4; ++part) {
    fixed[kLightKeys + part] = static_cast<u32>(program_lights(light_count(part)));
  }
  u64 stateKey = XXH3_64bits(fixed, sizeof(fixed));
  stateKey = XXH3_64bits_withSeed(s.tevStages.data(), stages * sizeof(TevStage), stateKey);
  stateKey = XXH3_64bits_withSeed(s.tevSwapTable.data(), sizeof(s.tevSwapTable), stateKey);
  stateKey = XXH3_64bits_withSeed(s.colorChannelConfig.data(), sizeof(s.colorChannelConfig), stateKey);
  // The layout field by field (its padding is not set). The slots each stage samples are the
  // program's; which coordinate generator and texture fill them is not.
  stateKey = XXH3_64bits_withSeed(layout.stageCoord.data(), sizeof(layout.stageCoord), stateKey);
  stateKey = XXH3_64bits_withSeed(layout.stageMap.data(), sizeof(layout.stageMap), stateKey);
  const int counts[2] = {layout.coordCount, layout.samplerCount};
  stateKey = XXH3_64bits_withSeed(counts, sizeof(counts), stateKey);
  // The mesh set each slot reads (the vertex program's attributes), and the slots the fragment
  // program makes.
  stateKey = XXH3_64bits_withSeed(layout.meshSet.data(), sizeof(layout.meshSet), stateKey);
  stateKey = XXH3_64bits_withSeed(layout.fragmentMtx.data(), sizeof(layout.fragmentMtx), stateKey);
  // The indirect stages read: their slots and scales (the stages' indirect setup is in theirs).
  stateKey = XXH3_64bits_withSeed(layout.indCoord.data(), sizeof(layout.indCoord), stateKey);
  stateKey = XXH3_64bits_withSeed(layout.indMap.data(), sizeof(layout.indMap), stateKey);
  u32 indScales[MaxIndStages] = {};
  for (u32 ind = 0; ind < MaxIndStages; ++ind) {
    if (layout.indCoord[ind] >= 0) {
      indScales[ind] = static_cast<u32>(s.indStages[ind].scaleS) | static_cast<u32>(s.indStages[ind].scaleT) << 8;
    }
  }
  stateKey = XXH3_64bits_withSeed(indScales, sizeof(indScales), stateKey);
  if (const auto it = sProgramsByState.find(stateKey); it != sProgramsByState.end()) {
    return it->second;
  }
  TevProgram info;
  info.indirect = layout.indirect;
  const std::string body = pack_constants(fragment_body(layout), info);
  const bool raw = body.find("vRaw") != std::string::npos;
  // The channels whose lights the raster colours take (raster), and so the lights summed.
  bool litChannel[2] = {body.find("vLit0") != std::string::npos, body.find("vLit1") != std::string::npos};
  bool lit = false;
  for (int n = 0; n < 2; ++n) {
    for (const int part : {GX_COLOR0 + n, GX_ALPHA0 + n}) {
      info.lightCounts[part] = static_cast<u8>(litChannel[n] ? program_lights(light_count(part)) : 0);
      lit = lit || info.lightCounts[part] != 0;
    }
  }
  bool normals = lit;
  for (int c = 0; c < layout.coordCount; ++c) {
    normals = normals || s.tcgs[layout.coords[c]].src == GX_TG_NRM;
  }

  // uVert (vita_gl::kProgramVertexConstants, tev_vertex_rows): the projection times the position
  // matrix; the coordinate sets' matrix rows (and post-transform rows); for lighting the position
  // and normal matrices and the lights.
  std::string fragmentParams;
  std::string vertexParams = "float3 aPos";
  std::string vertexOuts = "float4 out vPos : POSITION";
  std::string vertexBody = "  float4 p = float4(aPos, 1.0);\n"
                           "  vPos = float4(dot(uVert[0], p), dot(uVert[1], p), dot(uVert[2], p), dot(uVert[3], p));\n";
  int row = 4;
  const auto add = [](std::string& list, const std::string& item) {
    list += list.empty() ? item : ", " + item;
  };
  if (raw) {
    add(vertexParams, "float4 aRaw");
    add(vertexOuts, "float4 out vRaw : COLOR");
    vertexBody += "  vRaw = aRaw;\n";
    add(fragmentParams, "float4 vRaw : COLOR");
  }
  if (normals) {
    add(vertexParams, "float3 aNrm");
  }
  // The fog's depth and the lit channels' sums go in the TEXCOORD varyings past the coordinate
  // slots' (4, then 5 and 6, with four slots or fewer).
  const int extraVarying = std::max(kTexCoords, layout.coordCount);
  if (body.find("vFog") != std::string::npos) {
    // The fog's depth: clip z and w, divided per fragment (GL's window depth).
    add(vertexOuts, fmt::format("float2 out vFog : TEXCOORD{}", extraVarying));
    vertexBody += "  vFog = float2(dot(uVert[2], p), dot(uVert[3], p));\n";
    add(fragmentParams, fmt::format("float2 vFog : TEXCOORD{}", extraVarying));
  }
  for (int c = 0; c < layout.coordCount; ++c) {
    // As GX generates a coordinate: the source times the matrix (z 1 after a 2x4 one), normalised
    // when asked, then the post-transform matrix (Aurora's shader.cpp does the same).
    const TcgConfig& tcg = s.tcgs[layout.coords[c]];
    const char* type = coord_type(tcg);
    // A mesh set's attribute, declared by the first slot reading it.
    const int set = layout.meshSet[c];
    bool declared = false;
    for (int earlier = 0; earlier < c; ++earlier) {
      declared = declared || (set >= 0 && layout.meshSet[earlier] == set);
    }
    if (set >= 0 && !declared) {
      add(vertexParams, fmt::format("float2 aUv{}", set));
    }
    if (layout.fragmentMtx[c]) {
      // Made by the fragment program (fragment_body): its set's coordinates as they are, passed once
      // a set, in the TEXCOORD of the set's first such slot.
      info.fragmentMtx[c] = true;
      bool passed = false;
      for (int earlier = 0; earlier < c; ++earlier) {
        passed = passed || (layout.fragmentMtx[earlier] && layout.meshSet[earlier] == set);
      }
      if (!passed) {
        add(vertexOuts, fmt::format("float2 out vSet{} : TEXCOORD{}", set, c));
        vertexBody += fmt::format("  vSet{0} = aUv{0};\n", set);
        add(fragmentParams, fmt::format("float2 vSet{} : TEXCOORD{}", set, c));
      }
      continue;
    }
    add(vertexOuts, fmt::format("{} out vUv{} : TEXCOORD{}", type, c, c));
    vertexBody += fmt::format("  float4 t{} = {};\n", c, texgen_input(tcg, set));
    if (tcg.type == GX_TG_MTX3x4) {
      vertexBody += fmt::format("  float3 tc{0} = float3(dot(uVert[{1}], t{0}), dot(uVert[{2}], t{0}), dot(uVert[{3}], t{0}));\n",
                                c, row, row + 1, row + 2);
      row += 3;
    } else {
      vertexBody += fmt::format("  float3 tc{0} = float3(dot(uVert[{1}], t{0}), dot(uVert[{2}], t{0}), 1.0);\n", c,
                                row, row + 1);
      row += 2;
    }
    if (tcg.normalize) {
      vertexBody += fmt::format("  tc{0} = normalize(tc{0});\n", c);
    }
    if (has_post(tcg)) {
      info.post[c] = true;
      vertexBody += fmt::format("  tc{0} = float3(dot(uVert[{1}], float4(tc{0}, 1.0)), dot(uVert[{2}], float4(tc{0}, 1.0)),\n"
                                "               dot(uVert[{3}], float4(tc{0}, 1.0)));\n",
                                c, row, row + 1, row + 2);
      row += 3;
    }
    vertexBody += fmt::format("  vUv{0} = tc{0}{1};\n", c, projective(tcg) ? "" : ".xy");
    add(fragmentParams, fmt::format("{} vUv{} : TEXCOORD{}", type, c, c));
  }
  if (lit) {
    // The view-space position and normal (a zero normal stays zero), then each lit channel's lights.
    vertexBody += fmt::format("  float3 mv = float3(dot(uVert[{0}], p), dot(uVert[{1}], p), dot(uVert[{2}], p));\n"
                              "  float4 nn = float4(aNrm, 0.0);\n"
                              "  float3 n = float3(dot(uVert[{3}], nn), dot(uVert[{4}], nn), dot(uVert[{5}], nn));\n"
                              "  n = dot(n, n) > 1e-10 ? normalize(n) : n;\n",
                              row, row + 1, row + 2, row + 3, row + 4, row + 5);
    row += 6;
    for (int n = 0; n < 2; ++n) {
      if (!litChannel[n]) {
        continue;
      }
      add(vertexOuts, fmt::format("float4 out vLit{0} : TEXCOORD{1}", n, extraVarying + 1 + n));
      add(fragmentParams, fmt::format("float4 vLit{0} : TEXCOORD{1}", n, extraVarying + 1 + n));
      vertexBody += fmt::format("  vLit{} = float4(0.0);\n", n);
      for (const int part : {GX_COLOR0 + n, GX_ALPHA0 + n}) {
        const int count = info.lightCounts[part];
        vertexBody += lighting_code(part, count, row, fmt::format("vLit{}.{}", n, part >= GX_ALPHA0 ? "a" : "rgb"));
        row += 5 * count;
      }
    }
  }
  info.vertexCount = row;
  for (int m = 0; m < layout.samplerCount; ++m) {
    add(fragmentParams, fmt::format("uniform sampler2D uTex{0} : TEXUNIT{0}", m));
  }
  if (info.constantCount != 0) {
    add(fragmentParams, fmt::format("uniform float4 uConst[{}]", info.constantCount));
  }
  const std::string vertexUniforms = fmt::format("uniform float4 uVert[{}]", row);
  std::string vertexSource =
      fmt::format("void main({}, {}, {}) {{\n{}}}\n", vertexParams, vertexUniforms, vertexOuts, vertexBody);
  std::string fragmentSource = fmt::format("float4 main({}) : COLOR {{\n{}}}\n", fragmentParams, body);

  // The key also names the program's binary on the memory card (vita_gl.cpp); the seed changes
  // when what a source compiles to could (the compiler's options, the attribute layout).
  constexpr u64 kBinaryVersion = 3;
  u64 key = XXH3_64bits_withSeed(vertexSource.data(), vertexSource.size(), kBinaryVersion);
  key = XXH3_64bits_withSeed(fragmentSource.data(), fragmentSource.size(), key);
  TevProgram& program = sPrograms[key];
  if (!program.program) {
    // The first indirect programs' sources, for the log (they compile on the Vita alone).
    static int sIndirectLogged = 0;
    if (info.indirect && sIndirectLogged < 2) {
      ++sIndirectLogged;
      Log.info("an indirect program ({} textures, {} coordinate sets):\n-- vertex --\n{}-- fragment --\n{}",
               layout.samplerCount, layout.coordCount, vertexSource, fragmentSource);
    }
    info.program = vita_gl::make_program(std::move(vertexSource), std::move(fragmentSource), key);
    program = std::move(info);
  }
  sProgramsByState.emplace(stateKey, &program);
  return &program;
}

void tev_constants(const TevProgram& program, const u8* coordIds, const u8* mapIds,
                   float out[vita_gl::kProgramConstants][4]) noexcept {
  const GXState& s = g_gxState;
  for (int i = 0; i < program.constantCount; ++i) {
    const int slot = program.constantSlots[i];
    float* value = out[i];
    if (slot < kConstReg) {
      std::memcpy(value, &s.kcolors[slot - kConstKonst], sizeof(float[4]));
    } else if (slot < kConstMat0) {
      std::memcpy(value, &s.colorRegs[slot - kConstReg], sizeof(float[4]));
    } else if (slot < kConstAlphaRef) {
      const int n = (slot - kConstMat0) / 2;
      const bool ambient = ((slot - kConstMat0) & 1) != 0;
      const ColorChannelState& color = s.colorChannelState[GX_COLOR0 + n];
      const ColorChannelState& alpha = s.colorChannelState[GX_ALPHA0 + n];
      const auto& rgb = ambient ? color.ambColor : color.matColor;
      value[0] = rgb[0];
      value[1] = rgb[1];
      value[2] = rgb[2];
      value[3] = (ambient ? alpha.ambColor : alpha.matColor)[3];
    } else if (slot == kConstAlphaRef) {
      value[0] = static_cast<float>(s.alphaCompare.ref0 & 0xFF);
      value[1] = static_cast<float>(s.alphaCompare.ref1 & 0xFF);
      value[2] = 0.f;
      value[3] = 0.f;
    } else if (slot == kConstFogColor) {
      std::memcpy(value, &s.fog.color, sizeof(float[4]));
    } else if (slot == kConstFogParams) {
      value[0] = s.fog.a;
      value[1] = s.fog.b;
      value[2] = s.fog.c;
      value[3] = 0.f;
    } else if (slot < kConstTexSize) {
      // GX's SU scale holds the size less one (Aurora's texcoord_scale adds it back).
      const TexCoordScale& scale = s.texCoordScales[coordIds[slot - kConstTexCoordScale]];
      value[0] = static_cast<float>(scale.scaleS) + 1.f;
      value[1] = static_cast<float>(scale.scaleT) + 1.f;
      value[2] = 0.f;
      value[3] = 0.f;
    } else if (slot < kConstIndMtx) {
      const GXTexObj_& texture = s.loadedTextures[mapIds[slot - kConstTexSize]];
      value[0] = static_cast<float>(texture.width());
      value[1] = static_cast<float>(texture.height());
      value[2] = 0.f;
      value[3] = 0.f;
    } else if (slot < kConstTexMtx) {
      const IndTexMtxInfo& matrix = s.indTexMtxs[(slot - kConstIndMtx) / 2];
      const auto& m = matrix.mtx;
      if (((slot - kConstIndMtx) & 1) == 0) {
        value[0] = m.m0.x;
        value[1] = m.m0.y;
        value[2] = m.m1.x;
        value[3] = m.m1.y;
      } else {
        value[0] = m.m2.x;
        value[1] = m.m2.y;
        value[2] = std::exp2(static_cast<float>(matrix.scaleExp));
        value[3] = 0.f;
      }
    } else {
      // A slot the fragment program makes: its generator's row.
      const TcgConfig& tcg = s.tcgs[coordIds[(slot - kConstTexMtx) / 2]];
      std::memcpy(value, generator_rows(tcg) + 4 * ((slot - kConstTexMtx) & 1), sizeof(float[4]));
    }
  }
}

int tev_vertex_rows(const TevProgram& program, const TcgConfig* const coords[], int coordCount,
                    float (*out)[4]) noexcept {
  static constexpr float kIdentity[12] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f};
  const GXState& s = g_gxState;
  int row = 4;
  for (int c = 0; c < coordCount; ++c) {
    if (program.fragmentMtx[c]) {
      continue; // its matrix is the fragment program's (tev_constants)
    }
    const TcgConfig& tcg = *coords[c];
    const float* rows = generator_rows(tcg);
    const int count = tcg.type == GX_TG_MTX3x4 ? 3 : 2;
    std::memcpy(out[row], rows, sizeof(float[4]) * count);
    row += count;
    if (program.post[c]) {
      const u32 post = (static_cast<u32>(tcg.postMtx) - GX_PTTEXMTX0) / 3;
      std::memcpy(out[row], post < MaxPTTexMtx ? reinterpret_cast<const float*>(&s.ptTexMtxs[post]) : kIdentity,
                  sizeof(float[12]));
      row += 3;
    }
  }
  bool lit = false;
  for (const u8 count : program.lightCounts) {
    lit = lit || count != 0;
  }
  if (!lit) {
    return row;
  }
  const PnMtx& matrices = s.pnMtx[s.currentPnMtx];
  std::memcpy(out[row], &matrices.pos, sizeof(float[12]));
  std::memcpy(out[row + 3], &matrices.nrm, sizeof(float[12]));
  row += 6;
  // Each lit channel part's lights, in tev_program's order: its mask's first ones (a light short of
  // the count, which the mask changing without the program would make, is dark).
  for (int n = 0; n < 2; ++n) {
    for (const int part : {GX_COLOR0 + n, GX_ALPHA0 + n}) {
      const GX::LightMask& mask = s.colorChannelState[part].lightMask;
      int written = 0;
      for (u32 i = 0; i < GX::MaxLights && written < program.lightCounts[part]; ++i) {
        if (mask.test(i)) {
          static_assert(sizeof(Light) == sizeof(float[20]));
          std::memcpy(out[row], &s.lights[i], sizeof(Light));
          row += 5;
          ++written;
        }
      }
      // The program's lights past the mask's (program_lights rounds the count up): black, far off,
      // with both attenuations' first coefficient 1, so every function a light's attenuation can
      // use stays finite (an all-zero light divides zero by zero: NaN, which black does not cancel).
      static constexpr float kDark[20] = {1e5f, 1e5f, 1e5f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f,
                                          0.f,  0.f,  1.f,  0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f};
      for (; written < program.lightCounts[part]; ++written) {
        std::memcpy(out[row], kDark, sizeof(kDark));
        row += 5;
      }
    }
  }
  return row;
}

size_t tev_program_count() noexcept { return sPrograms.size(); }

TevProgramCounts tev_program_counts() noexcept {
  TevProgramCounts counts;
  counts.all = sPrograms.size();
  std::unordered_map<u32, bool> combinations;
  for (const auto& [_, program] : sPrograms) {
    counts.indirect += program.indirect ? 1 : 0;
    u32 packed = 0;
    for (int part = 0; part < 4; ++part) {
      packed |= static_cast<u32>(program.lightCounts[part]) << (8 * part);
    }
    if (packed != 0) {
      ++counts.lit;
      combinations[packed] = true;
    }
  }
  counts.lightCombinations = combinations.size();
  return counts;
}
} // namespace aurora::gx::vita
