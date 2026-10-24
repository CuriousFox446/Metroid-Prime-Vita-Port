#include "vita_tev.hpp"

#include "../internal.hpp"
#include "shader_info.hpp"

#include <fmt/format.h>
#include <xxhash.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace aurora::gx::vita {
namespace {
using vita_gl::kSamplers;
using vita_gl::kTexCoords;

// uConst's layout (vita_gl::kProgramConstants): konst colours, TEV registers, the channels'
// material and ambient colours, the alpha compare's references.
constexpr int kConstKonst = 0;
constexpr int kConstReg = 4;
constexpr int kConstMat0 = 8;
constexpr int kConstAlphaRef = 12;

// Programs by the hash of their source, and by the hash of the GX state they were made from.
std::unordered_map<u64, std::shared_ptr<vita_gl::Program>> sPrograms;
std::unordered_map<u64, std::shared_ptr<vita_gl::Program>> sProgramsByState;

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

char component(GXTevColorChan chan) noexcept { return "rgba"[chan & 3]; }

std::string swizzle(const TevSwap& swap) {
  return {component(swap.red), component(swap.green), component(swap.blue), component(swap.alpha)};
}

// A channel's raster colour (n 0 or 1): the material colour (the vertex colour or the register),
// times its lighting, which without normals is the ambient colour plus the face's shading.
std::string raster(int n) {
  const GXState& s = g_gxState;
  const ColorChannelConfig& colorCfg = s.colorChannelConfig[GX_COLOR0 + n];
  const ColorChannelConfig& alphaCfg = s.colorChannelConfig[GX_ALPHA0 + n];
  const std::string vertex = n == 0 ? "vRaw" : "float4(1.0)";
  const std::string mat = fmt::format("uConst[{}]", kConstMat0 + 2 * n);
  const std::string amb = fmt::format("uConst[{}]", kConstMat0 + 2 * n + 1);
  const auto part = [&](const ColorChannelConfig& cfg, const char* sw, const char* shade) {
    std::string m = fmt::format("{}.{}", cfg.matSrc == GX_SRC_VTX ? vertex : mat, sw);
    if (!cfg.lightingEnabled) {
      return m;
    }
    std::string a = fmt::format("{}.{}", cfg.ambSrc == GX_SRC_VTX ? vertex : amb, sw);
    return fmt::format("({} * saturate({} + {}))", m, a, shade);
  };
  return fmt::format("float4({}, {})", part(colorCfg, "rgb", "float3(vShade)"), part(alphaCfg, "a", "vShade"));
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

const char* coord_type(const TcgConfig& tcg) noexcept { return tcg.type == GX_TG_MTX3x4 ? "float3" : "float2"; }

// The fragment program's body: the stages, then the alpha compare.
std::string fragment_body(const TevLayout& layout) {
  const GXState& s = g_gxState;
  std::string body = "  float4 R0 = uConst[4];\n  float4 R1 = uConst[5];\n  float4 R2 = uConst[6];\n  float4 R3 = uConst[7];\n";
  const u32 stages = std::min<u32>(s.numTevStages, MaxTevStages);
  u32 lastOut = 0;
  for (u32 i = 0; i < stages; ++i) {
    const TevStage& stage = s.tevStages[i];
    body += fmt::format("  {{ // stage {}\n", i);
    if (layout.stageMap[i] >= 0) {
      const int coord = layout.stageCoord[i];
      const bool projective = s.tcgs[layout.coords[coord]].type == GX_TG_MTX3x4;
      body += fmt::format("    float4 tex = {}(uTex{}, vUv{}).{};\n", projective ? "tex2Dproj" : "tex2D",
                          layout.stageMap[i], coord, swizzle(s.tevSwapTable[stage.tevSwapTex]));
    } else if (reads_texture(stage)) {
      body += "    float4 tex = float4(0.0);\n";
    }
    if (reads_raster(stage)) {
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
  body += fmt::format("  return R{};\n", lastOut);
  return body;
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
  if (s.numIndStages != 0) {
    return layout;
  }
  const u32 stages = std::min<u32>(s.numTevStages, MaxTevStages);
  for (u32 i = 0; i < stages; ++i) {
    const TevStage& stage = s.tevStages[i];
    if (stage.indTexMtxId != GX_ITM_OFF || stage.indTexWrapS != GX_ITW_OFF || stage.indTexWrapT != GX_ITW_OFF ||
        stage.indTexAddPrev) {
      return layout;
    }
    if (!reads_texture(stage) || stage.texMapId >= MaxTextures || stage.texCoordId >= MaxTexCoord) {
      continue;
    }
    const TcgConfig& tcg = s.tcgs[stage.texCoordId];
    if ((tcg.type != GX_TG_MTX2x4 && tcg.type != GX_TG_MTX3x4) || tcg.src < GX_TG_TEX0 || tcg.src > GX_TG_TEX7 ||
        s.vtxDesc[GX_VA_TEX0 + (tcg.src - GX_TG_TEX0)] == GX_NONE) {
      return layout; // positions, normals, colours or another generator as the source: not yet
    }
    int coord = -1;
    for (int c = 0; c < layout.coordCount; ++c) {
      coord = layout.coords[c] == stage.texCoordId ? c : coord;
    }
    if (coord < 0) {
      if (layout.coordCount == kTexCoords) {
        return layout;
      }
      coord = layout.coordCount++;
      layout.coords[coord] = stage.texCoordId;
    }
    int map = -1;
    for (int m = 0; m < layout.samplerCount; ++m) {
      map = layout.maps[m] == stage.texMapId ? m : map;
    }
    if (map < 0) {
      if (layout.samplerCount == kSamplers) {
        return layout;
      }
      map = layout.samplerCount++;
      layout.maps[map] = stage.texMapId;
    }
    layout.stageCoord[i] = static_cast<s8>(coord);
    layout.stageMap[i] = static_cast<s8>(map);
  }
  layout.supported = true;
  return layout;
}

std::shared_ptr<vita_gl::Program> tev_program(const TevLayout& layout) noexcept {
  const GXState& s = g_gxState;
  // What the source is made from, hashed: generating it is the costly part, and most materials
  // share a program with another.
  const u32 stages = std::min<u32>(s.numTevStages, MaxTevStages);
  const AlphaCompare& compare = s.alphaCompare;
  u32 fixed[] = {stages, static_cast<u32>(compare.comp0), static_cast<u32>(compare.comp1),
                 static_cast<u32>(compare.op), 0, 0, 0, 0};
  for (int c = 0; c < layout.coordCount; ++c) {
    fixed[4 + c] = static_cast<u32>(s.tcgs[layout.coords[c]].type) + 1;
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
  if (const auto it = sProgramsByState.find(stateKey); it != sProgramsByState.end()) {
    return it->second;
  }
  const std::string body = fragment_body(layout);
  const bool raw = body.find("vRaw") != std::string::npos;
  const bool shade = body.find("vShade") != std::string::npos;

  std::string fragmentParams;
  std::string vertexParams = "float3 aPos";
  std::string vertexOuts = "float4 out vPos : POSITION";
  std::string vertexBody = "  float4 p = float4(aPos, 1.0);\n"
                           "  float4 v = float4(dot(uMv[0], p), dot(uMv[1], p), dot(uMv[2], p), 1.0);\n"
                           "  vPos = float4(dot(uProj[0], v), dot(uProj[1], v), dot(uProj[2], v), dot(uProj[3], v));\n";
  const auto add = [](std::string& list, const std::string& item) {
    list += list.empty() ? item : ", " + item;
  };
  if (raw) {
    add(vertexParams, "float4 aRaw");
    add(vertexOuts, "float4 out vRaw : COLOR");
    vertexBody += "  vRaw = aRaw;\n";
    add(fragmentParams, "float4 vRaw : COLOR");
  }
  if (shade) {
    add(vertexParams, "float aShade");
    add(vertexOuts, "float out vShade : TEXCOORD4");
    vertexBody += "  vShade = aShade;\n";
    add(fragmentParams, "float vShade : TEXCOORD4");
  }
  for (int c = 0; c < layout.coordCount; ++c) {
    const TcgConfig& tcg = s.tcgs[layout.coords[c]];
    const char* type = coord_type(tcg);
    add(vertexParams, fmt::format("float2 aUv{}", c));
    add(vertexOuts, fmt::format("{} out vUv{} : TEXCOORD{}", type, c, c));
    vertexBody += fmt::format("  float4 t{0} = float4(aUv{0}, 1.0, 1.0);\n", c);
    if (tcg.type == GX_TG_MTX3x4) {
      vertexBody += fmt::format("  vUv{0} = float3(dot(uTexMtx[{1}], t{0}), dot(uTexMtx[{2}], t{0}), dot(uTexMtx[{3}], t{0}));\n",
                                c, 3 * c, 3 * c + 1, 3 * c + 2);
    } else {
      vertexBody += fmt::format("  vUv{0} = float2(dot(uTexMtx[{1}], t{0}), dot(uTexMtx[{2}], t{0}));\n", c, 3 * c,
                                3 * c + 1);
    }
    add(fragmentParams, fmt::format("{} vUv{} : TEXCOORD{}", type, c, c));
  }
  for (int m = 0; m < layout.samplerCount; ++m) {
    add(fragmentParams, fmt::format("uniform sampler2D uTex{0} : TEXUNIT{0}", m));
  }
  add(fragmentParams, fmt::format("uniform float4 uConst[{}]", vita_gl::kProgramConstants));
  std::string vertexUniforms = "uniform float4 uMv[3], uniform float4 uProj[4]";
  if (layout.coordCount != 0) {
    vertexUniforms += fmt::format(", uniform float4 uTexMtx[{}]", 3 * kTexCoords);
  }
  std::string vertexSource =
      fmt::format("void main({}, {}, {}) {{\n{}}}\n", vertexParams, vertexUniforms, vertexOuts, vertexBody);
  std::string fragmentSource = fmt::format("float4 main({}) : COLOR {{\n{}}}\n", fragmentParams, body);

  // The key also names the program's binaries on the memory card (vita_gl.cpp); the seed changes
  // when what a source compiles to could (the compiler's options, the attribute layout).
  constexpr u64 kBinaryVersion = 1;
  u64 key = XXH3_64bits_withSeed(vertexSource.data(), vertexSource.size(), kBinaryVersion);
  key = XXH3_64bits_withSeed(fragmentSource.data(), fragmentSource.size(), key);
  auto& program = sPrograms[key];
  if (!program) {
    program = vita_gl::make_program(std::move(vertexSource), std::move(fragmentSource), key);
  }
  sProgramsByState.emplace(stateKey, program);
  return program;
}

void tev_constants(float out[vita_gl::kProgramConstants][4]) noexcept {
  const GXState& s = g_gxState;
  for (int i = 0; i < 4; ++i) {
    for (int c = 0; c < 4; ++c) {
      out[kConstKonst + i][c] = s.kcolors[i][c];
      out[kConstReg + i][c] = s.colorRegs[i][c];
    }
  }
  for (int n = 0; n < 2; ++n) {
    const ColorChannelState& color = s.colorChannelState[GX_COLOR0 + n];
    const ColorChannelState& alpha = s.colorChannelState[GX_ALPHA0 + n];
    for (int c = 0; c < 3; ++c) {
      out[kConstMat0 + 2 * n][c] = color.matColor[c];
      out[kConstMat0 + 2 * n + 1][c] = color.ambColor[c];
    }
    out[kConstMat0 + 2 * n][3] = alpha.matColor[3];
    out[kConstMat0 + 2 * n + 1][3] = alpha.ambColor[3];
  }
  out[kConstAlphaRef][0] = static_cast<float>(s.alphaCompare.ref0 & 0xFF);
  out[kConstAlphaRef][1] = static_cast<float>(s.alphaCompare.ref1 & 0xFF);
  out[kConstAlphaRef][2] = 0.f;
  out[kConstAlphaRef][3] = 0.f;
}

void tev_texgen_rows(const TcgConfig& tcg, float out[12]) noexcept {
  static constexpr float kIdentity[12] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f};
  const float* rows = kIdentity;
  if (tcg.mtx != GX_IDENTITY) {
    // GX numbers the position matrices first, then the texture matrices, three rows apiece.
    const u32 index = static_cast<u32>(tcg.mtx) / 3;
    if (index < MaxPnMtx) {
      rows = reinterpret_cast<const float*>(&g_gxState.pnMtx[index].pos);
    } else if (index - MaxPnMtx < MaxTexMtx) {
      rows = reinterpret_cast<const float*>(&g_gxState.texMtxs[index - MaxPnMtx]);
    }
  }
  std::memcpy(out, rows, sizeof(float) * (tcg.type == GX_TG_MTX3x4 ? 12 : 8));
  if (tcg.type != GX_TG_MTX3x4) {
    std::memcpy(out + 8, kIdentity + 8, sizeof(float) * 4);
  }
}

size_t tev_program_count() noexcept { return sPrograms.size(); }
} // namespace aurora::gx::vita
