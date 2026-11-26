// The Echoes suits conversion. See port_echoes_suits.h.

#include "port_echoes_suits.h"

#include "port_dread_samus_mod.h"
#include "port_federation_marine.h"
#include "port_strings.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>

namespace PortEchoesSuits {
namespace {

constexpr uint32_t kCMDL = 0x434D444C;
constexpr uint32_t kCSKR = 0x43534B52;
constexpr uint32_t kCINF = 0x43494E46;
constexpr uint32_t kTXTR = 0x54585452;
constexpr uint32_t kANCS = 0x414E4353;
constexpr uint32_t kANIM = 0x414E494D;

using port::Hex8;

bool Fail(std::string* error, const std::string& why) {
  if (error != nullptr) {
    *error = why;
  }
  return false;
}

// Bounds-checked big-endian reads over one buffer; a read past the end sets `bad` and yields 0.
struct Reader {
  const uint8_t* data;
  size_t size;
  bool bad = false;

  bool Has(size_t at, size_t n) {
    if (at > size || n > size - at) {
      bad = true;
      return false;
    }
    return true;
  }
  uint32_t U32(size_t at) {
    if (!Has(at, 4)) return 0;
    return (uint32_t(data[at]) << 24) | (uint32_t(data[at + 1]) << 16) | (uint32_t(data[at + 2]) << 8) |
           uint32_t(data[at + 3]);
  }
  uint16_t U16(size_t at) {
    if (!Has(at, 2)) return 0;
    return uint16_t((data[at] << 8) | data[at + 1]);
  }
  float F32(size_t at) {
    const uint32_t bits = U32(at);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
  }
};

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(uint8_t(value >> 24));
  out.push_back(uint8_t(value >> 16));
  out.push_back(uint8_t(value >> 8));
  out.push_back(uint8_t(value));
}

void Set32(std::vector<uint8_t>& out, size_t at, uint32_t value) {
  out[at] = uint8_t(value >> 24);
  out[at + 1] = uint8_t(value >> 16);
  out[at + 2] = uint8_t(value >> 8);
  out[at + 3] = uint8_t(value);
}

void Append(std::vector<uint8_t>& out, const uint8_t* data, size_t size) { out.insert(out.end(), data, data + size); }

void Pad32(std::vector<uint8_t>& out) { out.resize((out.size() + 31) & ~size_t(31), 0); }

// A run length past its instruction's bit field: each zero byte adds 255, the first non-zero ends it.
bool ReadExtendedLength(const uint8_t*& ip, const uint8_t* ipEnd, size_t& len) {
  while (ip < ipEnd && *ip == 0) {
    len += 255;
    ++ip;
  }
  if (ip >= ipEnd) {
    return false;
  }
  len += *ip++;
  return true;
}

// The UV animation modes' parameter counts (CMaterial UV animation types 0-8).
constexpr uint32_t kUvParams[] = {0, 0, 4, 2, 4, 4, 0, 2, 9};

// One Echoes material at `at` of the material set `r`, appended to `out` in Prime's layout.
bool ConvertMaterial(Reader& r, size_t& at, std::vector<uint8_t>& out, uint32_t& vertexFlags, std::string* error) {
  const uint32_t flags = r.U32(at);
  const uint32_t ntex = r.U32(at + 4);
  if (r.bad || !r.Has(at + 8, size_t(ntex) * 4)) {
    return Fail(error, "material cut short");
  }
  // Bit 14 is the port's PBR flag in Prime; Echoes sets it for its own reasons.
  Put32(out, flags & ~0x4000u);
  Append(out, r.data + at + 4, 4 + size_t(ntex) * 4);
  at += 8 + size_t(ntex) * 4;
  // Bits 24-31: matrix indices before each vertex, which ConvertSurface strips.
  vertexFlags = r.U32(at);
  Put32(out, vertexFlags & 0x00FFFFFF);
  // at + 4, at + 8: two words Prime does not have.
  Put32(out, r.U32(at + 12));  // group
  at += 16;
  const size_t start = at;
  if (flags & 0x8) {
    at += 4 + size_t(r.U32(at)) * 4;  // konst colours
  }
  at += 4;  // blend factors
  if (flags & 0x400) {
    at += 4;
  }
  at += 4 + size_t(r.U32(at)) * 4;  // colour channels
  const size_t stages = r.U32(at);
  at += 4 + stages * 20 + stages * 4;  // TEV stages, their inputs
  at += 4 + size_t(r.U32(at)) * 4;     // texgens
  const size_t animSize = r.U32(at);
  const size_t animStart = at + 4;
  const uint32_t anims = r.U32(animStart);
  size_t p = animStart + 4;
  for (uint32_t i = 0; i < anims && !r.bad; ++i) {
    const uint32_t mode = r.U32(p);
    if (mode >= std::size(kUvParams)) {
      return Fail(error, "unknown UV animation mode " + std::to_string(mode));
    }
    p += 4 + kUvParams[mode] * 4;
  }
  if (r.bad || p != animStart + animSize || !r.Has(start, p - start)) {
    return Fail(error, "material layout not understood");
  }
  Append(out, r.data + start, p - start);
  at = p;
  return true;
}

bool ConvertMaterialSet(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::vector<uint32_t>& flags,
                        std::vector<uint32_t>& textures, std::string* error) {
  Reader r{data, size};
  const uint32_t ntex = r.U32(0);
  if (r.bad || !r.Has(4, size_t(ntex) * 4)) {
    return Fail(error, "material set cut short");
  }
  for (uint32_t i = 0; i < ntex; ++i) {
    textures.push_back(r.U32(4 + i * 4));
  }
  size_t at = 4 + size_t(ntex) * 4;
  const uint32_t nmat = r.U32(at);
  if (r.bad || nmat > size / 4) {
    return Fail(error, "material set cut short");
  }
  at += 4 + size_t(nmat) * 4;
  std::vector<std::vector<uint8_t>> materials(nmat);
  flags.assign(nmat, 0);
  for (uint32_t i = 0; i < nmat; ++i) {
    if (!ConvertMaterial(r, at, materials[i], flags[i], error)) {
      return false;
    }
  }
  out.clear();
  Append(out, data, 4 + size_t(ntex) * 4);
  Put32(out, nmat);
  uint32_t end = 0;
  for (const auto& material : materials) {
    end += uint32_t(material.size());
    Put32(out, end);
  }
  for (const auto& material : materials) {
    Append(out, material.data(), material.size());
  }
  Pad32(out);
  return true;
}

bool ConvertSurface(const uint8_t* data, size_t size, const std::vector<uint32_t>& vertexFlags,
                    std::vector<uint8_t>& out, std::string* error) {
  Reader r{data, size};
  const uint32_t material = r.U32(0x0C);
  const uint32_t extra = r.U32(0x1C);
  if (r.bad || !r.Has(0x30, extra)) {
    return Fail(error, "surface header cut short");
  }
  if (material >= vertexFlags.size()) {
    return Fail(error, "surface material " + std::to_string(material) + " out of range");
  }
  const uint32_t vaf = vertexFlags[material];
  const size_t matrices = size_t(std::popcount(vaf >> 24));
  size_t vertexSize = matrices;
  for (int i = 0; i < 11; ++i) {
    switch ((vaf >> (2 * i)) & 3) {
    case 0:
      break;
    case 1:
      return Fail(error, "direct vertex attribute");
    case 2:
      vertexSize += 1;
      break;
    case 3:
      vertexSize += 2;
      break;
    }
  }
  std::vector<uint8_t> list;
  size_t at = (0x30 + size_t(extra) + 31) & ~size_t(31);
  while (at < size && data[at] != 0) {
    const uint16_t count = r.U16(at + 1);
    if (r.bad || !r.Has(at + 3, size_t(count) * vertexSize)) {
      return Fail(error, "display list cut short");
    }
    Append(list, data + at, 3);
    at += 3;
    for (uint16_t v = 0; v < count; ++v) {
      Append(list, data + at + matrices, vertexSize - matrices);
      at += vertexSize;
    }
  }
  list.push_back(0);
  Pad32(list);
  out.clear();
  Append(out, data, 0x2C);  // Echoes' word at 0x2C is not Prime's
  Append(out, data + 0x30, extra);
  Set32(out, 0x10, (r.U32(0x10) & 0xFFFF0000) | uint32_t(list.size()));
  Pad32(out);
  Append(out, list.data(), list.size());
  return true;
}

}  // namespace

bool ParseSet(const std::vector<uint8_t>& in, MaterialSet& out, std::string* error) {
  Reader r{in.data(), in.size()};
  const uint32_t ntex = r.U32(0);
  if (r.bad || ntex > in.size() / 4) {
    return Fail(error, "material set cut short");
  }
  out = MaterialSet{};
  for (uint32_t i = 0; i < ntex; ++i) {
    out.textures.push_back(r.U32(4 + i * 4));
  }
  const size_t count = 4 + size_t(ntex) * 4;
  const uint32_t nmat = r.U32(count);
  if (r.bad || nmat > in.size() / 4) {
    return Fail(error, "material set cut short");
  }
  size_t at = count + 4 + size_t(nmat) * 4;
  uint32_t begin = 0;
  for (uint32_t i = 0; i < nmat; ++i) {
    const uint32_t end = r.U32(count + 4 + i * 4);
    if (r.bad || end < begin || !r.Has(at + begin, end - begin)) {
      return Fail(error, "material " + std::to_string(i) + " cut short");
    }
    out.materials.emplace_back(in.begin() + std::ptrdiff_t(at + begin), in.begin() + std::ptrdiff_t(at + end));
    Reader m{out.materials.back().data(), out.materials.back().size()};
    m.U32(8 + size_t(m.U32(4)) * 4);
    if (m.bad) {
      return Fail(error, "material " + std::to_string(i) + " has no vertex flags");
    }
    begin = end;
  }
  return true;
}

std::vector<uint8_t> WriteSet(const MaterialSet& set) {
  std::vector<uint8_t> out;
  Put32(out, uint32_t(set.textures.size()));
  for (const uint32_t id : set.textures) {
    Put32(out, id);
  }
  Put32(out, uint32_t(set.materials.size()));
  uint32_t end = 0;
  for (const auto& material : set.materials) {
    end += uint32_t(material.size());
    Put32(out, end);
  }
  for (const auto& material : set.materials) {
    Append(out, material.data(), material.size());
  }
  Pad32(out);
  return out;
}

// The pause screen's doll draws its body three times (CModelData::MultiLightingDraw): depth alone,
// then lit and then its pulse, both testing against that depth. Prime's doll bodies have no glass:
// the visor is a model of its own (CSamusDoll's InvVisor), drawn after with the face showing through.
// Echoes' have the visor in the body, as Prime's cutscene bodies do: an alpha-blended glass and an
// added glow over the face, depth-sorted and writing depth. The depth pass wrote the glass's depth,
// so the face behind it never drew, and the glass showed solid against the dark, its texture's
// outlines with it (0.46.0 test 38, the user: "her face isn't visible when you view the suit in the
// pause menu and there are visible lines on the the visor"). A doll body's depth-sorted materials
// write no depth: the face draws, and the glass over it.
bool SortedWithoutDepth(PrimeCmdl& model, std::string* error) {
  constexpr uint32_t kDepthSorting = 0x10;
  constexpr uint32_t kDepthWrite = 0x80;
  for (std::vector<uint8_t>& bytes : model.materialSets) {
    MaterialSet set;
    if (!ParseSet(bytes, set, error)) {
      return false;
    }
    for (std::vector<uint8_t>& material : set.materials) {
      Reader r{material.data(), material.size()};
      const uint32_t flags = r.U32(0);
      if (r.bad) {
        return Fail(error, "material cut short");
      }
      if ((flags & kDepthSorting) != 0) {
        Set32(material, 0, flags & ~kDepthWrite);
      }
    }
    bytes = WriteSet(set);
  }
  return true;
}

namespace {

size_t VertexFlagsAt(const std::vector<uint8_t>& material) {
  Reader m{material.data(), material.size()};
  return 8 + size_t(m.U32(4)) * 4;
}

uint32_t VertexFlags(const std::vector<uint8_t>& material) {
  Reader m{material.data(), material.size()};
  return m.U32(VertexFlagsAt(material));
}

// The vertex flags of `surface`'s material in `set`.
bool SurfaceFlags(const std::vector<uint8_t>& surface, const MaterialSet& set, uint32_t& vaf, std::string* error) {
  Reader r{surface.data(), surface.size()};
  const uint32_t material = r.U32(0x0C);
  if (r.bad || material >= set.materials.size()) {
    return Fail(error, "surface material out of range");
  }
  vaf = VertexFlags(set.materials[material]);
  return true;
}

// `surface`'s display list (vertex flags `vaf`) rebuilt: each primitive's format through `format`,
// each index through `index` (attribute 0-10, its value), written 16-bit when `wide`, else at its
// own width.
bool RewriteList(std::vector<uint8_t>& surface, uint32_t vaf, bool wide,
                 const std::function<bool(uint8_t&)>& format,
                 const std::function<bool(int, uint32_t&)>& index, std::string* error) {
  Reader r{surface.data(), surface.size()};
  const uint32_t sizeWord = r.U32(0x10);
  const uint32_t extra = r.U32(0x1C);
  const size_t start = (0x2C + size_t(extra) + 31) & ~size_t(31);
  if (r.bad || start > surface.size()) {
    return Fail(error, "surface header cut short");
  }
  size_t vertexSize = 0;
  for (int i = 0; i < 11; ++i) {
    const uint32_t mode = (vaf >> (2 * i)) & 3;
    if (mode == 1) {
      return Fail(error, "direct vertex attribute");
    }
    vertexSize += mode == 0 ? 0 : mode - 1;
  }
  std::vector<uint8_t> list;
  size_t at = start;
  while (at < surface.size() && surface[at] != 0) {
    uint8_t command = surface[at];
    const uint16_t count = r.U16(at + 1);
    if (r.bad || !r.Has(at + 3, size_t(count) * vertexSize)) {
      return Fail(error, "display list cut short");
    }
    if (!format(command)) {
      return Fail(error, "vertex format " + std::to_string(command & 7) + " not supported here");
    }
    list.push_back(command);
    Append(list, surface.data() + at + 1, 2);
    at += 3;
    for (uint16_t v = 0; v < count; ++v) {
      for (int i = 0; i < 11; ++i) {
        const uint32_t mode = (vaf >> (2 * i)) & 3;
        if (mode == 0) continue;
        uint32_t value = mode == 2 ? surface[at] : r.U16(at);
        at += mode - 1;
        if (!index(i, value)) {
          return Fail(error, "vertex index out of range");
        }
        if (wide || mode == 3) {
          if (value > 0xFFFF) return Fail(error, "vertex index past 16 bits");
          list.push_back(uint8_t(value >> 8));
        } else if (value > 0xFF) {
          return Fail(error, "vertex index past 8 bits");
        }
        list.push_back(uint8_t(value));
      }
    }
  }
  list.push_back(0);
  Pad32(list);
  surface.resize(start);
  Append(surface, list.data(), list.size());
  Set32(surface, 0x10, (sizeWord & 0xFFFF0000) | uint32_t(list.size()));
  return true;
}

// Each list of `model` checked to read texture 0 as floats (formats 0 and 1), then its short UVs
// dropped: CCubeModel binds them only for packed lightmaps, which those formats cannot read.
bool DropShortUvs(PrimeCmdl& model, const MaterialSet& set, std::string* error) {
  for (auto& surface : model.surfaces) {
    uint32_t vaf;
    if (!SurfaceFlags(surface, set, vaf, error) ||
        !RewriteList(
            surface, vaf, false, [](uint8_t& c) { return (c & 7) < 2; }, [](int, uint32_t&) { return true; },
            error)) {
      return false;
    }
  }
  Reader r{model.header.data(), model.header.size()};
  const uint32_t flags = r.U32(8);
  if (flags & 0x4) {
    if (model.arrays.size() != 5) return Fail(error, "short UVs flagged without their array");
    model.arrays.pop_back();
    Set32(model.header, 8, flags & ~0x4u);
  }
  return true;
}

}  // namespace

bool LzoInflate(const uint8_t* source, size_t sourceLen, uint8_t* dest, size_t& destLen) {
  // LZO1X, written for the Metroid Prime 2 port from the stream format (MIT, CLZOSupportPort.cpp
  // there); bounds-checked, unlike retail's lzo1x_decompress.
  const uint8_t* ip = source;
  const uint8_t* const ipEnd = source + sourceLen;
  uint8_t* op = dest;
  uint8_t* const opEnd = dest + destLen;
  // Literals the last instruction copied: 0-3 from a match's low bits, 4 after a literal run.
  // It decides what an instruction below 16 means.
  size_t state = 0;
  bool ok = false;

#define LZO_NEED_IN(n) \
  if (size_t(ipEnd - ip) < size_t(n)) goto done
#define LZO_NEED_OUT(n) \
  if (size_t(opEnd - op) < size_t(n)) goto done

  if (ip < ipEnd && *ip > 17) {
    const size_t len = *ip++ - 17;
    LZO_NEED_IN(len);
    LZO_NEED_OUT(len);
    std::memcpy(op, ip, len);
    ip += len;
    op += len;
    state = len < 4 ? len : 4;
  }

  while (ip < ipEnd) {
    const size_t inst = *ip++;
    size_t len;
    size_t dist;
    size_t next;
    if (inst >= 64) {
      // M2: 3-8 bytes, up to 2 KiB back.
      LZO_NEED_IN(1);
      len = (inst >> 5) + 1;
      dist = ((inst >> 2) & 7) + (size_t(*ip++) << 3) + 1;
      next = inst & 3;
    } else if (inst >= 32) {
      // M3: up to 16 KiB back.
      len = inst & 31;
      if (len == 0) {
        len = 31;
        if (!ReadExtendedLength(ip, ipEnd, len)) goto done;
      }
      len += 2;
      LZO_NEED_IN(2);
      const size_t d = ip[0] | (size_t(ip[1]) << 8);
      ip += 2;
      dist = (d >> 2) + 1;
      next = d & 3;
    } else if (inst >= 16) {
      // M4: 16-48 KiB back; distance 0 ends the stream.
      len = inst & 7;
      if (len == 0) {
        len = 7;
        if (!ReadExtendedLength(ip, ipEnd, len)) goto done;
      }
      len += 2;
      LZO_NEED_IN(2);
      const size_t d = ip[0] | (size_t(ip[1]) << 8);
      ip += 2;
      dist = ((inst & 8) << 11) + (d >> 2);
      if (dist == 0) {
        break;
      }
      dist += 0x4000;
      next = d & 3;
    } else if (state == 0) {
      // Literal run of 4 or more bytes.
      len = inst;
      if (len == 0) {
        len = 15;
        if (!ReadExtendedLength(ip, ipEnd, len)) goto done;
      }
      len += 3;
      LZO_NEED_IN(len);
      LZO_NEED_OUT(len);
      std::memcpy(op, ip, len);
      ip += len;
      op += len;
      state = 4;
      continue;
    } else {
      // M1: a 2-byte match right after short literals, or 3 bytes 2-3 KiB back after a long run.
      LZO_NEED_IN(1);
      len = state == 4 ? 3 : 2;
      dist = (inst >> 2) + (size_t(*ip++) << 2) + (state == 4 ? 2049 : 1);
      next = inst & 3;
    }

    if (dist > size_t(op - dest)) goto done;
    LZO_NEED_OUT(len);
    {
      // Byte by byte: a match may overlap the bytes it is producing.
      const uint8_t* mp = op - dist;
      for (size_t i = 0; i < len; ++i) {
        op[i] = mp[i];
      }
      op += len;
    }

    LZO_NEED_IN(next);
    LZO_NEED_OUT(next);
    for (size_t i = 0; i < next; ++i) {
      *op++ = *ip++;
    }
    state = next;
  }
  // The end marker, or the input used up exactly; a bad instruction jumps past this.
  ok = true;

#undef LZO_NEED_IN
#undef LZO_NEED_OUT

done:
  destLen = size_t(op - dest);
  return ok;
}

bool Decompress(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string* error) {
  Reader r{data, size};
  const uint32_t total = r.U32(0);
  if (r.bad || total > (256u << 20)) {
    return Fail(error, "bad compressed size");
  }
  out.assign(total, 0);
  size_t at = 4;
  size_t written = 0;
  while (written < total) {
    const int16_t length = int16_t(r.U16(at));
    at += 2;
    if (r.bad) {
      return Fail(error, "compressed data cut short");
    }
    if (length == 0) {
      return Fail(error, "empty segment");
    }
    if (length < 0) {
      const size_t n = size_t(-int32_t(length));
      if (!r.Has(at, n) || n > total - written) {
        return Fail(error, "stored segment out of bounds");
      }
      std::memcpy(out.data() + written, data + at, n);
      at += n;
      written += n;
      continue;
    }
    if (!r.Has(at, size_t(length))) {
      return Fail(error, "compressed segment cut short");
    }
    const size_t expected = std::min<size_t>(0x4000, total - written);
    size_t got = expected;
    if (!LzoInflate(data + at, size_t(length), out.data() + written, got) || got != expected) {
      return Fail(error, "corrupt LZO segment");
    }
    at += size_t(length);
    written += got;
  }
  return true;
}

std::vector<uint8_t> WriteCmdl(const PrimeCmdl& model) {
  std::vector<std::vector<uint8_t>> sections = model.materialSets;
  sections.insert(sections.end(), model.arrays.begin(), model.arrays.end());
  std::vector<uint8_t> offsets;
  Put32(offsets, uint32_t(model.surfaces.size()));
  uint32_t end = 0;
  for (const auto& surface : model.surfaces) {
    end += uint32_t(surface.size());
    Put32(offsets, end);
  }
  Pad32(offsets);
  sections.push_back(std::move(offsets));
  sections.insert(sections.end(), model.surfaces.begin(), model.surfaces.end());

  std::vector<uint8_t> out = model.header;
  Set32(out, 0x24, uint32_t(sections.size()));
  Set32(out, 0x28, uint32_t(model.materialSets.size()));
  for (const auto& section : sections) {
    Put32(out, uint32_t(section.size()));
  }
  Pad32(out);
  for (const auto& section : sections) {
    Append(out, section.data(), section.size());
  }
  return out;
}

bool ConvertCmdl(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::vector<uint32_t>& textures,
                 std::string* error) {
  PrimeCmdl model;
  if (!ConvertCmdl(in, model, textures, error)) {
    return false;
  }
  out = WriteCmdl(model);
  return true;
}

bool ConvertCmdl(const std::vector<uint8_t>& in, PrimeCmdl& model, std::vector<uint32_t>& textures,
                 std::string* error) {
  Reader r{in.data(), in.size()};
  if (r.U32(0) != 0xDEADBABE || r.U32(4) != 4) {
    return Fail(error, "not an Echoes CMDL");
  }
  const uint32_t flags = r.U32(8);
  const uint32_t nsec = r.U32(0x24);
  const uint32_t nms = r.U32(0x28);
  if (r.bad || nsec > in.size() / 4) {
    return Fail(error, "CMDL header cut short");
  }
  std::vector<std::pair<size_t, size_t>> sections(nsec);
  size_t at = (0x2C + size_t(nsec) * 4 + 31) & ~size_t(31);
  for (uint32_t i = 0; i < nsec; ++i) {
    const size_t length = r.U32(0x2C + i * 4);
    if (!r.Has(at, length)) {
      return Fail(error, "CMDL section " + std::to_string(i) + " cut short");
    }
    sections[i] = {at, length};
    at += length;
  }
  const uint32_t arrays = (flags & 0x4) ? 5 : 4;
  const size_t surfaceHeader = size_t(nms) + arrays;
  if (surfaceHeader >= nsec) {
    return Fail(error, "CMDL has no surfaces");
  }
  const uint32_t nsurf = r.U32(sections[surfaceHeader].first);
  if (surfaceHeader + 1 + size_t(nsurf) != nsec) {
    return Fail(error, "CMDL section count does not match its surfaces");
  }

  model = PrimeCmdl{};
  std::vector<uint32_t> vertexFlags;
  for (uint32_t i = 0; i < nms; ++i) {
    std::vector<uint8_t> set;
    std::vector<uint32_t> setFlags;
    if (!ConvertMaterialSet(in.data() + sections[i].first, sections[i].second, set, setFlags, textures, error)) {
      return false;
    }
    if (i == 0) {
      vertexFlags = std::move(setFlags);
    }
    model.materialSets.push_back(std::move(set));
  }
  for (uint32_t i = 0; i < arrays; ++i) {
    const auto& [start, length] = sections[nms + i];
    model.arrays.emplace_back(in.begin() + std::ptrdiff_t(start), in.begin() + std::ptrdiff_t(start + length));
  }
  model.surfaces.resize(nsurf);
  for (uint32_t i = 0; i < nsurf; ++i) {
    const auto& [start, length] = sections[surfaceHeader + 1 + i];
    if (!ConvertSurface(in.data() + start, length, vertexFlags, model.surfaces[i], error)) {
      return false;
    }
  }
  model.header.assign(in.begin(), in.begin() + 0x2C);
  Set32(model.header, 4, 2);
  std::sort(textures.begin(), textures.end());
  textures.erase(std::unique(textures.begin(), textures.end()), textures.end());
  return true;
}

bool KeepMaterialSet(PrimeCmdl& model, size_t set, std::vector<uint32_t>& textures, std::string* error) {
  if (set >= model.materialSets.size()) {
    return Fail(error, "no material set " + std::to_string(set));
  }
  MaterialSet parsed;
  if (!ParseSet(model.materialSets[set], parsed, error)) {
    return false;
  }
  model.materialSets = {std::move(model.materialSets[set])};
  textures = std::move(parsed.textures);
  std::sort(textures.begin(), textures.end());
  textures.erase(std::unique(textures.begin(), textures.end()), textures.end());
  return true;
}

bool PatchGunAncs(std::vector<uint8_t>& ancs, std::string* error) {
  for (const Gun& gun : kGuns) {
    const uint32_t ids[3] = {gun.cmdl, gun.cskr, kGunCinf};
    uint8_t needle[12];
    for (int i = 0; i < 12; ++i) needle[i] = uint8_t(ids[i / 4] >> (24 - 8 * (i % 4)));
    size_t found = ancs.size();
    for (size_t at = 0; at + 12 <= ancs.size(); ++at) {
      if (std::memcmp(ancs.data() + at, needle, 12) != 0) continue;
      if (found != ancs.size()) {
        return Fail(error, std::string(gun.what) + " " + Hex8(gun.cmdl) + " is in it twice");
      }
      found = at;
    }
    if (found == ancs.size()) {
      return Fail(error, std::string(gun.what) + " " + Hex8(gun.cmdl) + " is not in it");
    }
    Set32(ancs, found, Cannon(gun.cmdl));
    Set32(ancs, found + 4, Cannon(gun.cskr));
  }
  return true;
}

namespace {

void Put16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(uint8_t(value >> 8));
  out.push_back(uint8_t(value));
}

void PutF32(std::vector<uint8_t>& out, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  Put32(out, bits);
}

void PutString(std::vector<uint8_t>& out, const char* text) {
  Append(out, reinterpret_cast<const uint8_t*>(text), std::strlen(text) + 1);
}

// An ANIM's key stream as CBitLevelLoader reads it: u32 words, big-endian, each from its low bit.
struct BitReader {
  Reader& r;
  size_t at;
  uint32_t word = 0;
  int used = 32;

  uint32_t Bits(int n) {
    uint32_t out = 0;
    int shift = 0;
    while (n > 0) {
      if (used == 32) {
        word = r.U32(at);
        at += 4;
        used = 0;
      }
      const int take = std::min(n, 32 - used);
      const uint32_t mask = take == 32 ? 0xFFFFFFFFu : (1u << take) - 1u;
      out |= ((word >> used) & mask) << shift;
      used += take;
      shift += take;
      n -= take;
    }
    return out;
  }
  int32_t Signed(int n) {
    if (n == 0) return 0;
    const uint32_t value = Bits(n);
    return (value >> (n - 1)) & 1u ? int32_t(value | ~((1u << n) - 1u)) : int32_t(value);
  }
};

struct BitWriter {
  std::vector<uint32_t> words;
  uint32_t word = 0;
  int used = 0;

  void Put(uint32_t value, int n) {
    while (n > 0) {
      const int take = std::min(n, 32 - used);
      const uint32_t mask = take == 32 ? 0xFFFFFFFFu : (1u << take) - 1u;
      word |= (value & mask) << used;
      value = take == 32 ? 0 : value >> take;
      used += take;
      n -= take;
      if (used == 32) {
        words.push_back(word);
        word = 0;
        used = 0;
      }
    }
  }
  void Finish() {
    if (used > 0) {
      words.push_back(word);
      word = 0;
      used = 0;
    }
  }
};

// One track (rotation, offset or scale) of a compressed ANIM channel: its key count (0: none), and
// per component the initial value and the bits of each key's step.
struct AnimTrack {
  uint16_t keys = 0;
  int16_t init[3] = {};
  uint8_t bits[3] = {};
};

}  // namespace

bool ConvertGunAnim(const std::vector<uint8_t>& in, const std::vector<EchoesJoint>& from, const PortDread::PrimeSkeleton& to,
                    uint32_t evnt, std::vector<uint8_t>& out, std::string* error) {
  Reader r{in.data(), in.size()};
  const auto u8 = [&r](size_t at) -> uint8_t { return r.Has(at, 1) ? r.data[at] : 0; };
  if (r.U32(0) != 2 || r.U16(8) != 0x0101) {
    return Fail(error, "not an Echoes compressed ANIM");
  }
  // Echoes' header after the size: 0x0101, duration, interval, root, looping, rotation divisor,
  // offset multiplier, scale multiplier, channel count, a word Prime keeps too, frame count.
  const float duration = r.F32(10);
  const float interval = r.F32(14);
  const uint32_t looping = r.U32(22);
  const uint32_t rotDiv = r.U32(26);
  const float offsetMult = r.F32(30);
  const uint32_t channels = r.U32(38);
  const uint32_t unk = r.U32(42);
  const uint32_t frames = r.U32(46);
  const size_t bitmapAt = 50;
  const size_t bitmapWords = (size_t(frames) + 31) / 32;
  if (r.bad || frames == 0 || frames > 0x10000 || channels == 0 || channels > 255 ||
      !r.Has(bitmapAt, bitmapWords * 4 + 4)) {
    return Fail(error, "ANIM header cut short");
  }
  uint32_t keys = 0;
  for (uint32_t f = 0; f < frames; ++f) {
    keys += (r.U32(bitmapAt + 4 * (f / 32)) >> (f % 32)) & 1u;
  }
  if (keys < 2 || (r.U32(bitmapAt) & 1u) == 0) {
    return Fail(error, "ANIM without keys");
  }
  const uint16_t steps = uint16_t(keys - 1);
  size_t at = bitmapAt + bitmapWords * 4 + 4;  // the key bitmap, then the channel count again
  struct Channel {
    uint32_t joint = 0;
    AnimTrack rot, off;
  };
  std::vector<Channel> chans(channels);
  for (Channel& c : chans) {
    c.joint = u8(at++);
    AnimTrack scale;
    for (AnimTrack* t : {&c.rot, &c.off, &scale}) {
      t->keys = r.U16(at);
      at += 2;
      if (t->keys == 0) continue;
      for (int j = 0; j < 3; ++j) {
        t->init[j] = int16_t(r.U16(at));
        t->bits[j] = u8(at + 2);
        at += 3;
        if (t->bits[j] > 31) return Fail(error, "ANIM track too wide");
      }
      if (t->keys != steps) return Fail(error, "ANIM track with its own key count");
    }
    if (r.bad) return Fail(error, "ANIM channels cut short");
    if (scale.keys != 0) return Fail(error, "ANIM with a scale track");
  }
  // Each step's sign bit and rotation steps, then its offset steps (Prime's order, scale aside).
  std::vector<int32_t> deltas(size_t(steps) * channels * 7, 0);
  BitReader bits{r, at};
  for (uint32_t s = 0; s < steps; ++s) {
    for (uint32_t c = 0; c < channels; ++c) {
      int32_t* d = &deltas[(size_t(s) * channels + c) * 7];
      if (chans[c].rot.keys) {
        d[0] = int32_t(bits.Bits(1));
        for (int j = 0; j < 3; ++j) d[1 + j] = bits.Signed(chans[c].rot.bits[j]);
      }
      if (chans[c].off.keys) {
        for (int j = 0; j < 3; ++j) d[4 + j] = bits.Signed(chans[c].off.bits[j]);
      }
    }
  }
  if (r.bad) return Fail(error, "ANIM keys cut short");

  std::map<uint32_t, const EchoesJoint*> byId;
  for (const EchoesJoint& joint : from) byId[joint.id] = &joint;
  // Every channel Echoes lists is posed whole there: a joint without a rotation track unrotated,
  // one without an offset track at its skeleton place (each beam's open anim ends with the gills
  // there, its close anim starts there). Prime's pose builder keeps a joint's last offset when the
  // anim gives none (CAnimData::RecalcPoseBuilder), so the beam's gill pose would stay through the
  // generic missile anims, which list the gills without tracks: the offset is written out instead.
  struct Planned {
    uint32_t channel, joint;
    int16_t place[3];  // the skeleton place, in offset steps, for a channel without an offset track
  };
  std::vector<Planned> plan;
  bool root = false;
  for (uint32_t c = 0; c < channels; ++c) {
    const bool tracks = chans[c].rot.keys != 0 || chans[c].off.keys != 0;
    const auto joint = byId.find(chans[c].joint);
    const int found = joint != byId.end() ? to.Find(joint->second->name) : -1;
    if (found < 0) {
      if (!tracks) continue;  // posed by neither game's anims
      return Fail(error, "joint " + (joint != byId.end() ? joint->second->name : std::to_string(chans[c].joint)) +
                             " has no Prime namesake");
    }
    const PortDread::PrimeJoint& pj = to.joints[size_t(found)];
    Planned p{c, pj.id, {0, 0, 0}};
    if (chans[c].off.keys == 0) {
      const int parent = pj.parent != PortDread::kPrimeNoParent ? to.IndexOfId(pj.parent) : -1;
      for (int j = 0; j < 3; ++j) {
        const float local = pj.head[j] - (parent >= 0 ? to.joints[size_t(parent)].head[j] : 0.f);
        const float stepsAway = std::round(local / offsetMult);
        if (!(offsetMult > 0.f) || std::fabs(stepsAway) > 32767.f) {
          return Fail(error, "joint " + pj.name + "'s skeleton place does not fit the ANIM's offset steps");
        }
        p.place[j] = int16_t(stepsAway);
      }
    }
    plan.push_back(p);
    root = root || pj.id == 3;
  }
  // CFBStreamedCompression measures the root's travel, by index among the channels.
  if (!root) return Fail(error, "ANIM without the root joint");

  std::vector<uint8_t> list;
  Put32(list, uint32_t(plan.size()));
  for (const Planned& p : plan) {
    const Channel& c = chans[p.channel];
    Put32(list, p.joint);
    Put16(list, steps);
    for (int j = 0; j < 3; ++j) {  // a still rotation where Echoes has none: no bits, all zero
      Put16(list, uint16_t(c.rot.keys ? c.rot.init[j] : 0));
      list.push_back(c.rot.keys ? c.rot.bits[j] : 0);
    }
    Put16(list, steps);  // a still offset at the skeleton place where Echoes has none: no bits
    for (int j = 0; j < 3; ++j) {
      Put16(list, uint16_t(c.off.keys ? c.off.init[j] : p.place[j]));
      list.push_back(c.off.keys ? c.off.bits[j] : 0);
    }
  }
  BitWriter w;
  for (uint32_t s = 0; s < steps; ++s) {
    for (const Planned& p : plan) {
      const Channel& c = chans[p.channel];
      const int32_t* d = &deltas[(size_t(s) * channels + p.channel) * 7];
      w.Put(c.rot.keys ? uint32_t(d[0]) : 0u, 1);
      if (c.rot.keys) {
        for (int j = 0; j < 3; ++j) w.Put(uint32_t(d[1 + j]), c.rot.bits[j]);
      }
      if (c.off.keys) {
        for (int j = 0; j < 3; ++j) w.Put(uint32_t(d[4 + j]), c.off.bits[j]);
      }
    }
  }
  w.Finish();

  // CStandardMultiFormatHeader, then the key bitmap (CFBStreamedCompressionTimeHeader).
  std::vector<uint8_t> head;
  Put32(head, 1);
  PutF32(head, duration);
  PutF32(head, interval);
  Put32(head, 3);  // CSegId::Root
  Put32(head, looping);
  Put32(head, rotDiv);
  PutF32(head, offsetMult);
  Put32(head, uint32_t(plan.size()));
  Put32(head, unk);
  Put32(head, frames);
  Append(head, in.data() + bitmapAt, bitmapWords * 4);
  // What CFBStreamedCompression::GetRotationsAndOffsets keeps: all but the word between the bitmap
  // and the channel list.
  const size_t memory = head.size() + list.size() + w.words.size() * 4;
  out.clear();
  Put32(out, 2);
  Put32(out, uint32_t(memory));
  Put32(out, evnt);
  Append(out, head.data(), head.size());
  Put32(out, uint32_t(plan.size()));
  Append(out, list.data(), list.size());
  for (const uint32_t word : w.words) Put32(out, word);
  return true;
}

namespace {

// Where the parts of a Prime ANCS are (CAnimCharacterSet: CCharacterSet, CAnimationSet).
struct AncsLayout {
  struct Character {
    size_t start = 0, animInfoAt = 0, animInfoEnd = 0, animIdxAt = 0, animIdxEnd = 0, end = 0;
    uint32_t animInfoCount = 0;
    std::vector<uint32_t> animIdx;  // character anim -> animation set index
  };
  struct Animation {
    uint32_t play = 0;  // the ANIM, when a play
    std::string name;   // the play's ANIM name
  };
  struct Transition {
    size_t start = 0, end = 0;
    uint32_t id = 0, a = 0, b = 0;
  };
  std::vector<Character> chars;
  size_t setStart = 0, animsAt = 0, animsEnd = 0, transEnd = 0, resAt = 0;
  std::vector<Animation> anims;
  std::vector<Transition> trans;
  std::vector<std::pair<uint32_t, uint32_t>> res;  // (ANIM, EVNT), sorted by signed ANIM id
};

bool SkipString(Reader& r, size_t& at, std::string* text = nullptr) {
  const void* end = at < r.size ? std::memchr(r.data + at, 0, r.size - at) : nullptr;
  if (end == nullptr) {
    r.bad = true;
    return false;
  }
  const size_t stop = size_t(static_cast<const uint8_t*>(end) - r.data);
  if (text != nullptr) text->assign(reinterpret_cast<const char*>(r.data + at), stop - at);
  at = stop + 1;
  return true;
}

bool SkipCount(Reader& r, size_t& at, uint32_t& n) {
  n = r.U32(at);
  at += 4;
  if (r.bad || n > r.size) {
    r.bad = true;
    return false;
  }
  return true;
}

// CMetaAnimFactory: 0 play {ANIM, index, name, start time}, 1 blend and 2 phase blend {a, b, weight,
// bool}, 3 random {n, {anim, chance}}, 4 sequence {n, anims}.
bool SkipMetaAnim(Reader& r, size_t& at, int depth, AncsLayout::Animation* play) {
  if (depth > 16) {
    r.bad = true;
    return false;
  }
  const uint32_t kind = r.U32(at);
  at += 4;
  uint32_t n = 0;
  switch (kind) {
  case 0:
    if (play != nullptr) play->play = r.U32(at);
    at += 8;
    if (!SkipString(r, at, play != nullptr ? &play->name : nullptr)) return false;
    at += 8;
    return r.Has(at, 0);
  case 1:
  case 2:
    if (!SkipMetaAnim(r, at, depth + 1, nullptr) || !SkipMetaAnim(r, at, depth + 1, nullptr)) return false;
    at += 5;
    return r.Has(at, 0);
  case 3:
    if (!SkipCount(r, at, n)) return false;
    for (uint32_t i = 0; i < n; ++i) {
      if (!SkipMetaAnim(r, at, depth + 1, nullptr)) return false;
      at += 4;
    }
    return r.Has(at, 0);
  case 4:
    if (!SkipCount(r, at, n)) return false;
    for (uint32_t i = 0; i < n; ++i) {
      if (!SkipMetaAnim(r, at, depth + 1, nullptr)) return false;
    }
    return true;
  default:
    r.bad = true;
    return false;
  }
}

// CMetaTransFactory: 0 meta-anim, 1 trans and 2 phase trans {time, two bools, flags}, 3 snap.
bool SkipMetaTrans(Reader& r, size_t& at) {
  const uint32_t kind = r.U32(at);
  at += 4;
  switch (kind) {
  case 0:
    return SkipMetaAnim(r, at, 0, nullptr);
  case 1:
  case 2:
    at += 14;
    return r.Has(at, 0);
  case 3:
    return !r.bad;
  default:
    r.bad = true;
    return false;
  }
}

bool ParseGunAncs(const std::vector<uint8_t>& d, AncsLayout& out, std::string* error) {
  Reader r{d.data(), d.size()};
  out = AncsLayout();
  size_t at = 4;  // version, the character set's table count
  uint32_t n = 0;
  uint32_t characters = 0;
  if (!SkipCount(r, at, characters) || characters == 0 || characters > 16) {
    return Fail(error, "ANCS characters cut short");
  }
  for (uint32_t c = 0; c < characters; ++c) {
    // CCharacterInfo, after its id: tables, name, model, skin, skeleton, anim names (index and two
    // strings), PAS database, particles, a word, AABBs, effects, overlay model and skin, anim index list.
    AncsLayout::Character ch;
    ch.start = at;
    const uint16_t tables = r.U16(at + 4);
    at += 6;
    if (!SkipString(r, at)) return Fail(error, "ANCS character name cut short");
    at += 12;
    ch.animInfoAt = at;
    if (!SkipCount(r, at, ch.animInfoCount)) return Fail(error, "ANCS anim names cut short");
    for (uint32_t i = 0; i < ch.animInfoCount; ++i) {
      at += 4;
      if (!SkipString(r, at) || !SkipString(r, at)) return Fail(error, "ANCS anim names cut short");
    }
    ch.animInfoEnd = at;
    const uint32_t states = r.U32(at + 4);
    at += 12;
    if (r.bad || states > 256) return Fail(error, "ANCS PAS database cut short");
    for (uint32_t s = 0; s < states; ++s) {
      const uint32_t parms = r.U32(at + 4);
      const uint32_t anims = r.U32(at + 8);
      at += 12;
      if (r.bad || parms > 8 || anims > d.size()) return Fail(error, "ANCS PAS state cut short");
      uint32_t types[8] = {};
      for (uint32_t p = 0; p < parms; ++p) {
        types[p] = r.U32(at);
        at += 12;  // type, weight function, weight
        at += types[p] == 3 ? 2 : types[p] <= 4 ? 8 : 0;  // a bool's range is two bytes
      }
      for (uint32_t a = 0; a < anims; ++a) {
        at += 4;
        for (uint32_t p = 0; p < parms; ++p) at += types[p] == 3 ? 1 : types[p] <= 4 ? 4 : 0;
      }
      if (!r.Has(at, 0)) return Fail(error, "ANCS PAS state cut short");
    }
    for (int v = 0; v < 3 + (tables > 5 ? 1 : 0); ++v) {
      if (!SkipCount(r, at, n)) return Fail(error, "ANCS particles cut short");
      at += size_t(n) * 4;
    }
    at += 4;
    if (tables > 1) {
      if (!SkipCount(r, at, n)) return Fail(error, "ANCS AABBs cut short");
      for (uint32_t i = 0; i < n; ++i) {
        if (!SkipString(r, at)) return Fail(error, "ANCS AABBs cut short");
        at += 24;
      }
    }
    if (tables > 2) {
      if (!SkipCount(r, at, n)) return Fail(error, "ANCS effects cut short");
      for (uint32_t i = 0; i < n; ++i) {
        uint32_t m = 0;
        if (!SkipString(r, at) || !SkipCount(r, at, m)) return Fail(error, "ANCS effects cut short");
        for (uint32_t k = 0; k < m; ++k) {
          if (!SkipString(r, at)) return Fail(error, "ANCS effects cut short");
          at += 8;
          if (!SkipString(r, at)) return Fail(error, "ANCS effects cut short");
          at += 12;
        }
      }
    }
    if (tables > 3) at += 8;
    if (tables <= 4) return Fail(error, "ANCS character without an anim index list");
    ch.animIdxAt = at;
    if (!SkipCount(r, at, n)) return Fail(error, "ANCS anim index list cut short");
    for (uint32_t i = 0; i < n; ++i, at += 4) ch.animIdx.push_back(r.U32(at));
    ch.animIdxEnd = at;
    ch.end = at;
    if (r.bad || at > d.size()) return Fail(error, "ANCS character cut short");
    out.chars.push_back(std::move(ch));
  }
  // CAnimationSet: tables, animations (name, meta-anim), transitions, default transition, additive
  // anims, half transitions, (ANIM, EVNT) list.
  out.setStart = at;
  const uint16_t tables = r.U16(at);
  at += 2;
  out.animsAt = at;
  if (!SkipCount(r, at, n)) return Fail(error, "ANCS animations cut short");
  for (uint32_t i = 0; i < n; ++i) {
    AncsLayout::Animation anim;
    if (!SkipString(r, at)) return Fail(error, "ANCS animations cut short");
    const bool play = r.U32(at) == 0;
    if (!SkipMetaAnim(r, at, 0, play ? &anim : nullptr)) return Fail(error, "ANCS meta-anim it does not know");
    out.anims.push_back(std::move(anim));
  }
  out.animsEnd = at;
  if (!SkipCount(r, at, n)) return Fail(error, "ANCS transitions cut short");
  for (uint32_t i = 0; i < n; ++i) {
    AncsLayout::Transition t;
    t.start = at;
    t.id = r.U32(at);
    t.a = r.U32(at + 4);
    t.b = r.U32(at + 8);
    at += 12;
    if (!SkipMetaTrans(r, at)) return Fail(error, "ANCS transition it does not know");
    t.end = at;
    out.trans.push_back(t);
  }
  out.transEnd = at;
  if (!SkipMetaTrans(r, at)) return Fail(error, "ANCS default transition it does not know");
  if (tables > 1) {
    if (!SkipCount(r, at, n)) return Fail(error, "ANCS additive anims cut short");
    at += size_t(n) * 12 + 8;
  }
  if (tables > 2) {
    if (!SkipCount(r, at, n)) return Fail(error, "ANCS half transitions cut short");
    for (uint32_t i = 0; i < n; ++i) {
      at += 4;
      if (!SkipMetaTrans(r, at)) return Fail(error, "ANCS half transition it does not know");
    }
  }
  if (tables <= 3) return Fail(error, "ANCS without its (ANIM, EVNT) list");
  out.resAt = at;
  if (!SkipCount(r, at, n)) return Fail(error, "ANCS (ANIM, EVNT) list cut short");
  for (uint32_t i = 0; i < n; ++i, at += 8) out.res.emplace_back(r.U32(at), r.U32(at + 4));
  if (r.bad || at != d.size()) return Fail(error, "ANCS does not end where its parts do");
  return true;
}

}  // namespace

bool FindGunMissileAnims(const std::vector<uint8_t>& ancs, GunMissileAnims& out, std::string* error) {
  out = GunMissileAnims();
  AncsLayout layout;
  if (!ParseGunAncs(ancs, layout, error)) return false;
  const std::vector<uint32_t>& idx = layout.chars[0].animIdx;
  if (idx.size() < 9) return true;
  for (int i = 0; i < 4; ++i) {
    const uint32_t set = idx[size_t(5 + i)];
    // Retro's spelling: powerMissleAway, genericMissleShoot...
    if (set >= layout.anims.size() || layout.anims[set].play == 0 ||
        layout.anims[set].name.find("Missle") == std::string::npos) {
      return true;
    }
    out.anim[i] = layout.anims[set].play;
    for (const auto& [anim, evnt] : layout.res) {
      if (anim == out.anim[i]) out.evnt[i] = evnt;
    }
  }
  out.found = true;
  return true;
}

bool AddGunMissileAnims(std::vector<uint8_t>& ancs, const GunMissileAnims& missiles, std::string* error) {
  if (!missiles.found) return Fail(error, "no missile anims");
  AncsLayout l;
  if (!ParseGunAncs(ancs, l, error)) return false;
  constexpr int kAdded = int(std::size(kMissileAnimNames));
  static_assert(kAdded == 5, "the four missile anims, then the first shot");
  const uint32_t ids[kAdded] = {Cannon(missiles.anim[0]), Cannon(missiles.anim[1]), Cannon(missiles.anim[2]),
                                Cannon(missiles.anim[3]), Cannon(kFirstMissileShot)};
  const uint32_t evnts[kAdded] = {missiles.evnt[0], missiles.evnt[1], missiles.evnt[2], missiles.evnt[3],
                                  missiles.evnt[2]};
  const uint32_t base = uint32_t(l.anims.size());
  // Prime's missile anims' set indices -> Echoes', for their transitions.
  std::map<uint32_t, uint32_t> mirror;
  for (int i = 0; i < 4; ++i) mirror[l.chars[0].animIdx[size_t(5 + i)]] = base + uint32_t(i);

  const std::vector<uint8_t>& d = ancs;
  std::vector<uint8_t> out(d.begin(), d.begin() + std::ptrdiff_t(l.chars[0].start));
  for (const AncsLayout::Character& c : l.chars) {
    const uint32_t first = uint32_t(c.animIdx.size());
    Append(out, d.data() + c.start, c.animInfoAt - c.start);
    Put32(out, c.animInfoCount + uint32_t(kAdded));
    Append(out, d.data() + c.animInfoAt + 4, c.animInfoEnd - c.animInfoAt - 4);
    for (int i = 0; i < kAdded; ++i) {
      Put32(out, first + uint32_t(i));
      out.push_back(0);  // the first name is empty, as Prime's are
      PutString(out, kMissileAnimNames[i]);
    }
    Append(out, d.data() + c.animInfoEnd, c.animIdxAt - c.animInfoEnd);
    Put32(out, first + uint32_t(kAdded));
    Append(out, d.data() + c.animIdxAt + 4, c.animIdxEnd - c.animIdxAt - 4);
    for (int i = 0; i < kAdded; ++i) Put32(out, base + uint32_t(i));
    Append(out, d.data() + c.animIdxEnd, c.end - c.animIdxEnd);
  }
  Append(out, d.data() + l.setStart, l.animsAt - l.setStart);
  Put32(out, base + uint32_t(kAdded));
  Append(out, d.data() + l.animsAt + 4, l.animsEnd - l.animsAt - 4);
  for (int i = 0; i < kAdded; ++i) {
    PutString(out, kMissileAnimNames[i]);
    Put32(out, 0);  // play
    Put32(out, ids[i]);
    Put32(out, base + uint32_t(i));
    PutString(out, kMissileAnimNames[i]);
    PutF32(out, 0.f);  // start time, of CCharAnimTime's type 2 as Prime's plays
    Put32(out, 2);
  }
  std::vector<std::vector<uint8_t>> added;
  for (const AncsLayout::Transition& t : l.trans) {
    const auto a = mirror.find(t.a);
    const auto b = mirror.find(t.b);
    if (a == mirror.end() && b == mirror.end()) continue;
    std::vector<uint8_t> copy;
    Put32(copy, t.id);
    Put32(copy, a != mirror.end() ? a->second : t.a);
    Put32(copy, b != mirror.end() ? b->second : t.b);
    Append(copy, d.data() + t.start + 12, t.end - t.start - 12);
    added.push_back(std::move(copy));
  }
  Put32(out, uint32_t(l.trans.size() + added.size()));
  Append(out, d.data() + l.animsEnd + 4, l.transEnd - l.animsEnd - 4);
  for (const auto& t : added) Append(out, t.data(), t.size());
  Append(out, d.data() + l.transEnd, l.resAt - l.transEnd);
  // CCharacterFactory finds an anim's EVNT by binary search over the signed ids.
  std::vector<std::pair<uint32_t, uint32_t>> res = l.res;
  for (int i = 0; i < kAdded; ++i) res.emplace_back(ids[i], evnts[i]);
  std::sort(res.begin(), res.end(), [](const auto& x, const auto& y) { return int32_t(x.first) < int32_t(y.first); });
  Put32(out, uint32_t(res.size()));
  for (const auto& [anim, evnt] : res) {
    Put32(out, anim);
    Put32(out, evnt);
  }
  AncsLayout check;
  std::string why;
  if (!ParseGunAncs(out, check, &why) || check.anims.size() != size_t(base) + kAdded) {
    return Fail(error, "the ANCS with Echoes' missile anims does not read back: " + why);
  }
  ancs = std::move(out);
  return true;
}

bool FloatNormals(PrimeCmdl& model, std::string* error) {
  Reader h{model.header.data(), model.header.size()};
  const uint32_t flags = h.U32(8);
  if (h.bad || model.materialSets.empty() || model.arrays.size() < 4) {
    return Fail(error, "model has no arrays");
  }
  if (!(flags & 0x2)) {
    return true;
  }
  for (const auto& setBytes : model.materialSets) {
    MaterialSet set;
    if (!ParseSet(setBytes, set, error)) {
      return false;
    }
    // Every set draws the same lists; checking against each covers the vertex sizes all see.
    for (auto surface : model.surfaces) {
      uint32_t vaf;
      if (!SurfaceFlags(surface, set, vaf, error) ||
          !RewriteList(
              surface, vaf, false, [](uint8_t& c) { return (c & 7) == 1; }, [](int, uint32_t&) { return true; },
              error)) {
        return false;
      }
    }
  }
  MaterialSet set;
  ParseSet(model.materialSets[0], set, error);
  for (auto& surface : model.surfaces) {
    uint32_t vaf;
    SurfaceFlags(surface, set, vaf, error);
    RewriteList(
        surface, vaf, false,
        [](uint8_t& c) {
          c &= ~7;
          return true;
        },
        [](int, uint32_t&) { return true; }, error);
  }
  const std::vector<uint8_t>& shorts = model.arrays[1];
  Reader n{shorts.data(), shorts.size()};
  std::vector<uint8_t> floats;
  for (size_t at = 0; at + 2 <= shorts.size(); at += 2) {
    const float value = float(int16_t(n.U16(at))) / 16384.f;
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    Put32(floats, bits);
  }
  // Whole normals only: a short array padded to 32 bytes can end mid-normal.
  floats.resize(floats.size() / 12 * 12);
  Pad32(floats);
  model.arrays[1] = std::move(floats);
  Set32(model.header, 8, flags & ~0x2u);
  return true;
}

bool MergeCmdl(PrimeCmdl& a, const PrimeCmdl& b, std::string* error) {
  if (a.materialSets.size() != 1 || b.materialSets.size() != 1) {
    return Fail(error, "merge needs one material set each");
  }
  MaterialSet setA, setB;
  if (!ParseSet(a.materialSets[0], setA, error) || !ParseSet(b.materialSets[0], setB, error)) {
    return false;
  }
  PrimeCmdl other = b;
  if (!DropShortUvs(a, setA, error) || !DropShortUvs(other, setB, error)) {
    return false;
  }
  Reader ha{a.header.data(), a.header.size()};
  Reader hb{other.header.data(), other.header.size()};
  const uint32_t flags = ha.U32(8);
  if (ha.bad || hb.bad || flags != hb.U32(8) || a.arrays.size() != 4 || other.arrays.size() != 4) {
    return Fail(error, "merged models differ in their vertex layout");
  }
  // Positions, normals, colours (both channels), float UVs (textures 0-6).
  const size_t stride[4] = {12, size_t(flags & 0x2 ? 6 : 12), 4, 8};
  constexpr int kArrayOf[11] = {0, 1, 2, 2, 3, 3, 3, 3, 3, 3, 3};
  uint32_t base[4];
  for (int k = 0; k < 4; ++k) {
    base[k] = uint32_t(a.arrays[k].size() / stride[k]);
    a.arrays[k].resize(base[k] * stride[k]);
    Append(a.arrays[k], other.arrays[k].data(), other.arrays[k].size());
    Pad32(a.arrays[k]);
  }
  const uint32_t materials = uint32_t(setA.materials.size());
  for (auto& surface : other.surfaces) {
    uint32_t vaf;
    if (!SurfaceFlags(surface, setB, vaf, error) ||
        !RewriteList(
            surface, vaf, true, [](uint8_t&) { return true; },
            [&base, &kArrayOf](int attr, uint32_t& value) {
              value += base[kArrayOf[attr]];
              return true;
            },
            error)) {
      return false;
    }
    Reader r{surface.data(), surface.size()};
    Set32(surface, 0x0C, r.U32(0x0C) + materials);
    a.surfaces.push_back(std::move(surface));
  }
  const uint32_t textures = uint32_t(setA.textures.size());
  for (auto& material : setB.materials) {
    Reader m{material.data(), material.size()};
    const uint32_t ntex = m.U32(4);
    for (uint32_t i = 0; i < ntex; ++i) {
      Set32(material, 8 + i * 4, m.U32(8 + i * 4) + textures);
    }
    // Every index 16-bit now, as the rewritten lists are.
    const size_t at = VertexFlagsAt(material);
    uint32_t vaf = m.U32(at);
    for (int i = 0; i < 11; ++i) {
      if (((vaf >> (2 * i)) & 3) == 2) vaf |= 1u << (2 * i);
    }
    Set32(material, at, vaf);
    setA.materials.push_back(std::move(material));
  }
  setA.textures.insert(setA.textures.end(), setB.textures.begin(), setB.textures.end());
  a.materialSets[0] = WriteSet(setA);
  for (int i = 0; i < 3; ++i) {
    const float lo = std::min(ha.F32(0x0C + i * 4), hb.F32(0x0C + i * 4));
    const float hi = std::max(ha.F32(0x18 + i * 4), hb.F32(0x18 + i * 4));
    uint32_t bits;
    std::memcpy(&bits, &lo, 4);
    Set32(a.header, 0x0C + i * 4, bits);
    std::memcpy(&bits, &hi, 4);
    Set32(a.header, 0x18 + i * 4, bits);
  }
  return true;
}

bool RigidSkin(PrimeCmdl& model, uint32_t joint, std::vector<uint8_t>& cskr, std::string* error) {
  Reader h{model.header.data(), model.header.size()};
  const uint32_t flags = h.U32(8);
  if (h.bad || model.arrays.size() < 2 || (flags & 0x3)) {
    return Fail(error, "rigid skin needs an unskinned model with float normals");
  }
  // Eight 12-byte entries fill 96 bytes, so the count stays whole after padding to 32.
  const size_t count = (std::max(model.arrays[0].size(), model.arrays[1].size()) / 12 + 7) & ~size_t(7);
  model.arrays[0].resize(count * 12, 0);
  model.arrays[1].resize(count * 12, 0);
  Set32(model.header, 8, flags | 0x1);
  cskr.clear();
  Put32(cskr, 1);  // groups
  Put32(cskr, 1);  // weights in the group
  Put32(cskr, joint);
  Put32(cskr, 0x3F800000);  // 1.0
  Put32(cskr, uint32_t(count));
  Put32(cskr, 0xFFFFFFFF);
  Put32(cskr, uint32_t(count));
  Put32(cskr, 0xFFFFFFFF);
  Put32(cskr, uint32_t(count));
  return true;
}

bool MainJoint(const std::vector<uint8_t>& cskr, uint32_t& joint, std::string* error) {
  Reader r{cskr.data(), cskr.size()};
  const uint32_t groups = r.U32(0);
  if (r.bad || groups > cskr.size() / 8) {
    return Fail(error, "CSKR cut short");
  }
  std::vector<std::pair<uint32_t, double>> votes;
  size_t at = 4;
  for (uint32_t g = 0; g < groups; ++g) {
    const uint32_t n = r.U32(at);
    if (r.bad || !r.Has(at + 4, size_t(n) * 8 + 4)) {
      return Fail(error, "CSKR group cut short");
    }
    const uint32_t vertices = r.U32(at + 4 + size_t(n) * 8);
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t bone = r.U32(at + 4 + i * 8);
      const double weight = double(r.F32(at + 8 + i * 8)) * vertices;
      auto same = std::find_if(votes.begin(), votes.end(), [bone](const auto& v) { return v.first == bone; });
      if (same == votes.end()) {
        votes.emplace_back(bone, weight);
      } else {
        same->second += weight;
      }
    }
    at += 8 + size_t(n) * 8;
  }
  if (votes.empty()) {
    return Fail(error, "CSKR has no weights");
  }
  joint = std::max_element(votes.begin(), votes.end(),
                           [](const auto& x, const auto& y) { return x.second < y.second; })
              ->first;
  return true;
}

bool ParseCinf(const uint8_t* data, size_t size, std::vector<EchoesJoint>& out, std::string* error) {
  Reader r{data, size};
  out.clear();
  const uint32_t count = r.U32(0);
  if (r.bad || count > size / 4) {
    return Fail(error, "CINF cut short");
  }
  size_t at = 4;
  std::map<uint32_t, size_t> byId;
  for (uint32_t i = 0; i < count; ++i) {
    EchoesJoint joint;
    joint.id = r.U32(at);
    joint.parent = r.U32(at + 4);
    for (int c = 0; c < 3; ++c) joint.position[c] = r.F32(at + 8 + size_t(c) * 4);
    at += 8 + 12 + 16 + 16;  // position, rotation, local rotation
    const uint32_t links = r.U32(at);
    at += 4 + size_t(links) * 4;
    if (r.bad || links > size) {
      return Fail(error, "CINF joints cut short");
    }
    byId[joint.id] = out.size();
    out.push_back(std::move(joint));
  }
  const uint32_t order = r.U32(at);
  at += 4 + size_t(order) * 4;
  const uint32_t names = r.U32(at);
  at += 4;
  if (r.bad || order > size || names > size) {
    return Fail(error, "CINF cut short");
  }
  for (uint32_t i = 0; i < names; ++i) {
    const uint8_t* text = static_cast<const uint8_t*>(at < size ? std::memchr(data + at, 0, size - at) : nullptr);
    if (text == nullptr) {
      return Fail(error, "CINF name cut short");
    }
    std::string name(reinterpret_cast<const char*>(data + at), size_t(text - (data + at)));
    at += name.size() + 1;
    const uint32_t id = r.U32(at);
    at += 4;
    if (r.bad) {
      return Fail(error, "CINF names cut short");
    }
    const auto found = byId.find(id);
    if (found != byId.end()) {
      out[found->second].name = std::move(name);
    }
  }
  return true;
}

bool ConvertCskr(const std::vector<uint8_t>& in, const std::vector<EchoesJoint>& from,
                 const PortDread::PrimeSkeleton& to, std::vector<uint8_t>& out, std::vector<std::string>* folded,
                 std::string* error) {
  // Each named Echoes joint to its Prime joint: of its name, else of its nearest named ancestor's.
  // Echoes renamed the Varia shoulder pads' joints; their Prime ones are L/R_varias2_SDK.
  const auto primeJoint = [&to](const std::string& name) -> int {
    int found = to.Find(name);
    if (found < 0 && name == "L_varia_SDK") found = to.Find("L_varias2_SDK");
    if (found < 0 && name == "R_varia_SDK") found = to.Find("R_varias2_SDK");
    return found;
  };
  std::map<uint32_t, const EchoesJoint*> byId;
  for (const EchoesJoint& joint : from) {
    byId[joint.id] = &joint;
  }
  std::map<uint32_t, uint32_t> remap;
  std::set<std::string> foldedNames;
  for (const EchoesJoint& joint : from) {
    if (joint.name.empty()) {
      continue;
    }
    const EchoesJoint* walk = &joint;
    int found = primeJoint(walk->name);
    for (size_t depth = 0; found < 0 && depth < from.size(); ++depth) {
      const auto parent = byId.find(walk->parent);
      if (parent == byId.end() || parent->second->name.empty()) {
        walk = nullptr;
        break;
      }
      walk = parent->second;
      found = primeJoint(walk->name);
    }
    if (found < 0) {
      if (walk != nullptr) {
        return Fail(error, "joint " + joint.name + " loops in its parents");
      }
      foldedNames.insert(joint.name + "->?");
      continue;
    }
    remap[joint.id] = to.joints[size_t(found)].id;
    if (walk != &joint) {
      foldedNames.insert(joint.name + "->" + to.joints[size_t(found)].name);
    }
  }
  if (folded != nullptr) {
    folded->assign(foldedNames.begin(), foldedNames.end());
  }

  Reader r{in.data(), in.size()};
  const uint32_t groups = r.U32(0);
  if (r.bad || groups > in.size() / 8) {
    return Fail(error, "CSKR cut short");
  }
  out.clear();
  Put32(out, groups);
  size_t at = 4;
  uint32_t total = 0;
  for (uint32_t g = 0; g < groups; ++g) {
    const uint32_t n = r.U32(at);
    at += 4;
    if (r.bad || !r.Has(at, size_t(n) * 8 + 4)) {
      return Fail(error, "CSKR group cut short");
    }
    // Prime's joints in first-use order, their weights summed (in double, rounded once).
    std::vector<std::pair<uint32_t, double>> weights;
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t bone = r.U32(at);
      const float weight = r.F32(at + 4);
      at += 8;
      const auto mapped = remap.find(bone);
      if (mapped == remap.end()) {
        const auto joint = byId.find(bone);
        return Fail(error, "weight on joint " + (joint != byId.end() ? joint->second->name : Hex8(bone)) +
                               ", which has no Prime joint");
      }
      auto same = std::find_if(weights.begin(), weights.end(),
                               [&mapped](const auto& w) { return w.first == mapped->second; });
      if (same == weights.end()) {
        weights.emplace_back(mapped->second, double(weight));
      } else {
        same->second += double(weight);
      }
    }
    const uint32_t vertices = r.U32(at);
    at += 4;
    Put32(out, uint32_t(weights.size()));
    for (const auto& [bone, weight] : weights) {
      Put32(out, bone);
      const float rounded = float(weight);
      uint32_t bits;
      std::memcpy(&bits, &rounded, 4);
      Put32(out, bits);
    }
    Put32(out, vertices);
    total += vertices;
  }
  // Echoes' footer (its own vertex tables) is dropped; Prime's names no extra vertices.
  Put32(out, 0xFFFFFFFF);
  Put32(out, total);
  Put32(out, 0xFFFFFFFF);
  Put32(out, total);
  return true;
}

bool IsLimbJoint(const std::string& name) {
  static const char* const kLimbs[] = {"L_varia_SDK", "R_varia_SDK", "L_varias2_SDK", "R_varias2_SDK", "L_shoulder",
                                       "R_shoulder",  "L_elbow",     "R_elbow",       "L_wrist",       "R_wrist",
                                       "L_hip",       "R_hip",       "L_knee",        "R_knee",        "L_ankle",
                                       "R_ankle",     "L_ball",      "R_ball"};
  for (const char* limb : kLimbs) {
    if (name == limb) return true;
  }
  // The fingers.
  for (const char* finger : {"thumb", "Index", "middle", "Ring", "Pinky"}) {
    if (name.size() > 2 && (name[0] == 'L' || name[0] == 'R') && name[1] == '_' &&
        name.compare(2, std::strlen(finger), finger) == 0) {
      return true;
    }
  }
  return false;
}

void RgbToHsv(const uint8_t* c, double& h, double& s, double& v) {
  const double r = c[0] / 255.0, g = c[1] / 255.0, b = c[2] / 255.0;
  const double hi = std::max({r, g, b}), lo = std::min({r, g, b}), d = hi - lo;
  v = hi;
  s = hi > 0.0 ? d / hi : 0.0;
  if (d <= 0.0) {
    h = 0.0;
  } else if (hi == r) {
    h = 60.0 * std::fmod((g - b) / d + 6.0, 6.0);
  } else if (hi == g) {
    h = 60.0 * ((b - r) / d + 2.0);
  } else {
    h = 60.0 * ((r - g) / d + 4.0);
  }
}

void HsvToRgb(double h, double s, double v, uint8_t* c) {
  h = std::fmod(std::fmod(h, 360.0) + 360.0, 360.0);
  s = std::clamp(s, 0.0, 1.0);
  v = std::clamp(v, 0.0, 1.0);
  const double k = h / 60.0, f = k - std::floor(k);
  const double p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
  double r, g, b;
  switch (int(k) % 6) {
  case 0: r = v, g = t, b = p; break;
  case 1: r = q, g = v, b = p; break;
  case 2: r = p, g = v, b = t; break;
  case 3: r = p, g = q, b = v; break;
  case 4: r = t, g = p, b = v; break;
  default: r = v, g = p, b = q; break;
  }
  c[0] = uint8_t(std::lround(r * 255.0));
  c[1] = uint8_t(std::lround(g * 255.0));
  c[2] = uint8_t(std::lround(b * 255.0));
}

// A model's triangles (set 0) through `fn`: the textures of the triangle's material, then its three
// vertices' UVs (texture 0's, from the float UV array) and position indices, wound as drawn (a
// strip's odd triangles turned back).
bool ForEachUvTriangle(const PrimeCmdl& model, const UvTriangleFn& fn, std::string* error) {
  return ForEachMaterialUvTriangle(
      model,
      [&fn](uint32_t, const std::vector<uint32_t>& textures, const float (&uv)[3][2], const uint32_t (&pos)[3]) {
        fn(textures, uv, pos);
      },
      error);
}

bool ForEachMaterialUvTriangle(const PrimeCmdl& model, const MaterialUvTriangleFn& fn, std::string* error) {
  MaterialSet set;
  if (model.materialSets.empty() || model.arrays.size() < 4 || !ParseSet(model.materialSets[0], set, error)) {
    return Fail(error, "model has no material set or UVs");
  }
  const std::vector<uint8_t>& uvs = model.arrays[3];
  Reader u{uvs.data(), uvs.size()};
  for (const auto& surface : model.surfaces) {
    uint32_t vaf;
    if (!SurfaceFlags(surface, set, vaf, error)) return false;
    Reader r{surface.data(), surface.size()};
    const uint32_t material = r.U32(0x0C);
    std::vector<uint32_t> textures;
    {
      const auto& m = set.materials[material];
      Reader mr{m.data(), m.size()};
      const uint32_t ntex = mr.U32(4);
      for (uint32_t i = 0; i < ntex && !mr.bad; ++i) {
        const uint32_t index = mr.U32(8 + i * 4);
        if (index < set.textures.size()) textures.push_back(set.textures[index]);
      }
    }
    if (textures.empty() || ((vaf >> 8) & 3) == 0) continue;  // no texture 0 coordinates
    const size_t start = (0x2C + size_t(r.U32(0x1C)) + 31) & ~size_t(31);
    size_t vertexSize = 0;
    for (int i = 0; i < 11; ++i) {
      const uint32_t mode = (vaf >> (2 * i)) & 3;
      vertexSize += mode == 0 ? 0 : mode - 1;
    }
    size_t at = start;
    while (at < surface.size() && surface[at] != 0) {
      const uint8_t command = surface[at];
      const uint16_t count = r.U16(at + 1);
      if (r.bad || !r.Has(at + 3, size_t(count) * vertexSize)) return Fail(error, "display list cut short");
      at += 3;
      std::vector<std::array<float, 2>> verts(count);
      std::vector<uint32_t> positions(count, 0);
      for (uint16_t v = 0; v < count; ++v) {
        for (int i = 0; i < 11; ++i) {
          const uint32_t mode = (vaf >> (2 * i)) & 3;
          if (mode == 0) continue;
          const uint32_t value = mode == 2 ? surface[at] : r.U16(at);
          at += mode - 1;
          if (i == 0) {
            positions[v] = value;
          } else if (i == 4) {  // texture 0
            verts[v] = {u.F32(size_t(value) * 8), u.F32(size_t(value) * 8 + 4)};
          }
        }
      }
      if (u.bad) return Fail(error, "UV index out of range");
      const auto emit = [&](int a, int b, int c) {
        const float tri[3][2] = {{verts[a][0], verts[a][1]}, {verts[b][0], verts[b][1]}, {verts[c][0], verts[c][1]}};
        const uint32_t pos[3] = {positions[a], positions[b], positions[c]};
        fn(material, textures, tri, pos);
      };
      switch (command & 0xF8) {
      case 0x90:
        for (int i = 0; i + 2 < count; i += 3) emit(i, i + 1, i + 2);
        break;
      case 0x98:
        for (int i = 0; i + 2 < count; ++i) {
          if (i % 2 == 0) {
            emit(i, i + 1, i + 2);
          } else {
            emit(i + 1, i, i + 2);
          }
        }
        break;
      case 0xA0:
        for (int i = 1; i + 1 < count; ++i) emit(0, i, i + 1);
        break;
      default:
        break;
      }
    }
  }
  return true;
}

namespace {

// Which way a triangle of `model` (its position indices, wound as drawn) faces: `middle`, how far
// toward the body's middle, the plane x = 0 (1 straight at it, -1 straight away); `front`, how far
// forward (Prime's bodies stand facing +y).
struct Facing {
  double middle = 0.0, front = 0.0;
};
Facing FacingOf(const PrimeCmdl& model, const uint32_t (&pos)[3]) {
  if (model.arrays.empty()) return {};
  Reader r{model.arrays[0].data(), model.arrays[0].size()};
  double p[3][3];
  for (int v = 0; v < 3; ++v) {
    for (int c = 0; c < 3; ++c) p[v][c] = r.F32(size_t(pos[v]) * 12 + size_t(c) * 4);
  }
  if (r.bad) return {};
  const double e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
  const double e2[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
  const double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
  const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  const double x = (p[0][0] + p[1][0] + p[2][0]) / 3.0;
  if (length <= 0.0) return {};
  return {x == 0.0 ? 0.0 : -n[0] / length * (x > 0.0 ? 1.0 : -1.0), n[1] / length};
}

}  // namespace

// The joint each vertex of a Prime CSKR weighs most on, in vertex order.
bool MainJoints(const std::vector<uint8_t>& cskr, std::vector<uint32_t>& joints, std::string* error) {
  Reader r{cskr.data(), cskr.size()};
  const uint32_t groups = r.U32(0);
  if (r.bad || groups > cskr.size() / 8) return Fail(error, "CSKR cut short");
  joints.clear();
  size_t at = 4;
  for (uint32_t g = 0; g < groups; ++g) {
    const uint32_t n = r.U32(at);
    if (r.bad || !r.Has(at + 4, size_t(n) * 8 + 4)) return Fail(error, "CSKR group cut short");
    uint32_t best = 0;
    float weight = -1.f;
    for (uint32_t i = 0; i < n; ++i) {
      const float w = r.F32(at + 8 + i * 8);
      if (w > weight) {
        weight = w;
        best = r.U32(at + 4 + i * 8);
      }
    }
    const uint32_t vertices = r.U32(at + 4 + size_t(n) * 8);
    joints.insert(joints.end(), vertices, best);
    at += 8 + size_t(n) * 8;
  }
  return true;
}

namespace {

// (TexelVotes, port_echoes_suits.h: limb and other votes over a texture's texels, LimbTexels, and
// where a thigh triangle facing the panel's way lies, MarkPanelSide and KeepUndersuit: 1 leaning
// out by up to half, 2 facing in or across.)

// The texels of a w x h texture a triangle covers (its UVs; the edges a little generous, so seams
// close), wrapped into the texture, each through fn(index); none for one wrapping the whole texture.
template <typename Fn>
void ForEachTexel(int w, int h, const float (&uv)[3][2], const Fn& fn) {
  double xs[3], ys[3];
  for (int i = 0; i < 3; ++i) {
    xs[i] = double(uv[i][0]) * w;
    ys[i] = double(uv[i][1]) * h;
  }
  const int x0 = int(std::floor(std::min({xs[0], xs[1], xs[2]}))), x1 = int(std::ceil(std::max({xs[0], xs[1], xs[2]})));
  const int y0 = int(std::floor(std::min({ys[0], ys[1], ys[2]}))), y1 = int(std::ceil(std::max({ys[0], ys[1], ys[2]})));
  if (double(x1 - x0) * double(y1 - y0) > double(w) * h) return;  // a triangle wrapping the whole texture
  const double den = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2]);
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      bool inside = true;
      if (std::abs(den) > 1e-12) {
        const double px = x + 0.5, py = y + 0.5;
        const double w0 = ((ys[1] - ys[2]) * (px - xs[2]) + (xs[2] - xs[1]) * (py - ys[2])) / den;
        const double w1 = ((ys[2] - ys[0]) * (px - xs[2]) + (xs[0] - xs[2]) * (py - ys[2])) / den;
        inside = w0 >= -0.02 && w1 >= -0.02 && 1.0 - w0 - w1 >= -0.02;
      }
      if (!inside) continue;
      fn(size_t(((y % h) + h) % h) * size_t(w) + size_t(((x % w) + w) % w));
    }
  }
}

}  // namespace

void VoteTriangle(TexelVotes& votes, const float (&uv)[3][2], bool limb) {
  auto& grid = limb ? votes.limb : votes.other;
  ForEachTexel(votes.width, votes.height, uv, [&grid](size_t k) {
    if (grid[k] < 0xFFFF) ++grid[k];
  });
}

namespace {

// The texels of a thigh triangle facing the panel's way (KeepUndersuit): `inward` for one facing in
// or across, where the panel surely is.
void MarkPanelSide(TexelVotes& votes, const float (&uv)[3][2], bool inward) {
  const uint8_t mark = inward ? 2 : 1;
  ForEachTexel(votes.width, votes.height, uv, [&votes, mark](size_t k) {
    votes.panelSide[k] = std::max(votes.panelSide[k], mark);
  });
}

// The thighs' undersuit panel, texel by texel, out of the limbs `limb` (LimbTexels) of the texture
// `rgba`. Where a thigh triangle facing the panel's way lies (`panelSide`; and the texels no triangle
// covers two round it, as LimbTexels fills them), a texel of the undersuit's yellow (38 to 70
// degrees) is yellow when its 5x5 neighbourhood holds more of that yellow than of the plates'
// orange (warm, below 38), and any other texel (orange, the line between, a shade) when it lies
// deep in the yellow (ten more of it than of orange round it). Measured texel by texel on Echoes'
// Varia, the panel is 39 degrees and up, the plates 36 and down, the abdomen 36 to 54. The panel is
// then the yellow texels joined (side by side, through yellow) to one a triangle facing in or across
// covers (`panelSide` 2): it goes with the torso. So the panel ends where the texture's colour turns,
// along the line Echoes draws between it and a plate (that line stays the plate's), round the back
// of the thigh as far as it reaches; and a plate's lighter patch, as yellow but cut off from the
// panel by that line or the plate's orange (the wedges at the back of the thighs, test 34), stays
// with the plate, as a highlight or a shade smaller than the window stays with its side. (Whole
// triangles took one side or the other till 0.46.0 test 35: a triangle across that line left a
// sliver of the wrong colour, and the low-poly in-game body's edges were ragged.)
void KeepUndersuit(const TexelVotes& votes, const std::vector<uint8_t>& rgba, std::vector<uint8_t>& limb) {
  const int w = votes.width, h = votes.height;
  const size_t count = size_t(w) * size_t(h);
  if (votes.panelSide.size() != count || rgba.size() < count * 4 || limb.size() != count) return;
  std::vector<int8_t> tone(count, 0);
  for (size_t k = 0; k < count; ++k) {
    double hue, sat, val;
    RgbToHsv(&rgba[k * 4], hue, sat, val);
    if (sat <= 0.25 || val <= 0.25 || !(hue < 70.0 || hue >= 330.0)) continue;
    tone[k] = hue >= 38.0 && hue < 70.0 ? 1 : -1;
  }
  const auto at = [w, h](int x, int y) {
    return size_t(((y % h) + h) % h) * size_t(w) + size_t(((x % w) + w) % w);
  };
  // The lines Echoes draws (dark brown or black: value below 0.45), and the bright red one between
  // the panel and a plate.
  std::vector<uint8_t> dark(count, 0), red(count, 0);
  for (size_t k = 0; k < count; ++k) {
    const uint8_t top = std::max({rgba[k * 4], rgba[k * 4 + 1], rgba[k * 4 + 2]});
    dark[k] = top < 115 ? 1 : 0;
    double hue, sat, val;
    RgbToHsv(&rgba[k * 4], hue, sat, val);
    red[k] = dark[k] == 0 && sat > 0.5 && (hue < 20.0 || hue >= 330.0) ? 1 : 0;
  }
  // 0: not the panel's; 1: yellow, or a line drawn on the thigh (not the gaps between the texture's
  // pieces, which no triangle covers), where the panel may be; 2: that, where it surely is (a drawn
  // line only passes the panel on: grow starts from yellow).
  std::vector<uint8_t> yellow(count, 0);
  std::vector<size_t> grow;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t k = size_t(y) * size_t(w) + size_t(x);
      if (limb[k] == 0) continue;
      const bool covered = votes.limb[k] + votes.other[k] != 0;
      uint8_t side = votes.panelSide[k];
      if (side == 0 && !covered) {
        for (int dy = -2; dy <= 2; ++dy) {
          for (int dx = -2; dx <= 2; ++dx) side = std::max(side, votes.panelSide[at(x + dx, y + dy)]);
        }
      }
      if (side == 0) continue;
      if (dark[k] != 0) {
        if (covered) yellow[k] = side;
        continue;
      }
      int sum = 0;
      for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) sum += tone[at(x + dx, y + dy)];
      }
      if (!(tone[k] > 0 ? sum > 0 : sum >= 10)) continue;
      yellow[k] = side;
      if (side == 2) grow.push_back(k);
    }
  }
  // From where the panel surely is, through yellow and the lines drawn on it, side by side.
  std::vector<uint8_t> panel(count, 0);
  for (const size_t k : grow) {
    limb[k] = 0;
    panel[k] = 1;
  }
  while (!grow.empty()) {
    const size_t k = grow.back();
    grow.pop_back();
    const int x = int(k % size_t(w)), y = int(k / size_t(w));
    for (const auto& [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
      const size_t n = at(x + dx, y + dy);
      if (yellow[n] != 0 && limb[n] != 0) {
        limb[n] = 0;
        panel[n] = 1;
        grow.push_back(n);
      }
    }
  }
  std::vector<uint8_t> near(count, 0);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint8_t side = 0;
      for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) side = std::max(side, votes.panelSide[at(x + dx, y + dy)]);
      }
      near[size_t(y) * size_t(w) + size_t(x)] = side;
    }
  }
  // A piece of the texture that is all panel goes whole: the texels triangles cover, side by side,
  // bounded by the red line and the gaps between pieces, holding some where the panel surely is, and
  // of the panel's shades (warm, 36 degrees and up) for 80% of their warm texels. Echoes shades the
  // panel's borders a deeper yellow (36 to 37 degrees, as some of a plate's orange), which the
  // texel-by-texel growth went round: purple along the bottom of the inner thigh's piece and by the
  // line down the leg (the user's circles, after test 36's first version). A piece holding a plate
  // too, beside a strip of the panel, keeps the growth's texel-by-texel line.
  {
    std::vector<int32_t> piece(count, -1);
    std::vector<size_t> stack;
    int32_t pieces = 0;
    for (size_t start = 0; start < count; ++start) {
      if (piece[start] >= 0 || votes.limb[start] + votes.other[start] == 0 || red[start] != 0) continue;
      const int32_t id = pieces++;
      piece[start] = id;
      stack.push_back(start);
      size_t warm = 0, shaded = 0;
      bool sure = false;
      std::vector<size_t> members;
      while (!stack.empty()) {
        const size_t k = stack.back();
        stack.pop_back();
        members.push_back(k);
        double hue, sat, val;
        RgbToHsv(&rgba[k * 4], hue, sat, val);
        if (sat > 0.25 && val > 0.25 && (hue < 70.0 || hue >= 330.0)) {
          ++warm;
          shaded += hue >= 36.0 && hue < 70.0 ? 1 : 0;
        }
        sure = sure || votes.panelSide[k] == 2;
        const int x = int(k % size_t(w)), y = int(k / size_t(w));
        for (const auto& [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
          const size_t n = at(x + dx, y + dy);
          if (piece[n] < 0 && votes.limb[n] + votes.other[n] != 0 && red[n] == 0) {
            piece[n] = id;
            stack.push_back(n);
          }
        }
      }
      if (!sure || warm < 20 || shaded * 5 < warm * 4) continue;
      for (const size_t k : members) {
        if (limb[k] != 0 && near[k] != 0) {
          limb[k] = 0;
          panel[k] = 1;
        }
      }
    }
  }
  // What the yellow leaves round the lines drawn on the panel (the lines themselves at its edge, the
  // darker orange either side of them where the line blends into the yellow, a shade) goes with the
  // panel where it surrounds it more than a plate does: left a limb, it came out as purple bits along
  // the curve round the back of the inner thigh and the line down the leg in Gravity's colours,
  // darker in Phazon's (the user's circles, after test 36's first version). Not the bright red line
  // between the panel and a plate, which stays the plate's; a plate's own lines and shades, among its
  // orange, stay with it.
  for (int pass = 0; pass < 3; ++pass) {
    std::vector<size_t> take;
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const size_t k = size_t(y) * size_t(w) + size_t(x);
        if (limb[k] == 0 || near[k] == 0 || tone[k] > 0 || red[k] != 0) continue;
        int panelNear = 0, plateNear = 0;
        for (int dy = -2; dy <= 2; ++dy) {
          for (int dx = -2; dx <= 2; ++dx) {
            const size_t n = at(x + dx, y + dy);
            if (panel[n] != 0) {
              ++panelNear;
            } else if (limb[n] != 0 && dark[n] == 0 && tone[n] != 0) {
              ++plateNear;
            }
          }
        }
        if (panelNear > plateNear) take.push_back(k);
      }
    }
    if (take.empty()) break;
    for (const size_t k : take) {
      limb[k] = 0;
      panel[k] = 1;
    }
  }
}

}  // namespace

// Per texel, 1 where limbs voted more than the rest; texels no triangle covers take their
// neighbours' (two texels round), so seams and the encoder's blocks stay on one side.
std::vector<uint8_t> LimbTexels(const TexelVotes& votes) {
  const int w = votes.width, h = votes.height;
  std::vector<uint8_t> out(size_t(w) * h, 0);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t k = size_t(y) * w + x;
      int limb = votes.limb[k], other = votes.other[k];
      if (limb + other == 0) {
        for (int dy = -2; dy <= 2; ++dy) {
          for (int dx = -2; dx <= 2; ++dx) {
            const size_t n = size_t(((y + dy) % h + h) % h) * w + size_t(((x + dx) % w + w) % w);
            limb += votes.limb[n];
            other += votes.other[n];
          }
        }
      }
      out[k] = limb > other ? 1 : 0;
    }
  }
  return out;
}

bool RecolourVaria(int worn, std::vector<uint8_t>& rgba, const std::vector<uint8_t>& limb) {
  bool changed = false;
  for (size_t k = 0; k * 4 + 3 < rgba.size(); ++k) {
    uint8_t* c = &rgba[k * 4];
    double h, s, v;
    RgbToHsv(c, h, s, v);
    const bool warm = (h < 70.0 || h >= 330.0) && s > 0.18 && v > 0.10;
    const bool green = h >= 70.0 && h < 170.0 && s > 0.25 && v > 0.15;
    const bool onLimb = k < limb.size() && limb[k] != 0;
    uint8_t next[3] = {c[0], c[1], c[2]};
    if (worn == kWornGravity) {
      // Prime's Gravity Suit over its Varia: purple limbs (its median 246.4 deg, saturation and
      // value against the Varia's orange 0.356 / 0.866, 0.482 / 0.969), cyan lights and visor.
      if (green) {
        HsvToRgb(h + 75.0, s, v, next);
      } else if (warm && onLimb) {
        const double d = h >= 180.0 ? h - 360.0 : h;
        HsvToRgb(246.4 + 0.35 * (d - 33.7), s * 0.41, v * 0.50, next);
      }
    } else if (worn == kWornPhazon) {
      // Prime's Phazon Suit: near-black armour, the torso and helmet a dark grey; red lights and visor.
      if (green) {
        HsvToRgb(12.0, std::max(s, 0.85), v, next);
      } else if (warm) {
        HsvToRgb(h, s * 0.15, v * (onLimb ? 0.16 : 0.30), next);
      } else {
        HsvToRgb(h, s, v * 0.6, next);
      }
    }
    if (next[0] != c[0] || next[1] != c[1] || next[2] != c[2]) {
      c[0] = next[0];
      c[1] = next[1];
      c[2] = next[2];
      changed = true;
    }
  }
  return changed;
}

bool DecodeCmpr(const std::vector<uint8_t>& txtr, int& width, int& height, std::vector<uint8_t>& rgba) {
  Reader r{txtr.data(), txtr.size()};
  if (r.U32(0) != 10) return false;
  width = r.U16(4);
  height = r.U16(6);
  if (r.bad || width <= 0 || height <= 0) return false;
  rgba.assign(size_t(width) * height * 4, 0);
  const auto expand = [](uint16_t c, uint8_t* out) {
    out[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
    out[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    out[2] = uint8_t((c & 31) * 255 / 31);
    out[3] = 255;
  };
  size_t at = 12;
  for (int ty = 0; ty < height; ty += 8) {
    for (int tx = 0; tx < width; tx += 8) {
      for (int sub = 0; sub < 4; ++sub) {
        const uint16_t c0 = r.U16(at), c1 = r.U16(at + 2);
        const uint32_t bits = r.U32(at + 4);
        at += 8;
        if (r.bad) return false;
        uint8_t pal[4][4];
        expand(c0, pal[0]);
        expand(c1, pal[1]);
        for (int i = 0; i < 4; ++i) {
          if (c0 > c1) {
            pal[2][i] = uint8_t((2 * pal[0][i] + pal[1][i]) / 3);
            pal[3][i] = uint8_t((pal[0][i] + 2 * pal[1][i]) / 3);
          } else {
            pal[2][i] = uint8_t((pal[0][i] + pal[1][i]) / 2);
            pal[3][i] = 0;
          }
        }
        if (c0 <= c1) pal[2][3] = 255;
        const int bx = tx + (sub & 1) * 4, by = ty + (sub >> 1) * 4;
        for (int y = 0; y < 4; ++y) {
          for (int x = 0; x < 4; ++x) {
            const int px = bx + x, py = by + y;
            const uint32_t index = (bits >> (30 - 2 * (y * 4 + x))) & 3;
            if (px < width && py < height) std::memcpy(&rgba[(size_t(py) * width + px) * 4], pal[index], 4);
          }
        }
      }
    }
  }
  return true;
}

std::vector<uint8_t> EncodeCmpr(int width, int height, const std::vector<uint8_t>& rgba, int mips) {
  std::vector<uint8_t> out;
  Put32(out, 10);
  Put16(out, uint16_t(width));
  Put16(out, uint16_t(height));
  Put32(out, uint32_t(std::max(mips, 1)));
  std::vector<uint8_t> level = rgba;
  int w = width, h = height;
  const auto pack = [](const uint8_t* c) {
    return uint16_t(((c[0] * 31 + 127) / 255) << 11 | ((c[1] * 63 + 127) / 255) << 5 | ((c[2] * 31 + 127) / 255));
  };
  for (int m = 0; m < std::max(mips, 1); ++m) {
    for (int ty = 0; ty < h; ty += 8) {
      for (int tx = 0; tx < w; tx += 8) {
        for (int sub = 0; sub < 4; ++sub) {
          const int bx = tx + (sub & 1) * 4, by = ty + (sub >> 1) * 4;
          uint8_t block[16][4];
          bool cut = false;
          for (int i = 0; i < 16; ++i) {
            const int px = std::min(bx + (i & 3), w - 1), py = std::min(by + (i >> 2), h - 1);
            std::memcpy(block[i], &level[(size_t(py) * w + px) * 4], 4);
            cut = cut || block[i][3] < 128;
          }
          // The ends: the opaque texels' darkest and brightest by luma, then each texel the nearest
          // of the palette (three colours and transparent when the block has cut-out texels).
          int lo = -1, hi = -1;
          int loLuma = 1 << 30, hiLuma = -1;
          for (int i = 0; i < 16; ++i) {
            if (block[i][3] < 128) continue;
            const int luma = block[i][0] * 299 + block[i][1] * 587 + block[i][2] * 114;
            if (luma < loLuma) loLuma = luma, lo = i;
            if (luma > hiLuma) hiLuma = luma, hi = i;
          }
          uint16_t c0 = 0, c1 = 0;
          if (lo >= 0) {
            c0 = pack(block[hi]);
            c1 = pack(block[lo]);
          }
          // Four-colour mode needs c0 > c1, three-colour (with transparent) c0 <= c1.
          if (cut ? c0 > c1 : c0 < c1) std::swap(c0, c1);
          if (!cut && c0 == c1) {
            if (c0 > 0) {
              c1 = uint16_t(c0 - 1);
            } else {
              c0 = 1;
            }
          }
          uint8_t pal[4][4];
          const auto expand = [](uint16_t c, uint8_t* o) {
            o[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
            o[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
            o[2] = uint8_t((c & 31) * 255 / 31);
            o[3] = 255;
          };
          expand(c0, pal[0]);
          expand(c1, pal[1]);
          for (int i = 0; i < 3; ++i) {
            if (c0 > c1) {
              pal[2][i] = uint8_t((2 * pal[0][i] + pal[1][i]) / 3);
              pal[3][i] = uint8_t((pal[0][i] + 2 * pal[1][i]) / 3);
            } else {
              pal[2][i] = uint8_t((pal[0][i] + pal[1][i]) / 2);
              pal[3][i] = 0;
            }
          }
          uint32_t bits = 0;
          for (int i = 0; i < 16; ++i) {
            uint32_t index = 0;
            if (cut && block[i][3] < 128) {
              index = 3;
            } else {
              int best = 1 << 30;
              for (uint32_t p = 0; p < (c0 > c1 ? 4u : 3u); ++p) {
                const int dr = block[i][0] - pal[p][0], dg = block[i][1] - pal[p][1], db = block[i][2] - pal[p][2];
                const int d = dr * dr * 3 + dg * dg * 6 + db * db;
                if (d < best) best = d, index = p;
              }
            }
            bits |= index << (30 - 2 * i);
          }
          Put16(out, c0);
          Put16(out, c1);
          Put32(out, bits);
        }
      }
    }
    if (w == 1 && h == 1) break;
    // The next level: each 2x2 averaged (the cut-out kept where most of them are).
    const int nw = std::max(w / 2, 1), nh = std::max(h / 2, 1);
    std::vector<uint8_t> next(size_t(nw) * nh * 4);
    for (int y = 0; y < nh; ++y) {
      for (int x = 0; x < nw; ++x) {
        int sum[4] = {0, 0, 0, 0}, n = 0;
        for (int dy = 0; dy < 2; ++dy) {
          for (int dx = 0; dx < 2; ++dx) {
            const int sx = std::min(x * 2 + dx, w - 1), sy = std::min(y * 2 + dy, h - 1);
            const uint8_t* c = &level[(size_t(sy) * w + sx) * 4];
            for (int i = 0; i < 4; ++i) sum[i] += c[i];
            ++n;
          }
        }
        for (int i = 0; i < 4; ++i) next[(size_t(y) * nw + x) * 4 + i] = uint8_t(sum[i] / n);
      }
    }
    level = std::move(next);
    w = nw;
    h = nh;
  }
  return out;
}

bool ReadCmdl(const std::vector<uint8_t>& in, PrimeCmdl& model, std::string* error) {
  Reader r{in.data(), in.size()};
  if (r.U32(0) != 0xDEADBABE || r.U32(4) != 2) {
    return Fail(error, "not a Prime CMDL");
  }
  const uint32_t flags = r.U32(8);
  const uint32_t nsec = r.U32(0x24);
  const uint32_t nms = r.U32(0x28);
  if (r.bad || nsec > in.size() / 4) {
    return Fail(error, "CMDL header cut short");
  }
  std::vector<std::pair<size_t, size_t>> sections(nsec);
  size_t at = (0x2C + size_t(nsec) * 4 + 31) & ~size_t(31);
  for (uint32_t i = 0; i < nsec; ++i) {
    const size_t length = r.U32(0x2C + i * 4);
    if (!r.Has(at, length)) {
      return Fail(error, "CMDL section " + std::to_string(i) + " cut short");
    }
    sections[i] = {at, length};
    at += length;
  }
  const uint32_t arrays = (flags & 0x4) ? 5 : 4;
  const size_t surfaceHeader = size_t(nms) + arrays;
  if (surfaceHeader >= nsec) {
    return Fail(error, "CMDL has no surfaces");
  }
  const uint32_t nsurf = r.U32(sections[surfaceHeader].first);
  if (surfaceHeader + 1 + size_t(nsurf) != nsec) {
    return Fail(error, "CMDL section count does not match its surfaces");
  }
  const auto take = [&in](const std::pair<size_t, size_t>& s) {
    return std::vector<uint8_t>(in.begin() + std::ptrdiff_t(s.first), in.begin() + std::ptrdiff_t(s.first + s.second));
  };
  model = PrimeCmdl{};
  model.header.assign(in.begin(), in.begin() + 0x2C);
  for (uint32_t i = 0; i < nms; ++i) model.materialSets.push_back(take(sections[i]));
  for (uint32_t i = 0; i < arrays; ++i) model.arrays.push_back(take(sections[nms + i]));
  for (uint32_t i = 0; i < nsurf; ++i) model.surfaces.push_back(take(sections[surfaceHeader + 1 + i]));
  return true;
}

// Each surface's lists rebuilt as triangle lists of the triangles `keep` passes (by their position
// indices), wound as drawn (a strip's odd triangles turned back); a surface left with none goes.
bool KeepTriangles(PrimeCmdl& model, const std::function<bool(uint32_t, uint32_t, uint32_t)>& keep,
                   std::string* error) {
  MaterialSet set;
  if (model.materialSets.empty() || !ParseSet(model.materialSets[0], set, error)) {
    return Fail(error, "model has no material set");
  }
  std::vector<std::vector<uint8_t>> kept;
  for (auto& surface : model.surfaces) {
    uint32_t vaf;
    if (!SurfaceFlags(surface, set, vaf, error)) return false;
    if (vaf >> 24) return Fail(error, "matrix indices in a display list");
    Reader r{surface.data(), surface.size()};
    const uint32_t sizeWord = r.U32(0x10);
    const size_t start = (0x2C + size_t(r.U32(0x1C)) + 31) & ~size_t(31);
    if (r.bad || start > surface.size()) return Fail(error, "surface header cut short");
    size_t vertexSize = 0;
    for (int i = 0; i < 11; ++i) {
      const uint32_t mode = (vaf >> (2 * i)) & 3;
      if (mode == 1) return Fail(error, "direct vertex attribute");
      vertexSize += mode == 0 ? 0 : mode - 1;
    }
    const uint32_t positionMode = vaf & 3;  // the position comes first
    if (positionMode == 0) return Fail(error, "vertex without a position");
    std::map<uint8_t, std::vector<uint8_t>> lists;  // kept triangles' vertex bytes by vertex format
    size_t at = start;
    while (at < surface.size() && surface[at] != 0) {
      const uint8_t command = surface[at];
      const uint16_t count = r.U16(at + 1);
      if (r.bad || !r.Has(at + 3, size_t(count) * vertexSize)) return Fail(error, "display list cut short");
      const uint8_t* verts = surface.data() + at + 3;
      at += 3 + size_t(count) * vertexSize;
      const auto position = [&](int v) -> uint32_t {
        const uint8_t* p = verts + size_t(v) * vertexSize;
        return positionMode == 2 ? p[0] : uint32_t(p[0] << 8 | p[1]);
      };
      const auto add = [&](int a, int b, int c) {
        if (!keep(position(a), position(b), position(c))) return;
        auto& list = lists[command & 7];
        for (const int v : {a, b, c}) Append(list, verts + size_t(v) * vertexSize, vertexSize);
      };
      switch (command & 0xF8) {
      case 0x90:
        for (int i = 0; i + 2 < count; i += 3) add(i, i + 1, i + 2);
        break;
      case 0x98:
        for (int i = 0; i + 2 < count; ++i) {
          if (i % 2 == 0) {
            add(i, i + 1, i + 2);
          } else {
            add(i + 1, i, i + 2);
          }
        }
        break;
      case 0xA0:
        for (int i = 1; i + 1 < count; ++i) add(0, i, i + 1);
        break;
      default:
        return Fail(error, "primitive " + std::to_string(command & 0xF8) + " not supported here");
      }
    }
    std::vector<uint8_t> list;
    for (const auto& [format, bytes] : lists) {
      const size_t triangles = bytes.size() / (vertexSize * 3);
      for (size_t t = 0; t < triangles; t += 21845) {  // at most 65535 vertices a primitive
        const size_t n = std::min<size_t>(21845, triangles - t);
        list.push_back(uint8_t(0x90 | format));
        Put16(list, uint16_t(n * 3));
        Append(list, bytes.data() + t * 3 * vertexSize, n * 3 * vertexSize);
      }
    }
    if (list.empty()) continue;
    list.push_back(0);
    Pad32(list);
    surface.resize(start);
    Append(surface, list.data(), list.size());
    Set32(surface, 0x10, (sizeWord & 0xFFFF0000) | uint32_t(list.size()));
    kept.push_back(std::move(surface));
  }
  model.surfaces = std::move(kept);
  return true;
}

// A skinned model's positions and normals cut to the ones its lists draw, renumbered in order, and
// its skin's runs cut to match: CSkinRules skins every vertex each frame. Positions and normals go
// together (a skinned list indexes both alike; anything else is left as it is).
bool CompactSkinned(PrimeCmdl& model, std::vector<uint8_t>& skin, std::string* error) {
  MaterialSet set;
  if (model.materialSets.empty() || model.arrays.size() < 2 || !ParseSet(model.materialSets[0], set, error)) {
    return Fail(error, "model has no material set");
  }
  Reader h{model.header.data(), model.header.size()};
  if (h.U32(8) & 0x2) return Fail(error, "short normals");
  const size_t vertices = model.arrays[0].size() / 12;
  std::vector<int32_t> remap(vertices, -1);
  bool parallel = true;
  for (auto surface : model.surfaces) {
    uint32_t vaf;
    if (!SurfaceFlags(surface, set, vaf, error)) return false;
    uint32_t position = 0;
    if (!RewriteList(
            surface, vaf, false, [](uint8_t&) { return true; },
            [&](int attr, uint32_t& value) {
              if (attr == 0) {
                position = value;
                if (value >= vertices) return false;
                remap[value] = 0;
              } else if (attr == 1 && value != position) {
                parallel = false;
              }
              return true;
            },
            error)) {
      return false;
    }
  }
  if (!parallel) return true;
  int32_t next = 0;
  for (int32_t& slot : remap) {
    if (slot == 0) slot = next++;
  }
  // The skin's per-vertex weights (as each group's bytes), kept vertices' runs merged again.
  Reader r{skin.data(), skin.size()};
  const uint32_t groups = r.U32(0);
  std::vector<std::pair<size_t, size_t>> weightsOf;  // per vertex: its group's weight bytes
  size_t at = 4;
  for (uint32_t g = 0; g < groups; ++g) {
    const uint32_t n = r.U32(at);
    if (r.bad || !r.Has(at + 4, size_t(n) * 8 + 4)) return Fail(error, "CSKR group cut short");
    const uint32_t count = r.U32(at + 4 + size_t(n) * 8);
    weightsOf.insert(weightsOf.end(), count, {at, 4 + size_t(n) * 8});
    at += 8 + size_t(n) * 8;
  }
  if (weightsOf.size() < size_t(next)) weightsOf.resize(vertices, weightsOf.empty() ? std::pair<size_t, size_t>{0, 0} : weightsOf.back());
  std::vector<uint8_t> out;
  Put32(out, 0);
  uint32_t runs = 0, covered = 0;
  std::pair<size_t, size_t> current{0, 0};
  uint32_t length = 0;
  const auto flush = [&]() {
    if (length == 0) return;
    Append(out, skin.data() + current.first, current.second);
    Put32(out, length);
    ++runs;
    covered += length;
    length = 0;
  };
  for (size_t v = 0; v < vertices && v < weightsOf.size(); ++v) {
    if (remap[v] < 0) continue;
    const auto& w = weightsOf[v];
    const bool same = length != 0 && w.second == current.second &&
                      std::equal(skin.begin() + std::ptrdiff_t(w.first), skin.begin() + std::ptrdiff_t(w.first + w.second),
                                 skin.begin() + std::ptrdiff_t(current.first));
    if (!same) {
      flush();
      current = w;
    }
    ++length;
  }
  flush();
  Set32(out, 0, runs);
  Put32(out, 0xFFFFFFFF);
  Put32(out, covered);
  Put32(out, 0xFFFFFFFF);
  Put32(out, covered);
  if (covered != uint32_t(next)) return Fail(error, "the skin does not cover the drawn vertices");
  for (auto& surface : model.surfaces) {
    uint32_t vaf;
    SurfaceFlags(surface, set, vaf, error);
    if (!RewriteList(
            surface, vaf, false, [](uint8_t&) { return true; },
            [&remap](int attr, uint32_t& value) {
              if (attr == 0 || attr == 1) value = uint32_t(remap[value]);
              return true;
            },
            error)) {
      return false;
    }
  }
  for (int k = 0; k < 2; ++k) {
    std::vector<uint8_t> kept;
    for (size_t v = 0; v < vertices; ++v) {
      if (remap[v] >= 0) Append(kept, model.arrays[k].data() + v * 12, 12);
    }
    Pad32(kept);
    model.arrays[k] = std::move(kept);
  }
  skin = std::move(out);
  return true;
}

// A Prime CSKR's groups as bytes (without the count and the footer), their count and the vertices
// they cover.
bool SkinGroups(const std::vector<uint8_t>& cskr, std::vector<uint8_t>& bytes, uint32_t& count, uint32_t& vertices,
                std::string* error) {
  Reader r{cskr.data(), cskr.size()};
  count = r.U32(0);
  if (r.bad || count > cskr.size() / 8) return Fail(error, "CSKR cut short");
  size_t at = 4;
  vertices = 0;
  for (uint32_t g = 0; g < count; ++g) {
    const uint32_t n = r.U32(at);
    if (r.bad || !r.Has(at + 4, size_t(n) * 8 + 4)) return Fail(error, "CSKR group cut short");
    vertices += r.U32(at + 4 + size_t(n) * 8);
    at += 8 + size_t(n) * 8;
  }
  bytes.assign(cskr.begin() + 4, cskr.begin() + std::ptrdiff_t(at));
  return true;
}

namespace {

// The texture ids of a material's slots, from its set's list.
bool MaterialTextureIds(const MaterialSet& set, size_t material, std::vector<uint32_t>& ids, std::string* error) {
  const std::vector<uint8_t>& m = set.materials[material];
  Reader r{m.data(), m.size()};
  const uint32_t n = r.U32(4);
  if (r.bad || !r.Has(8, size_t(n) * 4)) return Fail(error, "material " + std::to_string(material) + " cut short");
  ids.clear();
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t index = r.U32(8 + size_t(i) * 4);
    if (index >= set.textures.size()) {
      return Fail(error, "material " + std::to_string(material) + " has a texture out of range");
    }
    ids.push_back(set.textures[index]);
  }
  return true;
}

// Prime's original look of a beam (Ice, Wave or Plasma) on Echoes' cannon (kOriginalSets): Echoes'
// set of that beam (`echoes`, converted) with Prime's set of it (`prime`) in the two materials Echoes
// built on Prime's, its material 0 from Prime's 0 and its 3 from Prime's 2 (the beam's scrolling
// textures, and Prime's scrolling itself where the two draw alike), and the Power set's body and
// shine in material 2, as Prime's body is the same for every beam. `note` gets what stayed Echoes'.
bool OriginalGunSet(const std::vector<uint8_t>& echoes, const std::vector<uint8_t>& power,
                    const std::vector<uint8_t>& prime, std::vector<uint8_t>& out, std::string& note,
                    std::string* error) {
  MaterialSet set, powerSet, primeSet;
  if (!ParseSet(echoes, set, error) || !ParseSet(power, powerSet, error) || !ParseSet(prime, primeSet, error)) {
    return false;
  }
  if (set.materials.size() < 4 || powerSet.materials.size() != set.materials.size() ||
      primeSet.materials.size() < 3) {
    return Fail(error, "its materials are not laid out as Prime's");
  }
  std::vector<std::vector<uint32_t>> ids(set.materials.size());
  for (size_t m = 0; m < set.materials.size(); ++m) {
    if (!MaterialTextureIds(set, m, ids[m], error)) return false;
  }
  struct Take {
    size_t to, from;
  };
  for (const Take take : {Take{0, 0}, Take{3, 2}}) {
    std::vector<uint32_t> primeIds;
    if (!MaterialTextureIds(primeSet, take.from, primeIds, error)) return false;
    if (primeIds.size() != ids[take.to].size()) {
      return Fail(error, "Prime's material " + std::to_string(take.from) + " has " + std::to_string(primeIds.size()) +
                             " textures, Echoes' " + std::to_string(take.to) + " " +
                             std::to_string(ids[take.to].size()));
    }
    ids[take.to] = primeIds;
    // The rest of Prime's material (blend, TEV stages, texgens, UV animations) when it draws the
    // same vertices: the same vertex flags, and the flags its layout follows (konst colours, an
    // indirect texture). The header (flags, slots, vertex flags, group) stays Echoes'.
    std::vector<uint8_t>& mine = set.materials[take.to];
    const std::vector<uint8_t>& theirs = primeSet.materials[take.from];
    const size_t head = 16 + primeIds.size() * 4;
    Reader a{mine.data(), mine.size()}, b{theirs.data(), theirs.size()};
    const uint32_t layout = 0x408;
    if (a.Has(0, head) && b.Has(0, head) && ((a.U32(0) ^ b.U32(0)) & layout) == 0 &&
        ((a.U32(head - 8) ^ b.U32(head - 8)) & 0x00FFFFFF) == 0) {
      mine.resize(head);
      mine.insert(mine.end(), theirs.begin() + std::ptrdiff_t(head), theirs.end());
    } else {
      note += " material " + std::to_string(take.to) + " keeps Echoes' drawing;";
    }
  }
  std::vector<uint32_t> powerIds;
  if (!MaterialTextureIds(powerSet, 2, powerIds, error)) return false;
  if (powerIds.size() != ids[2].size()) return Fail(error, "the Power set's material 2 is not its own");
  ids[2] = powerIds;
  // The set's own list: the textures its materials use, so it loads none of the Echoes beam's.
  set.textures.clear();
  for (size_t m = 0; m < set.materials.size(); ++m) {
    for (size_t i = 0; i < ids[m].size(); ++i) {
      auto it = std::find(set.textures.begin(), set.textures.end(), ids[m][i]);
      if (it == set.textures.end()) it = set.textures.insert(set.textures.end(), ids[m][i]);
      Set32(set.materials[m], 8 + i * 4, uint32_t(it - set.textures.begin()));
    }
  }
  out = WriteSet(set);
  return true;
}

}  // namespace

bool SwapHead(PrimeCmdl& body, std::vector<uint8_t>& skin, const PrimeCmdl& donor,
              const std::vector<uint8_t>& donorSkin, const std::vector<uint32_t>& head, std::string* error) {
  std::vector<uint32_t> bodyJoints, donorJoints;
  if (!MainJoints(skin, bodyJoints, error) || !MainJoints(donorSkin, donorJoints, error)) return false;
  if (body.arrays.size() < 2 || donor.arrays.size() < 2) return Fail(error, "model has no arrays");
  const auto onHead = [&head](const std::vector<uint32_t>& joints, uint32_t p) {
    return p < joints.size() && std::find(head.begin(), head.end(), joints[p]) != head.end();
  };
  PrimeCmdl donorHead = donor;
  if (!FloatNormals(body, error) || !FloatNormals(donorHead, error) ||
      !KeepTriangles(
          body,
          [&](uint32_t a, uint32_t b, uint32_t c) {
            return !onHead(bodyJoints, a) && !onHead(bodyJoints, b) && !onHead(bodyJoints, c);
          },
          error) ||
      !KeepTriangles(
          donorHead,
          [&](uint32_t a, uint32_t b, uint32_t c) {
            return onHead(donorJoints, a) || onHead(donorJoints, b) || onHead(donorJoints, c);
          },
          error)) {
    return false;
  }
  // The body's vertices as MergeCmdl counts them (whole positions of its padded array): its skin's
  // groups cover the real ones, a group of its own the padding, so the donor's groups land on the
  // donor's vertices. Skinning runs each group over positions and normals alike.
  const size_t bodyVertices = body.arrays[0].size() / 12;
  if (body.arrays[1].size() / 12 != bodyVertices) return Fail(error, "the body's positions and normals differ in count");
  std::vector<uint8_t> bodyGroups, donorGroups;
  uint32_t bodyCount, bodyCovered, donorCount, donorCovered;
  if (!SkinGroups(skin, bodyGroups, bodyCount, bodyCovered, error) ||
      !SkinGroups(donorSkin, donorGroups, donorCount, donorCovered, error)) {
    return false;
  }
  if (bodyCovered > bodyVertices || bodyJoints.empty()) return Fail(error, "the body's skin does not fit it");
  if (!MergeCmdl(body, donorHead, error)) return false;
  const uint32_t pad = uint32_t(bodyVertices - bodyCovered);
  std::vector<uint8_t> out;
  Put32(out, bodyCount + (pad != 0 ? 1 : 0) + donorCount);
  Append(out, bodyGroups.data(), bodyGroups.size());
  if (pad != 0) {
    Put32(out, 1);
    Put32(out, bodyJoints.front());
    Put32(out, 0x3F800000);  // weight 1
    Put32(out, pad);
  }
  Append(out, donorGroups.data(), donorGroups.size());
  const uint32_t total = uint32_t(bodyVertices) + donorCovered;
  Put32(out, 0xFFFFFFFF);
  Put32(out, total);
  Put32(out, 0xFFFFFFFF);
  Put32(out, total);
  skin = std::move(out);
  // Only the vertices the lists draw: the donor's whole body came in for its head.
  return CompactSkinned(body, skin, error);
}

namespace {

// Dark Samus's first-person arm cannon (kDarkCannon; 0.46.0 test 70, the user: "I want the dark
// samus mod to have her arm cannon in first person too"): her right arm's triangles from the shoulder
// joint out (the upper arm, the forearm and the cannon it ends in; her shoulder pad is on a joint of
// its own and stays out), her muzzle locator at the origin and the T-posed arm turned down the gun's
// barrel (its +x to +y, PortFederationMarine::RifleFrame), unskinned. The Federation Marine's
// first-person rifle is made so (port_federation_marine.cpp, BuildRifle), and the gun draws both the
// same way: on its muzzle, in place of the arm cannon and the suit arm.
constexpr double kDarkCannonArmCut = 0.27;  // out from the middle: the shoulder joints sit at 0.295
constexpr const char* kDarkCannonMuzzle = "GUN_Particle_LCTR";

bool BuildDarkCannon(const PrimeCmdl& body, const std::vector<uint8_t>& echoesCskr,
                     const std::vector<EchoesJoint>& joints, PrimeCmdl& out, std::string* error) {
  const EchoesJoint* muzzle = nullptr;
  std::set<uint32_t> arm;
  for (const EchoesJoint& j : joints) {
    if (j.name == kDarkCannonMuzzle) muzzle = &j;
    if (j.name == "R_shoulder") arm.insert(j.id);
  }
  if (muzzle == nullptr || arm.empty()) return Fail(error, "her right shoulder or muzzle locator is missing");
  for (bool grew = true; grew;) {
    grew = false;
    for (const EchoesJoint& j : joints) {
      if (!arm.count(j.id) && arm.count(j.parent)) {
        arm.insert(j.id);
        grew = true;
      }
    }
  }
  std::vector<uint32_t> mains;  // each vertex's heaviest Echoes joint
  if (!MainJoints(echoesCskr, mains, error)) return false;
  out = body;
  if (!FloatNormals(out, error) || out.arrays.size() < 2) return Fail(error, "no positions and normals");
  const size_t count = out.arrays[0].size() / 12;
  std::vector<uint8_t> keep(count, 0);
  {
    Reader p{out.arrays[0].data(), out.arrays[0].size()};
    for (size_t v = 0; v < count && v < mains.size(); ++v) {
      keep[v] = arm.count(mains[v]) != 0 && p.F32(v * 12) >= kDarkCannonArmCut ? 1 : 0;
    }
  }
  if (!KeepTriangles(
          out,
          [&keep](uint32_t a, uint32_t b, uint32_t c) {
            return a < keep.size() && b < keep.size() && c < keep.size() && keep[a] && keep[b] && keep[c];
          },
          error)) {
    return false;
  }
  if (out.surfaces.empty()) return Fail(error, "no right arm triangles");
  const auto setF = [](std::vector<uint8_t>& d, size_t at, double value) {
    const float f = float(value);
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    Set32(d, at, bits);
  };
  for (size_t v = 0; v < count; ++v) {
    Reader p{out.arrays[0].data(), out.arrays[0].size()};
    Reader n{out.arrays[1].data(), out.arrays[1].size()};
    const double x = double(p.F32(v * 12)) - muzzle->position[0], y = double(p.F32(v * 12 + 4)) - muzzle->position[1],
                 z = double(p.F32(v * 12 + 8)) - muzzle->position[2];
    const double nx = n.F32(v * 12), ny = n.F32(v * 12 + 4), nz = n.F32(v * 12 + 8);
    setF(out.arrays[0], v * 12, -y);
    setF(out.arrays[0], v * 12 + 4, x);
    setF(out.arrays[0], v * 12 + 8, z);
    setF(out.arrays[1], v * 12, -ny);
    setF(out.arrays[1], v * 12 + 4, nx);
    setF(out.arrays[1], v * 12 + 8, nz);
  }
  // Unskinned: one group over every vertex, which CompactSkinned cuts to the drawn ones.
  std::vector<uint8_t> skin;
  Put32(skin, 1);
  Put32(skin, 1);
  Put32(skin, 0);
  PutF32(skin, 1.f);
  Put32(skin, uint32_t(count));
  Put32(skin, 0xFFFFFFFF);
  Put32(skin, uint32_t(count));
  Put32(skin, 0xFFFFFFFF);
  Put32(skin, uint32_t(count));
  if (!CompactSkinned(out, skin, error)) return false;
  Reader h{out.header.data(), out.header.size()};
  Set32(out.header, 8, h.U32(8) & ~0x1u);
  // Its bounds.
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  Reader p{out.arrays[0].data(), out.arrays[0].size()};
  for (size_t at = 0; at + 12 <= out.arrays[0].size(); at += 12) {
    for (int c = 0; c < 3; ++c) {
      const float value = p.F32(at + size_t(c) * 4);
      lo[c] = std::min(lo[c], value);
      hi[c] = std::max(hi[c], value);
    }
  }
  if (lo[0] <= hi[0]) {
    for (int c = 0; c < 3; ++c) {
      setF(out.header, 0x0C + size_t(c) * 4, lo[c]);
      setF(out.header, 0x18 + size_t(c) * 4, hi[c]);
    }
  }
  return PortFederationMarine::PruneMaterials(out, error);
}

}  // namespace

bool Import(const Io& io, Result& result, std::string& error) {
  const auto log = [&io](const std::string& line) {
    if (io.log) io.log(line);
  };
  const auto cancelled = [&io] { return io.cancelled && io.cancelled(); };
  const auto write = [&io, &result, &error](const std::string& name, const std::vector<uint8_t>& data) {
    if (!io.write(name, data)) {
      error = "Could not write " + name + ".";
      return false;
    }
    result.files.push_back(name);
    return true;
  };

  // Prime's ball ANCS, the offset of its character 0's model and skin (after the name from byte
  // 14), for pointing them at new ones, and that skin.
  const auto primeBall = [&io, &error](const PortDreadSamus::BallTarget& target, std::vector<uint8_t>& ancs,
                                       size_t& at, uint32_t& skin, std::vector<uint8_t>& retailSkin) {
    if (!io.retail(kANCS, target.ancs, ancs)) {
      error = "Prime's ball " + Hex8(target.ancs) + " is missing.";
      return false;
    }
    at = 14;
    while (at < ancs.size() && ancs[at] != 0) ++at;
    ++at;
    Reader a{ancs.data(), ancs.size()};
    skin = a.U32(at + 4);
    if (a.bad || a.U32(at) != target.cmdl || !io.retail(kCSKR, skin, retailSkin)) {
      error = "Prime's ball " + Hex8(target.ancs) + " is not the one this importer knows.";
      return false;
    }
    return true;
  };

  std::map<uint32_t, std::vector<EchoesJoint>> echoesCinfs;
  std::map<uint32_t, PortDread::PrimeSkeleton> primeCinfs;
  std::set<uint32_t> textures;
  // Echoes' Varia as converted, for its recolours: each body kind's model, skin and Prime skeleton,
  // the right arm, and the textures of its ball.
  struct VariaBody {
    PrimeCmdl model;
    std::vector<uint8_t> cskr;
    uint32_t cinf = 0;
  };
  std::map<int, VariaBody> variaBodies;
  PrimeCmdl variaArm;
  std::set<uint32_t> variaBallTextures;
  const auto remapTextures = [](PrimeCmdl& model, const std::map<uint32_t, uint32_t>& remap, std::string* why) {
    for (auto& setBytes : model.materialSets) {
      MaterialSet set;
      if (!ParseSet(setBytes, set, why)) return false;
      for (uint32_t& id : set.textures) {
        const auto found = remap.find(id);
        if (found != remap.end()) id = found->second;
      }
      setBytes = WriteSet(set);
    }
    return true;
  };
  // + Dark Samus's ball, the Varia's recolours, the gun
  const size_t total = std::size(kTargets) + std::size(kBalls) + 1 + std::size(kArms) + 1 + 1;
  for (size_t i = 0; i < std::size(kTargets); ++i) {
    const Target& t = kTargets[i];
    if (cancelled()) {
      error = "Cancelled.";
      return false;
    }
    if (io.progress) io.progress(i, total);
    const std::string what = std::string(t.suit) + " " + t.skeleton;
    std::string why;
    if (!echoesCinfs.count(t.echoesCinf)) {
      std::vector<uint8_t> data;
      if (!io.echoes(kCINF, t.echoesCinf, data)) {
        error = "The Echoes disc has no skeleton " + Hex8(t.echoesCinf) + ": is it Metroid Prime 2: Echoes (USA)?";
        return false;
      }
      if (!ParseCinf(data.data(), data.size(), echoesCinfs[t.echoesCinf], &why)) {
        error = "Echoes skeleton " + Hex8(t.echoesCinf) + ": " + why;
        return false;
      }
    }
    if (!primeCinfs.count(t.cinf)) {
      std::vector<uint8_t> data;
      if (!io.retail(kCINF, t.cinf, data) ||
          !PortDread::ParseCinf(data.data(), data.size(), primeCinfs[t.cinf], &why)) {
        error = "Prime's skeleton " + Hex8(t.cinf) + " is missing or unreadable" + (why.empty() ? "" : ": " + why);
        return false;
      }
    }
    std::vector<uint8_t> cmdl, cskr;
    if (!io.echoes(kCMDL, t.echoesCmdl, cmdl) || !io.echoes(kCSKR, t.echoesCskr, cskr)) {
      error = "The Echoes disc has no " + what + " body (" + Hex8(t.echoesCmdl) + "): is it Metroid Prime 2: "
              "Echoes (USA)?";
      return false;
    }
    std::vector<uint8_t> outCskr;
    std::vector<uint32_t> cmdlTextures;
    std::vector<std::string> folded;
    PrimeCmdl model;
    if (!ConvertCmdl(cmdl, model, cmdlTextures, &why)) {
      error = what + " model " + Hex8(t.echoesCmdl) + ": " + why;
      return false;
    }
    // A pause screen doll's glass writes no depth (SortedWithoutDepth); the Varia's colours and
    // helmet dolls are made from this one.
    if (t.kind == 2 && !SortedWithoutDepth(model, &why)) {
      error = what + " model " + Hex8(t.echoesCmdl) + ": " + why;
      return false;
    }
    if (!ConvertCskr(cskr, echoesCinfs[t.echoesCinf], primeCinfs[t.cinf], outCskr, &folded, &why)) {
      error = what + " skin " + Hex8(t.echoesCskr) + ": " + why;
      return false;
    }
    for (const uint32_t id : {t.cmdl, t.cskr}) {
      if (io.retailId(id)) {
        error = what + ": output id " + Hex8(id) + " is a Prime resource.";
        return false;
      }
    }
    textures.insert(cmdlTextures.begin(), cmdlTextures.end());
    std::string line = "echoes: " + what + " -> " + Hex8(t.cmdl) + ", " + std::to_string(cmdlTextures.size()) +
                       " textures";
    if (!folded.empty()) {
      line += ", folded:";
      for (const std::string& f : folded) line += " " + f;
    }
    log(line);
    if (!write(Hex8(t.cmdl) + ".CMDL", WriteCmdl(model)) || !write(Hex8(t.cskr) + ".CSKR", outCskr)) {
      return false;
    }
    // Dark Samus's first-person arm cannon (version 23), from her cutscene body: her bodies are one
    // mesh. Without it (a skeleton this does not know) she keeps the suit's arm and cannon.
    if (t.worn < 0 && t.cinf == 0x19C53902) {
      PrimeCmdl cannon;
      std::string cannonWhy;
      if (!BuildDarkCannon(model, cskr, echoesCinfs[t.echoesCinf], cannon, &cannonWhy)) {
        log("echoes: Dark Samus first-person arm cannon not made: " + cannonWhy);
      } else if (io.retailId(kDarkCannon)) {
        error = "Dark Samus first-person arm cannon: output id " + Hex8(kDarkCannon) + " is a Prime resource.";
        return false;
      } else {
        if (!write(Hex8(kDarkCannon) + ".CMDL", WriteCmdl(cannon))) {
          return false;
        }
        log("echoes: Dark Samus first-person arm cannon -> " + Hex8(kDarkCannon) + ", " +
            std::to_string(cannon.arrays[0].size() / 12) + " vertices, " + std::to_string(cannon.surfaces.size()) +
            " surfaces");
      }
    }
    if (t.worn == kWornVaria) {
      variaBodies[t.kind] = VariaBody{std::move(model), std::move(outCskr), t.cinf};
    }
    ++result.converted;
  }
  // One look's ball (kBalls' kind of row), its textures through `remap`, fit to every Prime ball ANCS
  // of its spider-ness and written under Wear(worn, ...).
  const auto writeBall = [&](const char* suit, int worn, bool spider, uint32_t echoesCmdl, uint32_t capsId,
                             const std::map<uint32_t, uint32_t>& remap) {
    const std::string what = std::string(suit) +
                             (worn == kWornGravity  ? " (Gravity colours)"
                              : worn == kWornPhazon ? " (Phazon colours)"
                                                    : "") +
                             (spider ? " Spider Ball" : " Morph Ball");
    std::string why;
    PrimeCmdl model;
    std::vector<uint32_t> ballTextures;
    std::vector<uint8_t> cmdl;
    if (!io.echoes(kCMDL, echoesCmdl, cmdl)) {
      error = "The Echoes disc has no " + what + " (" + Hex8(echoesCmdl) + "): is it Metroid Prime 2: Echoes (USA)?";
      return false;
    }
    if (!ConvertCmdl(cmdl, model, ballTextures, &why)) {
      error = what + " model " + Hex8(echoesCmdl) + ": " + why;
      return false;
    }
    if (capsId != 0) {
      PrimeCmdl caps;
      if (!io.echoes(kCMDL, capsId, cmdl)) {
        error = "The Echoes disc has no " + what + " caps (" + Hex8(capsId) + ").";
        return false;
      }
      if (!ConvertCmdl(cmdl, caps, ballTextures, &why) || !MergeCmdl(model, caps, &why)) {
        error = what + " caps " + Hex8(capsId) + ": " + why;
        return false;
      }
    }
    textures.insert(ballTextures.begin(), ballTextures.end());
    if (worn == kWornVaria) variaBallTextures.insert(ballTextures.begin(), ballTextures.end());
    if (!remapTextures(model, remap, &why)) {
      error = what + ": " + why;
      return false;
    }
    const std::vector<uint8_t> low = WriteCmdl(model);
    if (!FloatNormals(model, &why)) {
      error = what + ": " + why;
      return false;
    }
    std::set<uint32_t> ancsDone, lowDone;
    for (const auto& t : PortDreadSamus::kBallTargets) {
      if (t.spider != spider || !ancsDone.insert(t.ancs).second) continue;
      std::vector<uint8_t> ancs, retailSkin, cskr;
      size_t at;
      uint32_t skin, joint;
      if (!primeBall(t, ancs, at, skin, retailSkin)) {
        return false;
      }
      const uint32_t modelId = Wear(worn, t.cmdl);
      const uint32_t skinId = Wear(worn, skin);
      const uint32_t ancsId = Wear(worn, t.ancs);
      const uint32_t lowId = Wear(worn, t.low);
      for (const uint32_t id : {modelId, skinId, ancsId, lowId}) {
        if (id == 0 || io.retailId(id)) {
          error = what + ": output id " + Hex8(id) + " is a Prime resource.";
          return false;
        }
      }
      PrimeCmdl fitted = model;
      if (!MainJoint(retailSkin, joint, &why) || !RigidSkin(fitted, joint, cskr, &why)) {
        error = what + " on " + Hex8(t.ancs) + ": " + why;
        return false;
      }
      Set32(ancs, at, modelId);
      Set32(ancs, at + 4, skinId);
      if (!write(Hex8(modelId) + ".CMDL", WriteCmdl(fitted)) || !write(Hex8(skinId) + ".CSKR", cskr) ||
          !write(Hex8(ancsId) + ".ANCS", ancs) ||
          (lowDone.insert(t.low).second && !write(Hex8(lowId) + ".CMDL", low))) {
        return false;
      }
      log("echoes: " + what + " on " + Hex8(t.ancs) + " -> " + Hex8(modelId) + " (rigid joint " +
          std::to_string(joint) + "), " + std::to_string(ballTextures.size()) + " textures");
      ++result.balls;
    }
    return true;
  };
  const std::map<uint32_t, uint32_t> sameTextures;
  for (size_t i = 0; i < std::size(kBalls); ++i) {
    const Ball& ball = kBalls[i];
    if (cancelled()) {
      error = "Cancelled.";
      return false;
    }
    if (io.progress) io.progress(std::size(kTargets) + i, total);
    if (!writeBall(ball.suit, ball.worn, ball.spider, ball.echoesCmdl, ball.caps, sameTextures)) {
      return false;
    }
  }
  // Dark Samus's ball: skinned in Echoes, but on one joint at full weight, so unskinned it is the
  // static model the other balls are, fit the same way to each Prime ball it stands in for.
  {
    if (cancelled()) {
      error = "Cancelled.";
      return false;
    }
    if (io.progress) io.progress(std::size(kTargets) + std::size(kBalls), total);
    std::string why;
    PrimeCmdl dark;
    std::vector<uint32_t> ballTextures;
    std::vector<uint8_t> cmdl;
    if (!io.echoes(kCMDL, kDarkBall, cmdl)) {
      error = "The Echoes disc has no Dark Samus Morph Ball (" + Hex8(kDarkBall) + "): is it Metroid Prime 2: "
              "Echoes (USA)?";
      return false;
    }
    if (!ConvertCmdl(cmdl, dark, ballTextures, &why)) {
      error = "Dark Samus Morph Ball model " + Hex8(kDarkBall) + ": " + why;
      return false;
    }
    Set32(dark.header, 8, Reader{dark.header.data(), dark.header.size()}.U32(8) & ~0x1u);
    if (!FloatNormals(dark, &why)) {
      error = "Dark Samus Morph Ball: " + why;
      return false;
    }
    const std::vector<uint8_t> low = WriteCmdl(dark);
    std::set<uint32_t> ancsDone, lowDone;  // balls share them across suits
    for (const auto& t : PortDreadSamus::kBallTargets) {
      if (!ancsDone.insert(t.ancs).second) continue;
      std::vector<uint8_t> ancs, retailSkin, cskr;
      size_t at;
      uint32_t skin, joint;
      if (!primeBall(t, ancs, at, skin, retailSkin)) {
        return false;
      }
      const uint32_t modelId = DarkBallId(t.cmdl);
      const uint32_t skinId = DarkBallSkinId(t.cmdl);
      const uint32_t ancsId = DarkBallId(t.ancs);
      const uint32_t lowId = DarkBallId(t.low);
      for (const uint32_t id : {modelId, skinId, ancsId, lowId}) {
        if (id == 0 || io.retailId(id)) {
          error = "Dark Samus Morph Ball: output id " + Hex8(id) + " is a Prime resource.";
          return false;
        }
      }
      PrimeCmdl model = dark;
      if (!MainJoint(retailSkin, joint, &why) || !RigidSkin(model, joint, cskr, &why)) {
        error = "Dark Samus Morph Ball on " + Hex8(t.ancs) + ": " + why;
        return false;
      }
      Set32(ancs, at, modelId);
      Set32(ancs, at + 4, skinId);
      if (!write(Hex8(modelId) + ".CMDL", WriteCmdl(model)) || !write(Hex8(skinId) + ".CSKR", cskr) ||
          !write(Hex8(ancsId) + ".ANCS", ancs) ||
          (lowDone.insert(t.low).second && !write(Hex8(lowId) + ".CMDL", low))) {
        return false;
      }
      log("echoes: Dark Samus Morph Ball on " + Hex8(t.ancs) + " -> " + Hex8(modelId) + " (rigid joint " +
          std::to_string(joint) + ")");
      ++result.balls;
    }
    textures.insert(ballTextures.begin(), ballTextures.end());
    log("echoes: Dark Samus Morph Ball: " + std::to_string(ballTextures.size()) + " textures");
  }
  std::vector<uint8_t> arm;
  for (size_t i = 0; i < std::size(kArms); ++i) {
    const Arm& a = kArms[i];
    if (cancelled()) {
      error = "Cancelled.";
      return false;
    }
    if (io.progress) io.progress(std::size(kTargets) + std::size(kBalls) + 1 + i, total);
    const std::string what = std::string(a.suit) + " arm";
    if (arm.empty() && !io.echoes(kCMDL, kEchoesArm, arm)) {
      error = "The Echoes disc has no arm (" + Hex8(kEchoesArm) + "): is it Metroid Prime 2: Echoes (USA)?";
      return false;
    }
    std::string why;
    PrimeCmdl model;
    std::vector<uint32_t> armTextures;
    if (!ConvertCmdl(arm, model, armTextures, &why) || !KeepMaterialSet(model, a.set, armTextures, &why)) {
      error = what + " " + Hex8(kEchoesArm) + ": " + why;
      return false;
    }
    if (io.retailId(a.cmdl)) {
      error = what + ": output id " + Hex8(a.cmdl) + " is a Prime resource.";
      return false;
    }
    if (!write(Hex8(a.cmdl) + ".CMDL", WriteCmdl(model))) {
      return false;
    }
    textures.insert(armTextures.begin(), armTextures.end());
    log("echoes: " + what + " -> " + Hex8(a.cmdl) + ", " + std::to_string(armTextures.size()) + " textures");
    if (a.worn == kWornVaria) variaArm = std::move(model);
    ++result.arms;
  }
  // Echoes' Varia in Prime's Gravity and Phazon colours (RecolourVaria): each texture its bodies, arm
  // and ball draw, decoded, its texels voted limb or not by the triangles over them (a body's by the
  // joints its vertices weigh most on; the arm's and the ball's all limb), recoloured both ways and
  // written under Wear(worn, id); then the Varia's bodies, arm and balls again over those textures.
  {
    if (cancelled()) {
      error = "Cancelled.";
      return false;
    }
    if (io.progress) io.progress(std::size(kTargets) + std::size(kBalls) + 1 + std::size(kArms), total);
    std::string why;
    struct Map {
      int width = 0, height = 0, mips = 1;
      std::vector<uint8_t> rgba;
      TexelVotes votes;
    };
    std::map<uint32_t, Map> maps;
    std::set<uint32_t> notCmpr;
    const auto use = [&](uint32_t id) -> Map* {
      if (notCmpr.count(id)) return nullptr;
      const auto found = maps.find(id);
      if (found != maps.end()) return &found->second;
      std::vector<uint8_t> data;
      Map map;
      if (!io.echoes(kTXTR, id, data) || !DecodeCmpr(data, map.width, map.height, map.rgba)) {
        notCmpr.insert(id);  // left as it is
        return nullptr;
      }
      map.mips = int(Reader{data.data(), data.size()}.U32(8));
      map.votes.width = map.width;
      map.votes.height = map.height;
      map.votes.limb.assign(size_t(map.width) * map.height, 0);
      map.votes.other = map.votes.limb;
      map.votes.panelSide.assign(size_t(map.width) * map.height, 0);
      return &maps.emplace(id, std::move(map)).first->second;
    };
    for (const auto& [kind, body] : variaBodies) {
      std::vector<uint32_t> joints;
      if (!MainJoints(body.cskr, joints, &why)) {
        error = "Echoes Varia (" + std::to_string(kind) + "): " + why;
        return false;
      }
      std::map<uint32_t, bool> limbJoint;
      std::set<uint32_t> hipJoints;
      for (const auto& j : primeCinfs[body.cinf].joints) {
        limbJoint[j.id] = IsLimbJoint(j.name);
        if (j.name == "L_hip" || j.name == "R_hip") hipJoints.insert(j.id);
      }
      const bool ok = ForEachUvTriangle(
          body.model,
          [&](const std::vector<uint32_t>& ids, const float (&uv)[3][2], const uint32_t (&pos)[3]) {
            int limbs = 0, hips = 0;
            for (const uint32_t p : pos) {
              if (p < joints.size()) {
                const auto found = limbJoint.find(joints[p]);
                limbs += found != limbJoint.end() && found->second ? 1 : 0;
                hips += hipJoints.count(joints[p]) != 0 ? 1 : 0;
              }
            }
            const bool limb = limbs >= 2;
            // The inner thighs' undersuit goes with the torso, the plates with the limbs (KeepUndersuit,
            // texel by texel): Prime's Gravity Suit keeps it yellow between the purple thigh plates, as
            // its abdomen, and its Phazon Suit as dark as its abdomen. Painted as a limb, the inner
            // thighs came out purple and darker than the lower torso (the user, after 0.46.0 test 33).
            // Not where the thigh faces well out (past -0.5): the in-game body's plates have lighter
            // patches there, and there is yellow behind the outer plates, both purple in Prime's. The
            // panel leans out by up to half at its front and wraps round the back of the thigh so
            // too: cut at -0.1, its front went purple (tests 33-34), and cut there facing back (test
            // 35, against the plates' lighter wedges at the back, which the texels' colour now keeps
            // purple), the strip of it beside Echoes' line at the back did (the user's circles).
            // Facing in or across (to -0.1), the panel surely is there; leaning out by up to half,
            // it may be, joined to it (KeepUndersuit).
            double middle = -1.0;
            if (limb && hips >= 2) {
              middle = FacingOf(body.model, pos).middle;
            }
            for (const uint32_t id : ids) {
              Map* map = use(id);
              if (map == nullptr) continue;
              VoteTriangle(map->votes, uv, limb);
              if (middle >= -0.5) MarkPanelSide(map->votes, uv, middle >= -0.1);
            }
          },
          &why);
      if (!ok) {
        error = "Echoes Varia (" + std::to_string(kind) + "): " + why;
        return false;
      }
    }
    if (!variaArm.materialSets.empty() &&
        !ForEachUvTriangle(
            variaArm,
            [&](const std::vector<uint32_t>& ids, const float (&uv)[3][2], const uint32_t (&)[3]) {
              for (const uint32_t id : ids) {
                if (Map* map = use(id)) VoteTriangle(map->votes, uv, true);
              }
            },
            &why)) {
      error = "Echoes Varia arm: " + why;
      return false;
    }
    // The ball's textures are all limb where nothing else drew them.
    for (const uint32_t id : variaBallTextures) {
      if (Map* map = use(id)) {
        bool covered = false;
        for (size_t k = 0; k < map->votes.limb.size() && !covered; ++k) {
          covered = map->votes.limb[k] + map->votes.other[k] != 0;
        }
        if (!covered) std::fill(map->votes.limb.begin(), map->votes.limb.end(), uint16_t(1));
      }
    }
    std::map<uint32_t, uint32_t> remaps[kWornCount];
    for (auto& [id, map] : maps) {
      std::vector<uint8_t> limb = LimbTexels(map.votes);
      KeepUndersuit(map.votes, map.rgba, limb);
      for (const int worn : {int(kWornGravity), int(kWornPhazon)}) {
        std::vector<uint8_t> rgba = map.rgba;
        if (!RecolourVaria(worn, rgba, limb)) continue;
        const uint32_t out = Wear(worn, id);
        if (io.retailId(out)) {
          error = "Recoloured texture " + Hex8(id) + ": output id " + Hex8(out) + " is a Prime resource.";
          return false;
        }
        if (!write(Hex8(out) + ".TXTR", EncodeCmpr(map.width, map.height, rgba, map.mips))) {
          return false;
        }
        remaps[worn][id] = out;
      }
    }
    const SuitResources& varia = kSuitResources[kVariaSuit];
    for (const int worn : {int(kWornGravity), int(kWornPhazon)}) {
      const char* colours = worn == kWornGravity ? "Gravity" : "Phazon";
      for (const auto& [kind, body] : variaBodies) {
        PrimeCmdl model = body.model;
        if (!remapTextures(model, remaps[worn], &why)) {
          error = std::string("Echoes Varia in ") + colours + " colours: " + why;
          return false;
        }
        const uint32_t cmdlId = Wear(worn, varia.cmdl[kind]);
        const uint32_t cskrId = Wear(worn, varia.cskr[kind]);
        if (io.retailId(cmdlId) || io.retailId(cskrId)) {
          error = std::string("Echoes Varia in ") + colours + " colours: output id is a Prime resource.";
          return false;
        }
        if (!write(Hex8(cmdlId) + ".CMDL", WriteCmdl(model)) || !write(Hex8(cskrId) + ".CSKR", body.cskr)) {
          return false;
        }
        ++result.converted;
      }
      if (!variaArm.materialSets.empty()) {
        PrimeCmdl model = variaArm;
        const uint32_t armId = Wear(worn, varia.arm);
        if (!remapTextures(model, remaps[worn], &why) || io.retailId(armId)) {
          error = std::string("Echoes Varia arm in ") + colours + " colours: " + (why.empty() ? "output id taken" : why);
          return false;
        }
        if (!write(Hex8(armId) + ".CMDL", WriteCmdl(model))) {
          return false;
        }
        ++result.arms;
      }
      for (const Ball& ball : kBalls) {
        if (ball.worn != kWornVaria) continue;
        if (!writeBall(ball.suit, worn, ball.spider, ball.echoesCmdl, ball.caps, remaps[worn])) {
          return false;
        }
      }
      log(std::string("echoes: Echoes Varia in ") + colours + " colours: " + std::to_string(remaps[worn].size()) +
          " textures recoloured");
    }
    // Prime's bodies with Echoes' Varia helmet (kLookHelmet): each suit's cutscene, in-game and doll
    // body, its head (Head_1 and the face joints under it) swapped for the Varia's of the suit's
    // colours, under HelmetId of Prime's ids.
    for (int suit = 0; suit < kSuitLookSuits; ++suit) {
      const int worn = WornFor(suit, kLookColours);
      const SuitResources& mine = kSuitResources[suit];
      for (int kind = 0; kind < 3; ++kind) {
        const auto found = variaBodies.find(kind);
        if (found == variaBodies.end()) continue;
        const std::string what = "Prime's suit " + std::to_string(suit) + " body " + Hex8(mine.cmdl[kind]) +
                                 " with Echoes' helmet";
        std::vector<uint8_t> cmdl, cskr;
        PrimeCmdl body;
        if (!io.retail(kCMDL, mine.cmdl[kind], cmdl) || !io.retail(kCSKR, mine.cskr[kind], cskr) ||
            !ReadCmdl(cmdl, body, &why)) {
          error = what + ": Prime's body is missing or unreadable" + (why.empty() ? "" : ": " + why);
          return false;
        }
        PrimeCmdl donor = found->second.model;
        if (worn != kWornVaria && !remapTextures(donor, remaps[worn], &why)) {
          error = what + ": " + why;
          return false;
        }
        std::vector<uint32_t> head;
        const auto& joints = primeCinfs[found->second.cinf].joints;
        for (const auto& j : joints) {
          if (j.name == "Head_1") head.push_back(j.id);
        }
        for (bool grew = !head.empty(); grew;) {
          grew = false;
          for (const auto& j : joints) {
            if (std::find(head.begin(), head.end(), j.id) == head.end() &&
                std::find(head.begin(), head.end(), j.parent) != head.end()) {
              head.push_back(j.id);
              grew = true;
            }
          }
        }
        if (head.empty() || !SwapHead(body, cskr, donor, found->second.cskr, head, &why)) {
          error = what + ": " + (head.empty() ? std::string("no Head_1 joint") : why);
          return false;
        }
        const uint32_t cmdlId = HelmetId(mine.cmdl[kind]);
        const uint32_t cskrId = HelmetId(mine.cskr[kind]);
        if (io.retailId(cmdlId) || io.retailId(cskrId)) {
          error = what + ": output id is a Prime resource.";
          return false;
        }
        if (!write(Hex8(cmdlId) + ".CMDL", WriteCmdl(body)) || !write(Hex8(cskrId) + ".CSKR", cskr)) {
          return false;
        }
        log("echoes: " + what + " -> " + Hex8(cmdlId) + " (" + std::to_string(head.size()) + " head joints)");
        ++result.converted;
      }
    }
  }
  {
    if (io.progress) io.progress(total - 1, total);
    std::vector<uint8_t> data;
    std::string why;
    std::vector<EchoesJoint> echoesCinf;
    PortDread::PrimeSkeleton primeCinf;
    if (!io.echoes(kCINF, kGunCinf, data) || !ParseCinf(data.data(), data.size(), echoesCinf, &why)) {
      error = "The Echoes gun skeleton " + Hex8(kGunCinf) + " is missing or unreadable" +
              (why.empty() ? "" : ": " + why);
      return false;
    }
    if (!io.retail(kCINF, kGunCinf, data) || !PortDread::ParseCinf(data.data(), data.size(), primeCinf, &why)) {
      error = "Prime's gun skeleton " + Hex8(kGunCinf) + " is missing or unreadable" + (why.empty() ? "" : ": " + why);
      return false;
    }
    for (const Gun& gun : kGuns) {
      for (const uint32_t id : {Cannon(gun.cmdl), Cannon(gun.cskr)}) {
        if (io.retailId(id)) {
          error = std::string(gun.what) + ": output id " + Hex8(id) + " is a Prime resource.";
          return false;
        }
      }
      std::vector<uint8_t> cmdl, cskr, outCmdl, outCskr;
      std::vector<uint32_t> gunTextures;
      std::vector<std::string> folded;
      PrimeCmdl model;
      if (!io.echoes(kCMDL, gun.cmdl, cmdl) || !io.echoes(kCSKR, gun.cskr, cskr)) {
        error = "The Echoes disc has no " + std::string(gun.what) + " (" + Hex8(gun.cmdl) +
                "): is it Metroid Prime 2: Echoes (USA)?";
        return false;
      }
      if (!ConvertCmdl(cmdl, model, gunTextures, &why)) {
        error = std::string(gun.what) + " model " + Hex8(gun.cmdl) + ": " + why;
        return false;
      }
      // The gun has a set per beam; the hologram one for all.
      if (model.materialSets.size() != 1 && model.materialSets.size() != std::size(kGunSets)) {
        error = std::string(gun.what) + " model " + Hex8(gun.cmdl) + " has " +
                std::to_string(model.materialSets.size()) + " material sets, not 1 or " +
                std::to_string(std::size(kGunSets)) + ".";
        return false;
      }
      if (model.materialSets.size() == std::size(kGunSets)) {
        std::vector<std::vector<uint8_t>> sets;
        for (const uint32_t set : kGunSets) sets.push_back(model.materialSets[set]);
        model.materialSets = std::move(sets);
        // Then Prime's original Ice, Wave and Plasma looks (kOriginalSets on), from Prime's own gun
        // (the same id), its sets by beam.
        static_assert(kOriginalSets == std::size(kGunSets), "the originals follow the beam sets");
        static const char* const kOriginalOf[kCannonLookBeams] = {"", "Ice", "Wave", "Plasma"};
        std::vector<uint8_t> primeGun;
        PrimeCmdl prime;
        why.clear();
        if (!io.retail(kCMDL, gun.cmdl, primeGun) || !ReadCmdl(primeGun, prime, &why) ||
            prime.materialSets.size() < size_t(kCannonLookBeams)) {
          error = "Prime's " + std::string(gun.what) + " " + Hex8(gun.cmdl) + " is missing or unreadable" +
                  (why.empty() ? "" : ": " + why);
          return false;
        }
        for (int beam = 1; beam < kCannonLookBeams; ++beam) {
          std::vector<uint8_t> set;
          std::string note;
          if (!OriginalGunSet(model.materialSets[beam], model.materialSets[0], prime.materialSets[beam], set, note,
                              &why)) {
            error = std::string(gun.what) + ": the original " + kOriginalOf[beam] + " Beam look: " + why;
            return false;
          }
          model.materialSets.push_back(std::move(set));
          log("echoes: " + std::string(gun.what) + " set " + std::to_string(model.materialSets.size() - 1) +
              ", the original " + kOriginalOf[beam] + " Beam" + note);
        }
      }
      outCmdl = WriteCmdl(model);
      if (!ConvertCskr(cskr, echoesCinf, primeCinf, outCskr, &folded, &why)) {
        error = std::string(gun.what) + " skin " + Hex8(gun.cskr) + ": " + why;
        return false;
      }
      // Prime's gun animations move these joints by name: one without a namesake would freeze.
      if (!folded.empty()) {
        error = std::string(gun.what) + " skin " + Hex8(gun.cskr) + ": joint " + folded.front() +
                " has no namesake in Prime's gun skeleton.";
        return false;
      }
      if (!write(Hex8(Cannon(gun.cmdl)) + ".CMDL", outCmdl) || !write(Hex8(Cannon(gun.cskr)) + ".CSKR", outCskr)) {
        return false;
      }
      textures.insert(gunTextures.begin(), gunTextures.end());
      log("echoes: " + std::string(gun.what) + " -> " + Hex8(Cannon(gun.cmdl)) + ", " +
          std::to_string(gunTextures.size()) + " textures");
    }
    // Echoes' missile anims, each converted once: the shots and the reload are every beam's.
    std::set<uint32_t> missileAnims;
    for (const uint32_t id : kBeamAncs) {
      const uint32_t out = Cannon(id);
      std::vector<uint8_t> ancs;
      if (io.retailId(out)) {
        error = "Gun " + Hex8(id) + ": output id " + Hex8(out) + " is a Prime resource.";
        return false;
      }
      if (!io.retail(kANCS, id, ancs)) {
        error = "Prime's gun " + Hex8(id) + " is missing.";
        return false;
      }
      if (!PatchGunAncs(ancs, &why)) {
        error = "Prime's gun " + Hex8(id) + ": " + why;
        return false;
      }
      GunMissileAnims missiles;
      if (!FindGunMissileAnims(ancs, missiles, &why)) {
        error = "Prime's gun " + Hex8(id) + ": " + why;
        return false;
      }
      if (missiles.found) {
        const uint32_t sources[5] = {missiles.anim[0], missiles.anim[1], missiles.anim[2], missiles.anim[3],
                                     kFirstMissileShot};
        const uint32_t evnts[5] = {missiles.evnt[0], missiles.evnt[1], missiles.evnt[2], missiles.evnt[3],
                                   missiles.evnt[2]};
        for (int i = 0; i < 5; ++i) {
          if (!missileAnims.insert(sources[i]).second) continue;
          const uint32_t animOut = Cannon(sources[i]);
          std::vector<uint8_t> anim, converted;
          if (io.retailId(animOut)) {
            error = "Gun anim " + Hex8(sources[i]) + ": output id " + Hex8(animOut) + " is a Prime resource.";
            return false;
          }
          if (!io.echoes(kANIM, sources[i], anim)) {
            error = "The Echoes disc has no gun anim " + Hex8(sources[i]) + ": is it Metroid Prime 2: Echoes (USA)?";
            return false;
          }
          if (!ConvertGunAnim(anim, echoesCinf, primeCinf, evnts[i], converted, &why)) {
            error = "Echoes gun anim " + Hex8(sources[i]) + ": " + why;
            return false;
          }
          if (!write(Hex8(animOut) + ".ANIM", converted)) {
            return false;
          }
          log("echoes: gun anim " + Hex8(sources[i]) + " -> " + Hex8(animOut) + ", " +
              std::to_string(converted.size()) + " bytes");
        }
        if (!AddGunMissileAnims(ancs, missiles, &why)) {
          error = "Prime's gun " + Hex8(id) + ": " + why;
          return false;
        }
      }
      if (!write(Hex8(out) + ".ANCS", ancs)) {
        return false;
      }
      ++result.guns;
    }
  }
  for (const uint32_t id : textures) {
    if (cancelled()) {
      error = "Cancelled.";
      return false;
    }
    std::vector<uint8_t> data;
    if (!io.echoes(kTXTR, id, data)) {
      error = "The Echoes disc has no texture " + Hex8(id) + ".";
      return false;
    }
    // A texture id Prime also uses would replace Prime's texture everywhere: only when they are
    // the same picture.
    if (io.retailId(id)) {
      std::vector<uint8_t> retail;
      if (!io.retail(kTXTR, id, retail) || retail != data) {
        error = "Echoes texture " + Hex8(id) + " has the id of a different Prime resource.";
        return false;
      }
      continue;
    }
    if (!write(Hex8(id) + ".TXTR", data)) {
      return false;
    }
  }
  if (io.progress) io.progress(total, total);
  log("echoes: " + std::to_string(result.converted) + " bodies and left arms, " + std::to_string(result.balls) +
      " balls, " + std::to_string(result.arms) + " right arms, " + std::to_string(result.guns) + " beam guns, " +
      std::to_string(textures.size()) + " textures");
  return true;
}

}  // namespace PortEchoesSuits
