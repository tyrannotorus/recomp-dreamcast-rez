// See png.h. Written from RFC 1950 (zlib), RFC 1951 (deflate) and the PNG specification.
#include "dream/render/png.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dream::render::png {

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

void set_error(std::string* error, const char* what) {
    if (error)
        *error = what;
}

// ---------------------------------------------------------------------------------------------
// Checksums

const std::array<u32, 256>& crc_table() {
    static const std::array<u32, 256> table = [] {
        std::array<u32, 256> t{};
        for (u32 n = 0; n < 256; ++n) {
            u32 c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    return table;
}

// ---------------------------------------------------------------------------------------------
// Deflate tables (RFC 1951 section 3.2.5)

constexpr u16 kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr u8 kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr u16 kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                               33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                               1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr u8 kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                               6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
// The order the code-length code lengths are sent in a dynamic block header.
constexpr u8 kCodeLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

u32 reverse_bits(u32 code, unsigned len) {
    u32 r = 0;
    for (unsigned i = 0; i < len; ++i) {
        r = (r << 1) | (code & 1u);
        code >>= 1;
    }
    return r;
}

// ---------------------------------------------------------------------------------------------
// Inflate

// Deflate packs bits least significant first. The reader keeps up to 64 bits buffered and pads
// past the end with zeros; `overrun()` says whether any of the padding was consumed.
class BitReader {
public:
    BitReader(const u8* data, std::size_t size) : p_(data), n_(size) {}

    u32 peek(unsigned k) {
        if (cnt_ < k)
            refill();
        return k == 0 ? 0u : static_cast<u32>(buf_ & ((u64{1} << k) - 1u));
    }
    void drop(unsigned k) {
        buf_ >>= k;
        cnt_ -= k;
    }
    u32 bits(unsigned k) {
        const u32 v = peek(k);
        drop(k);
        return v;
    }
    void align() { drop(cnt_ % 8u); }
    bool overrun() const { return pos_ * 8 - cnt_ > n_ * 8; }

private:
    void refill() {
        while (cnt_ <= 56) {
            const u64 b = pos_ < n_ ? p_[pos_] : 0u;
            ++pos_;
            buf_ |= b << cnt_;
            cnt_ += 8;
        }
    }

    const u8* p_;
    std::size_t n_;
    std::size_t pos_ = 0;
    u64 buf_ = 0;
    unsigned cnt_ = 0;
};

// A canonical Huffman code as a single lookup table indexed by the next `maxbits` input bits
// (already in the stream's reversed order). Each entry is symbol << 4 | length; length 0 marks a
// bit pattern no code starts with.
struct Huffman {
    std::vector<u16> table;
    unsigned maxbits = 0;

    bool build(const u8* lengths, unsigned n) {
        unsigned count[16]{};
        maxbits = 0;
        for (unsigned i = 0; i < n; ++i) {
            ++count[lengths[i]];
            maxbits = std::max<unsigned>(maxbits, lengths[i]);
        }
        count[0] = 0;
        int left = 1;  // over-subscribed sets are invalid; incomplete ones are allowed
        for (unsigned len = 1; len <= 15; ++len) {
            left <<= 1;
            left -= static_cast<int>(count[len]);
            if (left < 0)
                return false;
        }
        u32 next[16]{};
        u32 code = 0;
        for (unsigned len = 1; len <= 15; ++len) {
            code = (code + count[len - 1]) << 1;
            next[len] = code;
        }
        table.assign(std::size_t{1} << maxbits, 0);
        for (unsigned sym = 0; sym < n; ++sym) {
            const unsigned len = lengths[sym];
            if (!len)
                continue;
            const u32 rev = reverse_bits(next[len]++, len);
            for (u32 i = rev; i < table.size(); i += 1u << len)
                table[i] = static_cast<u16>((sym << 4) | len);
        }
        return true;
    }

    int decode(BitReader& br) const {
        const u16 e = table[br.peek(maxbits)];
        if (!(e & 15u))
            return -1;
        br.drop(e & 15u);
        return e >> 4;
    }
};

bool inflate_codes(BitReader& br, const Huffman& lit, const Huffman& dist, std::vector<u8>& out,
                   std::size_t limit, std::string* error) {
    for (;;) {
        const int sym = lit.decode(br);
        if (sym < 0 || br.overrun()) {
            set_error(error, "inflate: bad literal/length code or truncated data");
            return false;
        }
        if (sym < 256) {
            out.push_back(static_cast<u8>(sym));
        } else if (sym == 256) {
            return true;
        } else {
            const int li = sym - 257;
            if (li >= 29) {
                set_error(error, "inflate: invalid length symbol");
                return false;
            }
            const u32 length = kLengthBase[li] + br.bits(kLengthExtra[li]);
            const int ds = dist.decode(br);
            if (ds < 0 || ds >= 30) {
                set_error(error, "inflate: bad distance code");
                return false;
            }
            const u32 distance = kDistBase[ds] + br.bits(kDistExtra[ds]);
            if (distance > out.size()) {
                set_error(error, "inflate: distance reaches before the start of the data");
                return false;
            }
            std::size_t from = out.size() - distance;
            for (u32 i = 0; i < length; ++i) out.push_back(out[from++]);
        }
        if (limit && out.size() > limit) {
            set_error(error, "inflate: more data than the image needs");
            return false;
        }
    }
}

bool inflate(const u8* data, std::size_t size, std::vector<u8>& out, std::size_t limit,
             std::string* error) {
    BitReader br(data, size);
    Huffman fixed_lit, fixed_dist;
    bool have_fixed = false;
    for (;;) {
        const u32 final_block = br.bits(1);
        const u32 type = br.bits(2);
        if (type == 0) {
            br.align();
            const u32 len = br.bits(16), nlen = br.bits(16);
            if ((len ^ 0xFFFFu) != nlen) {
                set_error(error, "inflate: stored block length check failed");
                return false;
            }
            for (u32 i = 0; i < len; ++i) out.push_back(static_cast<u8>(br.bits(8)));
            if (br.overrun()) {
                set_error(error, "inflate: truncated stored block");
                return false;
            }
            if (limit && out.size() > limit) {
                set_error(error, "inflate: more data than the image needs");
                return false;
            }
        } else if (type == 1) {
            if (!have_fixed) {
                u8 lengths[288];
                std::fill(lengths, lengths + 144, u8{8});
                std::fill(lengths + 144, lengths + 256, u8{9});
                std::fill(lengths + 256, lengths + 280, u8{7});
                std::fill(lengths + 280, lengths + 288, u8{8});
                fixed_lit.build(lengths, 288);
                u8 d[30];
                std::fill(d, d + 30, u8{5});
                fixed_dist.build(d, 30);
                have_fixed = true;
            }
            if (!inflate_codes(br, fixed_lit, fixed_dist, out, limit, error))
                return false;
        } else if (type == 2) {
            const unsigned hlit = br.bits(5) + 257, hdist = br.bits(5) + 1,
                           hclen = br.bits(4) + 4;
            if (hlit > 286 || hdist > 30) {
                set_error(error, "inflate: too many codes in a dynamic block");
                return false;
            }
            u8 cl_lengths[19]{};
            for (unsigned i = 0; i < hclen; ++i)
                cl_lengths[kCodeLengthOrder[i]] = static_cast<u8>(br.bits(3));
            Huffman cl;
            if (!cl.build(cl_lengths, 19)) {
                set_error(error, "inflate: bad code-length code");
                return false;
            }
            u8 lengths[286 + 30]{};
            unsigned n = 0;
            while (n < hlit + hdist) {
                const int sym = cl.decode(br);
                if (sym < 0 || br.overrun()) {
                    set_error(error, "inflate: bad code length");
                    return false;
                }
                if (sym < 16) {
                    lengths[n++] = static_cast<u8>(sym);
                    continue;
                }
                u8 value = 0;
                unsigned repeat = 0;
                if (sym == 16) {
                    if (n == 0) {
                        set_error(error, "inflate: repeat with no previous length");
                        return false;
                    }
                    value = lengths[n - 1];
                    repeat = 3 + br.bits(2);
                } else if (sym == 17) {
                    repeat = 3 + br.bits(3);
                } else {
                    repeat = 11 + br.bits(7);
                }
                if (n + repeat > hlit + hdist) {
                    set_error(error, "inflate: code lengths overrun");
                    return false;
                }
                while (repeat--) lengths[n++] = value;
            }
            if (lengths[256] == 0) {
                set_error(error, "inflate: no end-of-block code");
                return false;
            }
            Huffman lit, dist;
            if (!lit.build(lengths, hlit) || !dist.build(lengths + hlit, hdist)) {
                set_error(error, "inflate: over-subscribed Huffman code");
                return false;
            }
            if (!inflate_codes(br, lit, dist, out, limit, error))
                return false;
        } else {
            set_error(error, "inflate: reserved block type");
            return false;
        }
        if (final_block)
            break;
    }
    if (br.overrun()) {
        set_error(error, "inflate: truncated data");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Deflate: one fixed-Huffman block, greedy LZ77 over a 32 KB window with hash chains.

class BitWriter {
public:
    explicit BitWriter(std::vector<u8>& out) : out_(out) {}
    void put(u32 bits, unsigned n) {
        buf_ |= static_cast<u64>(bits) << cnt_;
        cnt_ += n;
        while (cnt_ >= 8) {
            out_.push_back(static_cast<u8>(buf_));
            buf_ >>= 8;
            cnt_ -= 8;
        }
    }
    void flush() {
        if (cnt_)
            out_.push_back(static_cast<u8>(buf_));
        buf_ = 0;
        cnt_ = 0;
    }

private:
    std::vector<u8>& out_;
    u64 buf_ = 0;
    unsigned cnt_ = 0;
};

void put_fixed_literal(BitWriter& bw, unsigned sym) {
    u32 code;
    unsigned len;
    if (sym < 144) {
        code = 0x30u + sym;
        len = 8;
    } else if (sym < 256) {
        code = 0x190u + (sym - 144);
        len = 9;
    } else if (sym < 280) {
        code = sym - 256;
        len = 7;
    } else {
        code = 0xC0u + (sym - 280);
        len = 8;
    }
    bw.put(reverse_bits(code, len), len);
}

void put_match(BitWriter& bw, u32 length, u32 distance) {
    int li = 28;
    while (kLengthBase[li] > length) --li;
    put_fixed_literal(bw, 257u + static_cast<unsigned>(li));
    bw.put(length - kLengthBase[li], kLengthExtra[li]);
    int di = 29;
    while (kDistBase[di] > distance) --di;
    bw.put(reverse_bits(static_cast<u32>(di), 5), 5);
    bw.put(distance - kDistBase[di], kDistExtra[di]);
}

void deflate_fixed(const u8* data, std::size_t size, std::vector<u8>& out) {
    constexpr std::size_t kWindow = 32768;
    constexpr unsigned kHashBits = 15;
    constexpr int kMaxChain = 32;
    constexpr u32 kMinMatch = 3, kMaxMatch = 258;
    BitWriter bw(out);
    bw.put(1, 1);  // final block
    bw.put(1, 2);  // fixed Huffman
    std::vector<std::int64_t> head(std::size_t{1} << kHashBits, -1);
    std::vector<std::int64_t> prev(kWindow, -1);
    const auto hash_at = [&](std::size_t i) {
        const u32 v = data[i] | (u32{data[i + 1]} << 8) | (u32{data[i + 2]} << 16);
        return (v * 2654435761u) >> (32 - kHashBits);
    };
    const auto insert = [&](std::size_t i) {
        if (i + kMinMatch > size)
            return;
        const u32 h = hash_at(i);
        prev[i % kWindow] = head[h];
        head[h] = static_cast<std::int64_t>(i);
    };
    std::size_t i = 0;
    while (i < size) {
        u32 best_len = 0, best_dist = 0;
        if (i + kMinMatch <= size) {
            const std::size_t max_len = std::min<std::size_t>(kMaxMatch, size - i);
            std::int64_t cand = head[hash_at(i)];
            for (int chain = 0; chain < kMaxChain && cand >= 0; ++chain) {
                const std::size_t c = static_cast<std::size_t>(cand);
                if (i - c > kWindow - 1)
                    break;
                if (data[c + best_len] == data[i + best_len] || best_len == 0) {
                    std::size_t len = 0;
                    while (len < max_len && data[c + len] == data[i + len]) ++len;
                    if (len > best_len) {
                        best_len = static_cast<u32>(len);
                        best_dist = static_cast<u32>(i - c);
                        if (len == max_len)
                            break;
                    }
                }
                const std::int64_t next = prev[c % kWindow];
                if (next >= cand)
                    break;  // the slot was reused by a newer position
                cand = next;
            }
        }
        if (best_len >= kMinMatch) {
            put_match(bw, best_len, best_dist);
            for (u32 k = 0; k < best_len; ++k) insert(i + k);
            i += best_len;
        } else {
            put_fixed_literal(bw, data[i]);
            insert(i);
            ++i;
        }
    }
    put_fixed_literal(bw, 256);
    bw.flush();
}

// ---------------------------------------------------------------------------------------------
// PNG

constexpr u8 kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

void put_be32(std::vector<u8>& v, u32 x) {
    v.push_back(static_cast<u8>(x >> 24));
    v.push_back(static_cast<u8>(x >> 16));
    v.push_back(static_cast<u8>(x >> 8));
    v.push_back(static_cast<u8>(x));
}

u32 get_be32(const u8* p) {
    return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | u32{p[3]};
}

void put_chunk(std::vector<u8>& out, const char type[4], const std::vector<u8>& data) {
    put_be32(out, static_cast<u32>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put_be32(out, crc32(out.data() + start, out.size() - start));
}

u8 paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc)
        return static_cast<u8>(a);
    return static_cast<u8>(pb <= pc ? b : c);
}

// Applies filter `type` to `row` (with the previous row `up`, all zeros for the first) into `dst`.
void filter_row(int type, const u8* row, const u8* up, std::size_t n, unsigned bpp, u8* dst) {
    for (std::size_t x = 0; x < n; ++x) {
        const int a = x >= bpp ? row[x - bpp] : 0;
        const int b = up[x];
        const int c = x >= bpp ? up[x - bpp] : 0;
        int pred = 0;
        switch (type) {
            case 1: pred = a; break;
            case 2: pred = b; break;
            case 3: pred = (a + b) / 2; break;
            case 4: pred = paeth(a, b, c); break;
            default: break;
        }
        dst[x] = static_cast<u8>(row[x] - pred);
    }
}

bool unfilter_row(int type, u8* row, const u8* up, std::size_t n, unsigned bpp) {
    if (type < 0 || type > 4)
        return false;
    for (std::size_t x = 0; x < n; ++x) {
        const int a = x >= bpp ? row[x - bpp] : 0;
        const int b = up[x];
        const int c = x >= bpp ? up[x - bpp] : 0;
        int pred = 0;
        switch (type) {
            case 1: pred = a; break;
            case 2: pred = b; break;
            case 3: pred = (a + b) / 2; break;
            case 4: pred = paeth(a, b, c); break;
            default: break;
        }
        row[x] = static_cast<u8>(row[x] + pred);
    }
    return true;
}

}  // namespace

std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc) {
    const auto& t = crc_table();
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) crc = t[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

std::uint32_t adler32(const std::uint8_t* data, std::size_t size, std::uint32_t adler) {
    u32 a = adler & 0xFFFFu, b = adler >> 16;
    while (size) {
        // 5552 is the largest run that cannot overflow 32 bits before the modulo.
        const std::size_t n = std::min<std::size_t>(size, 5552);
        for (std::size_t i = 0; i < n; ++i) {
            a += data[i];
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
        data += n;
        size -= n;
    }
    return (b << 16) | a;
}

std::vector<std::uint8_t> zlib_compress(const std::uint8_t* data, std::size_t size) {
    std::vector<u8> out;
    out.reserve(size / 2 + 64);
    out.push_back(0x78);  // deflate, 32 KB window
    out.push_back(0x01);  // no dictionary, (0x7801 % 31) == 0
    deflate_fixed(data, size, out);
    put_be32(out, adler32(data, size));
    return out;
}

bool zlib_decompress(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out,
                     std::size_t expected_size, std::string* error) {
    out.clear();
    if (size < 6) {
        set_error(error, "zlib: stream too short");
        return false;
    }
    const u32 cmf = data[0], flg = data[1];
    if ((cmf & 0x0Fu) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31u != 0) {
        set_error(error, "zlib: bad header");
        return false;
    }
    if (flg & 0x20u) {
        set_error(error, "zlib: preset dictionaries are not supported");
        return false;
    }
    if (expected_size)
        out.reserve(expected_size);
    if (!inflate(data + 2, size - 6, out, expected_size, error))
        return false;
    if (get_be32(data + size - 4) != adler32(out.data(), out.size())) {
        set_error(error, "zlib: Adler-32 mismatch");
        return false;
    }
    return true;
}

std::vector<std::uint8_t> encode(const std::uint32_t* pixels, std::uint32_t width,
                                 std::uint32_t height) {
    const std::size_t stride = static_cast<std::size_t>(width) * 4;
    std::vector<u8> raw;
    raw.reserve((stride + 1) * height);
    std::vector<u8> row(stride), up(stride, 0), best(stride), trial(stride);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const u32 p = pixels[static_cast<std::size_t>(y) * width + x];
            row[x * 4u + 0] = static_cast<u8>(p);
            row[x * 4u + 1] = static_cast<u8>(p >> 8);
            row[x * 4u + 2] = static_cast<u8>(p >> 16);
            row[x * 4u + 3] = static_cast<u8>(p >> 24);
        }
        // The usual heuristic: the filter whose output has the smallest sum of magnitudes.
        int best_type = 0;
        u64 best_score = ~u64{0};
        for (int type = 0; type <= 4; ++type) {
            filter_row(type, row.data(), up.data(), stride, 4, trial.data());
            u64 score = 0;
            for (u8 v : trial) score += static_cast<u64>(std::abs(static_cast<int>(static_cast<std::int8_t>(v))));
            if (score < best_score) {
                best_score = score;
                best_type = type;
                best.swap(trial);
            }
        }
        raw.push_back(static_cast<u8>(best_type));
        raw.insert(raw.end(), best.begin(), best.end());
        up.swap(row);
    }

    std::vector<u8> out(kSignature, kSignature + 8);
    std::vector<u8> ihdr;
    put_be32(ihdr, width);
    put_be32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA, deflate, adaptive filter, progressive off
    put_chunk(out, "IHDR", ihdr);
    put_chunk(out, "IDAT", zlib_compress(raw.data(), raw.size()));
    put_chunk(out, "IEND", {});
    return out;
}

bool decode(const std::uint8_t* data, std::size_t size, Image& out, std::string* error) {
    out = Image{};
    if (size < 8 || std::memcmp(data, kSignature, 8) != 0) {
        set_error(error, "png: not a PNG file");
        return false;
    }
    u32 width = 0, height = 0;
    u8 depth = 0, color = 0, interlace = 0;
    bool have_header = false;
    std::vector<u8> idat;
    u8 palette[256][4]{};
    unsigned palette_size = 0;
    bool have_key = false;
    u16 key[3]{};
    std::size_t pos = 8;
    bool ended = false;
    while (pos + 12 <= size) {
        const u32 len = get_be32(data + pos);
        if (len > size - pos - 12) {
            set_error(error, "png: chunk runs past the end of the file");
            return false;
        }
        const u8* type = data + pos + 4;
        const u8* body = data + pos + 8;
        if (get_be32(body + len) != crc32(type, len + 4u)) {
            set_error(error, "png: chunk CRC mismatch");
            return false;
        }
        pos += 12u + len;
        if (!std::memcmp(type, "IHDR", 4)) {
            if (len != 13) {
                set_error(error, "png: bad IHDR");
                return false;
            }
            width = get_be32(body);
            height = get_be32(body + 4);
            depth = body[8];
            color = body[9];
            interlace = body[12];
            if (body[10] != 0 || body[11] != 0) {
                set_error(error, "png: unknown compression or filter method");
                return false;
            }
            have_header = true;
        } else if (!std::memcmp(type, "PLTE", 4)) {
            palette_size = std::min<u32>(len / 3, 256);
            for (unsigned i = 0; i < palette_size; ++i) {
                palette[i][0] = body[i * 3];
                palette[i][1] = body[i * 3 + 1];
                palette[i][2] = body[i * 3 + 2];
                palette[i][3] = 255;
            }
        } else if (!std::memcmp(type, "tRNS", 4)) {
            if (color == 3) {
                for (unsigned i = 0; i < len && i < 256; ++i) palette[i][3] = body[i];
            } else if (color == 0 && len >= 2) {
                have_key = true;
                key[0] = static_cast<u16>((body[0] << 8) | body[1]);
            } else if (color == 2 && len >= 6) {
                have_key = true;
                for (int c = 0; c < 3; ++c)
                    key[c] = static_cast<u16>((body[c * 2] << 8) | body[c * 2 + 1]);
            }
        } else if (!std::memcmp(type, "IDAT", 4)) {
            idat.insert(idat.end(), body, body + len);
        } else if (!std::memcmp(type, "IEND", 4)) {
            ended = true;
            break;
        } else if (!(type[0] & 0x20u)) {
            set_error(error, "png: unknown critical chunk");
            return false;
        }
    }
    if (!have_header || !ended) {
        set_error(error, "png: missing IHDR or IEND");
        return false;
    }
    if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension) {
        set_error(error, "png: unsupported dimensions");
        return false;
    }
    if (depth != 8) {
        set_error(error, "png: only 8-bit channels are supported");
        return false;
    }
    if (interlace != 0) {
        set_error(error, "png: interlaced images are not supported");
        return false;
    }
    unsigned channels = 0;
    switch (color) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default:
            set_error(error, "png: unknown colour type");
            return false;
    }
    if (color == 3 && palette_size == 0) {
        set_error(error, "png: palette image without PLTE");
        return false;
    }
    const std::size_t stride = static_cast<std::size_t>(width) * channels;
    const std::size_t expected = (stride + 1) * height;
    std::vector<u8> raw;
    if (!zlib_decompress(idat.data(), idat.size(), raw, expected, error))
        return false;
    if (raw.size() != expected) {
        set_error(error, "png: image data is the wrong size");
        return false;
    }
    std::vector<u8> zero(stride, 0);
    out.width = width;
    out.height = height;
    out.pixels.resize(static_cast<std::size_t>(width) * height);
    for (u32 y = 0; y < height; ++y) {
        u8* row = raw.data() + y * (stride + 1);
        const u8* up = y ? raw.data() + (y - 1) * (stride + 1) + 1 : zero.data();
        if (!unfilter_row(row[0], row + 1, up, stride, channels)) {
            set_error(error, "png: unknown filter type");
            out = Image{};
            return false;
        }
        const u8* p = row + 1;
        u32* dst = out.pixels.data() + static_cast<std::size_t>(y) * width;
        for (u32 x = 0; x < width; ++x, p += channels) {
            u32 r, g, b, a = 255;
            switch (color) {
                case 0:
                    r = g = b = p[0];
                    if (have_key && p[0] == key[0])
                        a = 0;
                    break;
                case 2:
                    r = p[0], g = p[1], b = p[2];
                    if (have_key && r == key[0] && g == key[1] && b == key[2])
                        a = 0;
                    break;
                case 3: {
                    if (p[0] >= palette_size) {
                        set_error(error, "png: palette index out of range");
                        out = Image{};
                        return false;
                    }
                    const u8* e = palette[p[0]];
                    r = e[0], g = e[1], b = e[2], a = e[3];
                    break;
                }
                case 4:
                    r = g = b = p[0];
                    a = p[1];
                    break;
                default:
                    r = p[0], g = p[1], b = p[2], a = p[3];
                    break;
            }
            dst[x] = r | (g << 8) | (b << 16) | (a << 24);
        }
    }
    return true;
}

bool write_file(const std::filesystem::path& path, const std::uint32_t* pixels,
                std::uint32_t width, std::uint32_t height) {
    const std::vector<u8> bytes = encode(pixels, width, height);
#ifdef _WIN32
    FILE* f = _wfopen(path.c_str(), L"wb");
#else
    FILE* f = std::fopen(path.c_str(), "wb");
#endif
    if (!f)
        return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return std::fclose(f) == 0 && ok;
}

bool read_file(const std::filesystem::path& path, Image& out, std::string* error) {
#ifdef _WIN32
    FILE* f = _wfopen(path.c_str(), L"rb");
#else
    FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) {
        set_error(error, "png: cannot open the file");
        return false;
    }
    std::vector<u8> bytes;
    u8 buf[65536];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
    return decode(bytes.data(), bytes.size(), out, error);
}

}  // namespace dream::render::png
