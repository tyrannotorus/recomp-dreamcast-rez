// SHA-1 of a byte buffer, as lowercase hex. Used to check that the boot
// executable read off a user's disc is the exact build a port was translated from: translated
// code bakes in addresses and constants, so any other revision of the game would misbehave.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace dream {

inline std::string sha1_hex(const std::uint8_t* data, std::size_t len) {
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    auto rol = [](std::uint32_t v, int s) { return (v << s) | (v >> (32 - s)); };
    const std::uint64_t bits = static_cast<std::uint64_t>(len) * 8;
    const std::size_t padded = ((len + 8) / 64 + 1) * 64;
    std::uint8_t block[64];
    for (std::size_t base = 0; base < padded; base += 64) {
        for (std::size_t i = 0; i < 64; ++i) {
            const std::size_t at = base + i;
            std::uint8_t b = 0;
            if (at < len)
                b = data[at];
            else if (at == len)
                b = 0x80;
            else if (at >= padded - 8)
                b = static_cast<std::uint8_t>(bits >> (8 * (padded - 1 - at)));
            block[i] = b;
        }
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = static_cast<std::uint32_t>(block[4 * i] << 24 | block[4 * i + 1] << 16 |
                                              block[4 * i + 2] << 8 | block[4 * i + 3]);
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    char out[41];
    for (int i = 0; i < 5; ++i) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
    return std::string(out, 40);
}

}  // namespace dream
