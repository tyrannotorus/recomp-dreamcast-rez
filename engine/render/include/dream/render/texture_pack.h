// Texture dumping and replacement packs. Device-independent: the Vulkan texture cache
// asks this layer, on every decode, whether a texture should be written out or swapped for an
// image from a pack, and nothing here knows about Vulkan.
//
// A texture's identity is a content hash, not the cache key. The cache key holds the texture's
// address in video memory, and a title that streams reloads the same art at different addresses;
// a pack keyed by address would work on one screen and fall apart on the next. The hash is taken
// over the raw video-memory bytes the decoder reads (plus the palette colours an indexed texture
// actually uses, and the codebook of a compressed one), never over decoded pixels, so fixing a
// decoder bug can never silently invalidate a pack.
//
// The scheme is versioned (kTextureHashVersion). Packs depend on it for ever: change it only by
// adding a new version, never by editing version 1.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "dream/render/png.h"
#include "dream/render/texture.h"

namespace dream::render {

constexpr std::uint32_t kTextureHashVersion = 1;

// XXH64 with the given seed, written from the published algorithm description.
std::uint64_t xxh64(const void* data, std::size_t size, std::uint64_t seed = 0);

// The byte string version 1 hashes. Empty when the texture would read
// outside video memory, which is also when the decoder refuses it.
std::vector<std::uint8_t> texture_hash_input(const TextureInfo& info, const std::uint8_t* vram,
                                             std::size_t vram_size, const std::uint32_t* palette);
// XXH64 (seed 0) of texture_hash_input. False when there is nothing to hash.
bool texture_hash(const TextureInfo& info, const std::uint8_t* vram, std::size_t vram_size,
                  const std::uint32_t* palette, std::uint64_t& out);

// "1555", "565", "4444", "yuv422", "bump", "pal4", "pal8", prefixed "vq" when compressed and
// suffixed "mm" when mipmapped: "vq565mm".
std::string texture_format_name(const TextureInfo& info);
// "<w>x<h>_<format>_<16 hex digits>.png". Only the hash after the last underscore identifies the
// texture; the rest is for the person editing the pack.
std::string texture_file_name(const TextureInfo& info, std::uint64_t hash);
// The hash a pack file stands for, read from the 16 hex digits between the last '_' and ".png".
bool parse_texture_file_name(const std::string& file_name, std::uint64_t& hash);

// What the texture cache consults on every decode. Inactive (and free) unless a pack or a dump
// directory has been given.
class TextureReplacer {
public:
    // Indexes every `*_<hash>.png` under `dir`, recursively, once: lookups never touch the file
    // system afterwards. Returns false (with a message) when the directory cannot be read.
    bool open_pack(const std::filesystem::path& dir, std::string* error = nullptr);
    // Each distinct texture is written to `dir` once, as an RGBA PNG of its top level. Files
    // already there (from an earlier run) are not written again.
    bool set_dump_dir(const std::filesystem::path& dir, std::string* error = nullptr);

    bool active() const noexcept { return replacing() || dumping(); }
    bool replacing() const noexcept { return !index_.empty(); }
    bool dumping() const noexcept { return !dump_dir_.empty(); }
    std::size_t pack_size() const noexcept { return index_.size(); }

    // The replacement image for `hash`, loaded on first use and then kept, or null when the pack
    // has none (or its file cannot be read, which is reported once).
    const png::Image* find(const TextureInfo& info, std::uint64_t hash);
    // Writes `pixels` (the decoded top level) unless this hash was dumped before.
    void dump(const TextureInfo& info, std::uint64_t hash, const std::vector<std::uint32_t>& pixels);

    // Bytes of decoded replacement images kept in memory; beyond this budget an image is loaded
    // again from disk each time the cache asks (after a palette change, say).
    std::size_t retain_budget = std::size_t{1} << 30;

private:
    struct Loaded {
        bool failed = false;
        png::Image image;
    };
    std::unordered_map<std::uint64_t, std::filesystem::path> index_;
    std::unordered_map<std::uint64_t, std::unique_ptr<Loaded>> loaded_;
    std::size_t retained_bytes_ = 0;
    png::Image scratch_;  // an image over the budget lives here until the next load
    std::filesystem::path dump_dir_;
    std::unordered_set<std::uint64_t> dumped_;
};

}  // namespace dream::render
