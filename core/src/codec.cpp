#include "codec.hpp"

#include <array>
#include <cstring>

#include "miniz.h"

namespace ncx {

// ------------------------------------------------------------------------------ SHA-256
// FIPS 180-4. Small and self-contained so the core has no crypto dependency.
namespace {

constexpr std::array<uint32_t, 64> K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void sha256_block(std::array<uint32_t, 8>& h, const unsigned char* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) | (uint32_t(p[4 * i + 2]) << 8) |
               uint32_t(p[4 * i + 3]);
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

}  // namespace

std::string sha256_hex(std::string_view data) {
    std::array<uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    size_t n = data.size(), i = 0;
    for (; i + 64 <= n; i += 64) sha256_block(h, p + i);
    unsigned char tail[128] = {};
    size_t rem = n - i;
    std::memcpy(tail, p + i, rem);
    tail[rem] = 0x80;
    size_t tail_len = rem + 1 + 8 <= 64 ? 64 : 128;
    uint64_t bits = uint64_t(n) * 8;
    for (int k = 0; k < 8; ++k) tail[tail_len - 1 - k] = static_cast<unsigned char>(bits >> (8 * k));
    sha256_block(h, tail);
    if (tail_len == 128) sha256_block(h, tail + 64);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (uint32_t v : h)
        for (int k = 28; k >= 0; k -= 4) out.push_back(hex[(v >> k) & 0xF]);
    return out;
}

// ------------------------------------------------------------------------------- base64
static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(std::string_view data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    size_t i = 0, n = data.size();
    for (; i + 3 <= n; i += 3) {
        uint32_t v = (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8) | p[i + 2];
        out += B64[v >> 18]; out += B64[(v >> 12) & 63]; out += B64[(v >> 6) & 63]; out += B64[v & 63];
    }
    if (n - i == 1) {
        uint32_t v = uint32_t(p[i]) << 16;
        out += B64[v >> 18]; out += B64[(v >> 12) & 63]; out += "==";
    } else if (n - i == 2) {
        uint32_t v = (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8);
        out += B64[v >> 18]; out += B64[(v >> 12) & 63]; out += B64[(v >> 6) & 63]; out += '=';
    }
    return out;
}

std::optional<std::string> base64_decode(std::string_view text) {
    if (text.size() % 4 != 0) return std::nullopt;
    int rev[256];
    std::fill(std::begin(rev), std::end(rev), -1);
    for (int i = 0; i < 64; ++i) rev[static_cast<unsigned char>(B64[i])] = i;
    std::string out;
    out.reserve(text.size() / 4 * 3);
    for (size_t i = 0; i < text.size(); i += 4) {
        int pad = 0, v[4];
        for (int k = 0; k < 4; ++k) {
            unsigned char c = static_cast<unsigned char>(text[i + k]);
            if (c == '=') {
                // padding only in the last quantum, only in its last two places
                if (i + 4 != text.size() || k < 2) return std::nullopt;
                v[k] = 0;
                ++pad;
            } else {
                if (pad || rev[c] < 0) return std::nullopt;
                v[k] = rev[c];
            }
        }
        uint32_t x = (uint32_t(v[0]) << 18) | (uint32_t(v[1]) << 12) | (uint32_t(v[2]) << 6) | uint32_t(v[3]);
        out += static_cast<char>(x >> 16);
        if (pad < 2) out += static_cast<char>((x >> 8) & 0xFF);
        if (pad < 1) out += static_cast<char>(x & 0xFF);
    }
    return out;
}

// ------------------------------------------------------------------------------ deflate
std::string deflate_raw(std::string_view data) {
    size_t out_len = 0;
    // level 9 probes; no TDEFL_WRITE_ZLIB_HEADER => raw RFC 1951 stream
    void* p = tdefl_compress_mem_to_heap(data.data(), data.size(), &out_len, TDEFL_DEFAULT_MAX_PROBES * 4);
    if (!p && !data.empty()) return {};
    std::string out(static_cast<const char*>(p), out_len);
    mz_free(p);
    return out;
}

std::optional<std::string> inflate_raw(std::string_view data) {
    size_t out_len = 0;
    void* p = tinfl_decompress_mem_to_heap(data.data(), data.size(), &out_len, 0);
    if (!p) return std::nullopt;
    std::string out(static_cast<const char*>(p), out_len);
    mz_free(p);
    return out;
}

}  // namespace ncx
