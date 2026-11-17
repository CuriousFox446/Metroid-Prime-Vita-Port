#pragma once

// PS Vita (0.46.0 test 13): the few OpenSSL calls the NSP reader makes (port_remastered_nsp.cpp), for a
// build without OpenSSL, under OpenSSL's names so the reader's code stays one. AES-128 (FIPS-197) as
// ECB, CTR and XTS decryption, with the behaviour the reader relies on:
// - ECB: whole 16-byte blocks, no padding.
// - CTR: the IV is the counter, counted up as one 128-bit big-endian number; any length; each
//   EVP_DecryptInit_ex starts again from its IV (the reader inits once a chunk).
// - XTS: a 32-byte key (the data key, then the tweak key) and a 16-byte tweak, whole 16-byte blocks
//   (the reader decrypts one 0x200-byte sector an init).
// SHA-256 is not here: EVP_Digest fails (only the Remastered import's BRDF table check uses it, and
// that import is not on the Vita). Checked against OpenSSL 3.5's command line in 0.46.0 test 13.
// Only port_remastered_nsp.cpp includes this, and everything is internal to it.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

namespace PortSoftCrypto {
constexpr uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9,
    0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f,
    0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07,
    0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3,
    0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58,
    0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3,
    0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f,
    0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
    0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac,
    0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a,
    0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70,
    0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42,
    0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

// The inverse S-box, made from kSbox.
struct InvSbox {
  uint8_t v[256];
  constexpr InvSbox() : v{} {
    for (int i = 0; i < 256; ++i) {
      v[kSbox[i]] = uint8_t(i);
    }
  }
};
constexpr InvSbox kInvSbox{};

constexpr uint8_t XTime(uint8_t x) { return uint8_t((x << 1) ^ ((x & 0x80) != 0 ? 0x1b : 0)); }

constexpr uint8_t Mul(uint8_t a, uint8_t b) {
  uint8_t r = 0;
  while (b != 0) {
    if ((b & 1) != 0) {
      r ^= a;
    }
    a = XTime(a);
    b >>= 1;
  }
  return r;
}

// AES-128 with its 11 round keys. The state is column-major: byte c * 4 + r is row r of column c.
struct Aes128 {
  uint8_t rk[176] = {};

  void SetKey(const uint8_t key[16]) {
    constexpr uint8_t kRcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};
    std::memcpy(rk, key, 16);
    for (int i = 4; i < 44; ++i) {
      uint8_t t[4] = {rk[(i - 1) * 4], rk[(i - 1) * 4 + 1], rk[(i - 1) * 4 + 2], rk[(i - 1) * 4 + 3]};
      if (i % 4 == 0) {
        const uint8_t first = t[0];
        t[0] = uint8_t(kSbox[t[1]] ^ kRcon[i / 4 - 1]);
        t[1] = kSbox[t[2]];
        t[2] = kSbox[t[3]];
        t[3] = kSbox[first];
      }
      for (int j = 0; j < 4; ++j) {
        rk[i * 4 + j] = uint8_t(rk[(i - 4) * 4 + j] ^ t[j]);
      }
    }
  }

  void Encrypt(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t s[16];
    for (int i = 0; i < 16; ++i) {
      s[i] = uint8_t(in[i] ^ rk[i]);
    }
    for (int round = 1; round <= 10; ++round) {
      uint8_t t[16];
      // SubBytes and ShiftRows: row r of column c comes from column c + r.
      for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
          t[c * 4 + r] = kSbox[s[((c + r) & 3) * 4 + r]];
        }
      }
      if (round != 10) {
        for (int c = 0; c < 4; ++c) {
          uint8_t* col = t + c * 4;
          const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
          col[0] = uint8_t(XTime(a0) ^ XTime(a1) ^ a1 ^ a2 ^ a3);
          col[1] = uint8_t(a0 ^ XTime(a1) ^ XTime(a2) ^ a2 ^ a3);
          col[2] = uint8_t(a0 ^ a1 ^ XTime(a2) ^ XTime(a3) ^ a3);
          col[3] = uint8_t(XTime(a0) ^ a0 ^ a1 ^ a2 ^ XTime(a3));
        }
      }
      for (int i = 0; i < 16; ++i) {
        s[i] = uint8_t(t[i] ^ rk[round * 16 + i]);
      }
    }
    std::memcpy(out, s, 16);
  }

  void Decrypt(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t s[16];
    for (int i = 0; i < 16; ++i) {
      s[i] = uint8_t(in[i] ^ rk[160 + i]);
    }
    for (int round = 9; round >= 0; --round) {
      uint8_t t[16];
      // InvShiftRows and InvSubBytes: row r of column c goes to column c + r.
      for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
          t[((c + r) & 3) * 4 + r] = kInvSbox.v[s[c * 4 + r]];
        }
      }
      for (int i = 0; i < 16; ++i) {
        t[i] ^= rk[round * 16 + i];
      }
      if (round != 0) {
        for (int c = 0; c < 4; ++c) {
          uint8_t* col = t + c * 4;
          const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
          col[0] = uint8_t(Mul(a0, 14) ^ Mul(a1, 11) ^ Mul(a2, 13) ^ Mul(a3, 9));
          col[1] = uint8_t(Mul(a0, 9) ^ Mul(a1, 14) ^ Mul(a2, 11) ^ Mul(a3, 13));
          col[2] = uint8_t(Mul(a0, 13) ^ Mul(a1, 9) ^ Mul(a2, 14) ^ Mul(a3, 11));
          col[3] = uint8_t(Mul(a0, 11) ^ Mul(a1, 13) ^ Mul(a2, 9) ^ Mul(a3, 14));
        }
      }
      std::memcpy(s, t, 16);
    }
    std::memcpy(out, s, 16);
  }
};

enum class Mode { Ecb, Ctr, Xts };
} // namespace PortSoftCrypto

struct EVP_CIPHER {
  PortSoftCrypto::Mode mode;
};
struct EVP_MD {
  int unused;
};
struct EVP_CIPHER_CTX {
  PortSoftCrypto::Mode mode = PortSoftCrypto::Mode::Ecb;
  PortSoftCrypto::Aes128 key;
  PortSoftCrypto::Aes128 tweakKey; // XTS
  uint8_t iv[16] = {};
};

inline void OPENSSL_cleanse(void* data, size_t size) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) {
    bytes[i] = 0;
  }
}

inline const EVP_CIPHER* EVP_aes_128_ecb() {
  static const EVP_CIPHER cipher{PortSoftCrypto::Mode::Ecb};
  return &cipher;
}
inline const EVP_CIPHER* EVP_aes_128_ctr() {
  static const EVP_CIPHER cipher{PortSoftCrypto::Mode::Ctr};
  return &cipher;
}
inline const EVP_CIPHER* EVP_aes_128_xts() {
  static const EVP_CIPHER cipher{PortSoftCrypto::Mode::Xts};
  return &cipher;
}
inline const EVP_MD* EVP_sha256() {
  static const EVP_MD md{0};
  return &md;
}

inline EVP_CIPHER_CTX* EVP_CIPHER_CTX_new() { return new EVP_CIPHER_CTX(); }

inline void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX* ctx) {
  if (ctx != nullptr) {
    OPENSSL_cleanse(ctx, sizeof(*ctx));
    delete ctx;
  }
}

inline int EVP_CIPHER_CTX_set_padding(EVP_CIPHER_CTX*, int) { return 1; }

inline int EVP_DecryptInit_ex(EVP_CIPHER_CTX* ctx, const EVP_CIPHER* cipher, void*, const uint8_t* key,
                              const uint8_t* iv) {
  if (ctx == nullptr || cipher == nullptr || key == nullptr) {
    return 0;
  }
  ctx->mode = cipher->mode;
  ctx->key.SetKey(key);
  if (cipher->mode == PortSoftCrypto::Mode::Xts) {
    ctx->tweakKey.SetKey(key + 16);
  }
  if (iv != nullptr) {
    std::memcpy(ctx->iv, iv, 16);
  } else {
    std::memset(ctx->iv, 0, 16);
  }
  return 1;
}

inline int EVP_DecryptUpdate(EVP_CIPHER_CTX* ctx, uint8_t* out, int* outLen, const uint8_t* in, int inLen) {
  using PortSoftCrypto::Mode;
  if (ctx == nullptr || out == nullptr || in == nullptr || outLen == nullptr || inLen < 0) {
    return 0;
  }
  const size_t size = size_t(inLen);
  if (ctx->mode == Mode::Ecb) {
    if (size % 16 != 0) {
      return 0;
    }
    for (size_t at = 0; at < size; at += 16) {
      ctx->key.Decrypt(in + at, out + at);
    }
  } else if (ctx->mode == Mode::Ctr) {
    for (size_t at = 0; at < size; at += 16) {
      uint8_t stream[16];
      ctx->key.Encrypt(ctx->iv, stream);
      const size_t n = size - at < 16 ? size - at : 16;
      for (size_t i = 0; i < n; ++i) {
        out[at + i] = uint8_t(in[at + i] ^ stream[i]);
      }
      for (int i = 15; i >= 0 && ++ctx->iv[i] == 0; --i) {
      }
    }
  } else {
    if (size % 16 != 0) {
      return 0;
    }
    uint8_t tweak[16];
    ctx->tweakKey.Encrypt(ctx->iv, tweak);
    for (size_t at = 0; at < size; at += 16) {
      uint8_t block[16];
      for (int i = 0; i < 16; ++i) {
        block[i] = uint8_t(in[at + i] ^ tweak[i]);
      }
      ctx->key.Decrypt(block, block);
      for (int i = 0; i < 16; ++i) {
        out[at + i] = uint8_t(block[i] ^ tweak[i]);
      }
      // The tweak times x in GF(2^128), its bytes little-endian (IEEE 1619).
      uint8_t carry = 0;
      for (int i = 0; i < 16; ++i) {
        const uint8_t next = uint8_t(tweak[i] >> 7);
        tweak[i] = uint8_t((tweak[i] << 1) | carry);
        carry = next;
      }
      if (carry != 0) {
        tweak[0] ^= 0x87;
      }
    }
  }
  *outLen = inLen;
  return 1;
}

// SHA-256 is not here (see the top): the Remastered import's BRDF table check fails on the Vita.
inline int EVP_Digest(const void*, size_t, uint8_t*, unsigned int*, const EVP_MD*, void*) { return 0; }

} // namespace
