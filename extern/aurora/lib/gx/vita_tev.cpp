#include "vita_tev.hpp"

#include "../internal.hpp"
#include "shader_info.hpp"

#include <fmt/format.h>
#include <xxhash.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

namespace aurora::gx::vita {
namespace {
using vita_gl::kSamplers;
using vita_gl::kTexCoords;

// The constants' slots (vita_gl::kProgramConstants): konst colours, TEV registers, the channels'
// material and ambient colours, the alpha compare's references. Sources name them so,
// uConst[slot], until pack_constants renumbers them to the ones the source reads.
constexpr int kConstKonst = 0;
constexpr int kConstReg = 4;
constexpr int kConstMat0 = 8;
constexpr int kConstAlphaRef = 12;

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

// A generator whose coordinates are projective (sampled with tex2Dproj): a 3x4 matrix's, or after a
// post-transform matrix, whose third row makes the divisor.
bool projective(const TcgConfig& tcg) noexcept { return tcg.type == GX_TG_MTX3x4 || has_post(tcg); }

const char* coord_type(const TcgConfig& tcg) noexcept { return projective(tcg) ? "float3" : "float2"; }

// The fragment program's body: the stages, then the alpha compare.
std::string fragment_body(const TevLayout& layout) {
  const GXState& s = g_gxState;
  std::string body;
  const u32 stages = std::min<u32>(s.numTevStages, MaxTevStages);
  u32 lastOut = 0;
  // A register starts at its value from GX only where something reads it (its constant is then
  // declared); one only written starts at zero.
  bool registerRead[4] = {};
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
    if (layout.stageMap[i] >= 0) {
      const int coord = layout.stageCoord[i];
      const bool proj = projective(s.tcgs[layout.coords[coord]]);
      body += fmt::format("    float4 tex = {}(uTex{}, vUv{}).{};\n", proj ? "tex2Dproj" : "tex2D",
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

// A coordinate generator's source as the vertex program has it, (x, y, z, 1) as GX takes it.
std::string texgen_input(const TcgConfig& tcg, int c) {
  if (tcg.src == GX_TG_POS) {
    return "float4(aPos, 1.0)";
  }
  if (tcg.src == GX_TG_NRM) {
    return "float4(aNrm, 1.0)";
  }
  return fmt::format("float4(aUv{}, 1.0, 1.0)", c);
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
    const bool fromTex = tcg.src >= GX_TG_TEX0 && tcg.src <= GX_TG_TEX7;
    if ((tcg.type != GX_TG_MTX2x4 && tcg.type != GX_TG_MTX3x4) ||
        (!fromTex && tcg.src != GX_TG_POS && tcg.src != GX_TG_NRM) ||
        (fromTex && s.vtxDesc[GX_VA_TEX0 + (tcg.src - GX_TG_TEX0)] == GX_NONE) ||
        (has_post(tcg) && (tcg.postMtx - GX_PTTEXMTX0) / 3 >= MaxPTTexMtx)) {
      return layout; // colours, binormals, another generator or a bump map as the source: not yet
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

const TevProgram* tev_program(const TevLayout& layout) noexcept {
  const GXState& s = g_gxState;
  // What the source is made from, hashed: generating it is the costly part, and most materials
  // share a program with another.
  const u32 stages = std::min<u32>(s.numTevStages, MaxTevStages);
  const AlphaCompare& compare = s.alphaCompare;
  // A coordinate set by its generator's type, source (a texture coordinate, the position or the
  // normal), normalisation and whether it has a post-transform matrix; the lights each channel
  // part sums.
  u32 fixed[] = {stages, static_cast<u32>(compare.comp0), static_cast<u32>(compare.comp1),
                 static_cast<u32>(compare.op), 0, 0, 0, 0, 0, 0, 0, 0};
  for (int c = 0; c < layout.coordCount; ++c) {
    const TcgConfig& tcg = s.tcgs[layout.coords[c]];
    const u32 source = tcg.src == GX_TG_POS ? 1 : tcg.src == GX_TG_NRM ? 2 : 0;
    fixed[4 + c] = (static_cast<u32>(tcg.type) + 1) | source << 8 | (tcg.normalize ? 1u : 0u) << 12 |
                   (has_post(tcg) ? 1u : 0u) << 13;
  }
  for (int part = 0; part < 4; ++part) {
    fixed[8 + part] = static_cast<u32>(light_count(part));
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
  TevProgram info;
  const std::string body = pack_constants(fragment_body(layout), info);
  const bool raw = body.find("vRaw") != std::string::npos;
  // The channels whose lights the raster colours take (raster), and so the lights summed.
  bool litChannel[2] = {body.find("vLit0") != std::string::npos, body.find("vLit1") != std::string::npos};
  bool lit = false;
  for (int n = 0; n < 2; ++n) {
    for (const int part : {GX_COLOR0 + n, GX_ALPHA0 + n}) {
      info.lightCounts[part] = static_cast<u8>(litChannel[n] ? light_count(part) : 0);
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
  for (int c = 0; c < layout.coordCount; ++c) {
    // As GX generates a coordinate: the source times the matrix (z 1 after a 2x4 one), normalised
    // when asked, then the post-transform matrix (Aurora's shader.cpp does the same).
    const TcgConfig& tcg = s.tcgs[layout.coords[c]];
    const char* type = coord_type(tcg);
    if (tcg.src >= GX_TG_TEX0 && tcg.src <= GX_TG_TEX7) {
      add(vertexParams, fmt::format("float2 aUv{}", c));
    }
    add(vertexOuts, fmt::format("{} out vUv{} : TEXCOORD{}", type, c, c));
    vertexBody += fmt::format("  float4 t{} = {};\n", c, texgen_input(tcg, c));
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
      add(vertexOuts, fmt::format("float4 out vLit{0} : TEXCOORD{1}", n, 5 + n));
      add(fragmentParams, fmt::format("float4 vLit{0} : TEXCOORD{1}", n, 5 + n));
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
    info.program = vita_gl::make_program(std::move(vertexSource), std::move(fragmentSource), key);
    program = std::move(info);
  }
  sProgramsByState.emplace(stateKey, &program);
  return &program;
}

void tev_constants(const TevProgram& program, float out[vita_gl::kProgramConstants][4]) noexcept {
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
    } else {
      value[0] = static_cast<float>(s.alphaCompare.ref0 & 0xFF);
      value[1] = static_cast<float>(s.alphaCompare.ref1 & 0xFF);
      value[2] = 0.f;
      value[3] = 0.f;
    }
  }
}

int tev_vertex_rows(const TevProgram& program, const TcgConfig* const coords[], int coordCount,
                    float (*out)[4]) noexcept {
  static constexpr float kIdentity[12] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f};
  const GXState& s = g_gxState;
  int row = 4;
  for (int c = 0; c < coordCount; ++c) {
    const TcgConfig& tcg = *coords[c];
    const float* rows = kIdentity;
    if (tcg.mtx != GX_IDENTITY) {
      // GX numbers the position matrices first, then the texture matrices, three rows apiece.
      const u32 index = static_cast<u32>(tcg.mtx) / 3;
      if (index < MaxPnMtx) {
        rows = reinterpret_cast<const float*>(&s.pnMtx[index].pos);
      } else if (index - MaxPnMtx < MaxTexMtx) {
        rows = reinterpret_cast<const float*>(&s.texMtxs[index - MaxPnMtx]);
      }
    }
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
      for (; written < program.lightCounts[part]; ++written) {
        std::memset(out[row], 0, sizeof(float[20]));
        row += 5;
      }
    }
  }
  return row;
}

size_t tev_program_count() noexcept { return sPrograms.size(); }
} // namespace aurora::gx::vita
