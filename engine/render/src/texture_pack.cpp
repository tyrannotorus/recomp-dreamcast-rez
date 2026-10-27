// See texture_pack.h. Any change to what texture_hash_input produces for an existing texture breaks every pack made with
// it, so version 1 is frozen.
#include "dream/render/texture_pack.h"

#include <cstdio>
#include <cstring>

namespace dream::render {

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u64 kP1 = 0x9E3779B185EBCA87ull;
constexpr u64 kP2 = 0xC2B2AE3D27D4EB4Full;
constexpr u64 kP3 = 0x165667B19E3779F9ull;
constexpr u64 kP4 = 0x85EBCA77C2B2AE63ull;
constexpr u64 kP5 = 0x27D4EB2F165667C5ull;

constexpr u64 rotl(u64 x, int r) {
    return (x << r) | (x >> (64 - r));
}
u64 read64(const u8* p) {
    u64 v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
u32 read32(const u8* p) {
    return u32{p[0]} | (u32{p[1]} << 8) | (u32{p[2]} << 16) | (u32{p[3]} << 24);
}
u64 xxh_round(u64 acc, u64 input) {
    acc += input * kP2;
    acc = rotl(acc, 31);
    return acc * kP1;
}
u64 xxh_merge(u64 acc, u64 val) {
    acc ^= xxh_round(0, val);
    return acc * kP1 + kP4;
}

void put32(std::vector<u8>& v, u32 x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<u8>(x >> (8 * i)));
}

// Where the base level of a mipmapped texture starts, in bytes from its address (after the
// codebook for a compressed one). This must stay identical to mipmap_base_offset in texture.cpp:
// the hash covers exactly the bytes the decoder reads.
u32 base_level_offset(const TextureInfo& info) {
    if (!info.mipmapped)
        return 0;
    u32 texels = 0;
    for (u32 size = 1; size < info.width; size <<= 1) texels += size * size;
    if (info.vq)
        return texels / 4u + 1u;
    const u32 bpp = info.format == PixelFormat::Palette4   ? 4u
                    : info.format == PixelFormat::Palette8 ? 8u
                                                           : 16u;
    return (texels + 3u) * bpp / 8u;
}

}  // namespace

std::uint64_t xxh64(const void* data, std::size_t size, std::uint64_t seed) {
    const u8* p = static_cast<const u8*>(data);
    const u8* const end = p + size;
    u64 h;
    if (size >= 32) {
        u64 v1 = seed + kP1 + kP2, v2 = seed + kP2, v3 = seed, v4 = seed - kP1;
        const u8* const limit = end - 32;
        do {
            v1 = xxh_round(v1, read64(p));
            v2 = xxh_round(v2, read64(p + 8));
            v3 = xxh_round(v3, read64(p + 16));
            v4 = xxh_round(v4, read64(p + 24));
            p += 32;
        } while (p <= limit);
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = xxh_merge(h, v1);
        h = xxh_merge(h, v2);
        h = xxh_merge(h, v3);
        h = xxh_merge(h, v4);
    } else {
        h = seed + kP5;
    }
    h += static_cast<u64>(size);
    while (p + 8 <= end) {
        h ^= xxh_round(0, read64(p));
        h = rotl(h, 27) * kP1 + kP4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= static_cast<u64>(read32(p)) * kP1;
        h = rotl(h, 23) * kP2 + kP3;
        p += 4;
    }
    while (p < end) {
        h ^= static_cast<u64>(*p) * kP5;
        h = rotl(h, 11) * kP1;
        ++p;
    }
    h ^= h >> 33;
    h *= kP2;
    h ^= h >> 29;
    h *= kP3;
    h ^= h >> 32;
    return h;
}

std::vector<std::uint8_t> texture_hash_input(const TextureInfo& info, const std::uint8_t* vram,
                                             std::size_t vram_size, const std::uint32_t* palette) {
    std::vector<u8> out;
    if (!vram || info.format == PixelFormat::Reserved)
        return out;
    const u32 w = info.width, h = info.height;
    const auto in_range = [&](u32 offset, u32 bytes) {
        return static_cast<std::size_t>(offset) + bytes <= vram_size;
    };

    // Header: what kind of texture this is, so the same bytes read as two different formats
    // never collide.
    const u32 flags = (static_cast<u32>(info.format) & 7u) | (info.twiddled ? 1u << 3 : 0u) |
                      (info.vq ? 1u << 4 : 0u) | (info.mipmapped ? 1u << 5 : 0u) |
                      (info.stride ? 1u << 6 : 0u);
    std::vector<u8> header;
    header.insert(header.end(), {'D', 'C', 'T', 'X'});
    put32(header, kTextureHashVersion);
    put32(header, w);
    put32(header, h);
    put32(header, flags);

    if (info.vq) {
        // The whole 2 KB codebook, then one index byte per 2x2 block of the top level.
        const u32 codebook = info.address;
        const u32 indices = info.address + 256u * 8u + base_level_offset(info);
        const u32 index_bytes = (w / 2) * (h / 2);
        if (!in_range(codebook, 256u * 8u) || !in_range(indices, index_bytes))
            return out;
        out = std::move(header);
        out.insert(out.end(), vram + codebook, vram + codebook + 256u * 8u);
        out.insert(out.end(), vram + indices, vram + indices + index_bytes);
        return out;
    }

    u32 bytes = 0;
    switch (info.format) {
        case PixelFormat::Palette4: bytes = w * h / 2; break;
        case PixelFormat::Palette8: bytes = w * h; break;
        default: bytes = w * h * 2; break;
    }
    const u32 base = info.address + base_level_offset(info);
    if (!in_range(base, bytes))
        return out;
    out = std::move(header);
    out.insert(out.end(), vram + base, vram + base + bytes);

    if (info.indexed()) {
        if (!palette)
            return {};
        // The colours of the palette entries the texture actually uses, in index order. Unused
        // entries are left out on purpose: titles share a palette bank between textures, and a
        // change to someone else's colours must not rename this one.
        bool used[256]{};
        if (info.format == PixelFormat::Palette4) {
            for (u32 i = 0; i < bytes; ++i) {
                used[vram[base + i] & 0xFu] = true;
                used[vram[base + i] >> 4] = true;
            }
        } else {
            for (u32 i = 0; i < bytes; ++i) used[vram[base + i]] = true;
        }
        const u32 entries = info.format == PixelFormat::Palette4 ? 16u : 256u;
        for (u32 i = 0; i < entries; ++i) {
            if (!used[i])
                continue;
            const u32 index = info.palette_base + i;
            put32(out, index < 1024 ? palette[index] : 0u);
        }
    }
    return out;
}

bool texture_hash(const TextureInfo& info, const std::uint8_t* vram, std::size_t vram_size,
                  const std::uint32_t* palette, std::uint64_t& out) {
    const std::vector<u8> input = texture_hash_input(info, vram, vram_size, palette);
    if (input.empty())
        return false;
    out = xxh64(input.data(), input.size(), 0);
    return true;
}

std::string texture_format_name(const TextureInfo& info) {
    static const char* kFormats[8] = {"1555", "565",  "4444", "yuv422",
                                      "bump", "pal4", "pal8", "reserved"};
    std::string name = info.vq ? "vq" : "";
    name += kFormats[static_cast<unsigned>(info.format) & 7u];
    if (info.mipmapped)
        name += "mm";
    return name;
}

std::string texture_file_name(const TextureInfo& info, std::uint64_t hash) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%ux%u_%s_%016llx.png", info.width, info.height,
                  texture_format_name(info).c_str(), static_cast<unsigned long long>(hash));
    return buf;
}

bool parse_texture_file_name(const std::string& name, std::uint64_t& hash) {
    if (name.size() < 4 + 16 + 1)
        return false;
    std::string ext = name.substr(name.size() - 4);
    for (char& c : ext) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    if (ext != ".png")
        return false;
    const std::size_t digits = name.size() - 4 - 16;
    if (name[digits - 1] != '_')
        return false;
    u64 v = 0;
    for (std::size_t i = digits; i < digits + 16; ++i) {
        const char c = name[i];
        u64 d;
        if (c >= '0' && c <= '9')
            d = static_cast<u64>(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = static_cast<u64>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            d = static_cast<u64>(c - 'A' + 10);
        else
            return false;
        v = (v << 4) | d;
    }
    hash = v;
    return true;
}

bool TextureReplacer::open_pack(const std::filesystem::path& dir, std::string* error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        if (error)
            *error = "not a directory: " + dir.string();
        return false;
    }
    std::size_t duplicates = 0;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec))
            continue;
        u64 hash;
        if (!parse_texture_file_name(it->path().filename().string(), hash))
            continue;
        if (!index_.emplace(hash, it->path()).second)
            ++duplicates;  // the first one found wins
    }
    if (ec) {
        if (error)
            *error = "cannot read " + dir.string() + ": " + ec.message();
        return false;
    }
    if (duplicates)
        std::fprintf(stderr, "texture pack: %zu files repeat a hash already in the pack; ignored\n",
                     duplicates);
    return true;
}

bool TextureReplacer::set_dump_dir(const std::filesystem::path& dir, std::string* error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec)) {
        if (error)
            *error = "cannot create " + dir.string();
        return false;
    }
    dump_dir_ = dir;
    // What an earlier run already wrote is not written again.
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        u64 hash;
        if (parse_texture_file_name(it->path().filename().string(), hash))
            dumped_.insert(hash);
    }
    return true;
}

const png::Image* TextureReplacer::find(const TextureInfo& info, std::uint64_t hash) {
    const auto where = index_.find(hash);
    if (where == index_.end())
        return nullptr;
    if (auto it = loaded_.find(hash); it != loaded_.end())
        return it->second->failed ? nullptr : &it->second->image;

    auto slot = std::make_unique<Loaded>();
    std::string error;
    if (!png::read_file(where->second, slot->image, &error)) {
        std::fprintf(stderr, "texture pack: %s: %s\n", where->second.string().c_str(),
                     error.c_str());
        slot->failed = true;
        slot->image = png::Image{};
        loaded_.emplace(hash, std::move(slot));
        return nullptr;
    }
    const png::Image& img = slot->image;
    // A replacement may be any size, but a different shape stretches the art across the polygons.
    if (static_cast<u64>(img.width) * info.height != static_cast<u64>(img.height) * info.width)
        std::fprintf(stderr,
                     "texture pack: %s is %ux%u, the original is %ux%u; it will be stretched\n",
                     where->second.filename().string().c_str(), img.width, img.height, info.width,
                     info.height);
    const std::size_t bytes = img.pixels.size() * 4;
    if (retained_bytes_ + bytes <= retain_budget) {
        retained_bytes_ += bytes;
        const png::Image* kept = &slot->image;
        loaded_.emplace(hash, std::move(slot));
        return kept;
    }
    scratch_ = std::move(slot->image);
    return &scratch_;
}

void TextureReplacer::dump(const TextureInfo& info, std::uint64_t hash,
                           const std::vector<std::uint32_t>& pixels) {
    if (dump_dir_.empty() || !dumped_.insert(hash).second)
        return;
    if (pixels.size() < static_cast<std::size_t>(info.width) * info.height)
        return;
    const auto path = dump_dir_ / texture_file_name(info, hash);
    if (!png::write_file(path, pixels.data(), info.width, info.height))
        std::fprintf(stderr, "texture dump: cannot write %s\n", path.string().c_str());
}

}  // namespace dream::render
