// A minimal PNG codec for texture dumps and replacement packs. Written from the PNG
// (RFC 2083 / ISO 15948), zlib (RFC 1950) and deflate (RFC 1951) specifications; no third-party
// code. Device-independent.
//
//   encode   8-bit RGBA, non-interlaced. Each row takes whichever of the five filters gives the
//            smallest sum of absolute differences, and the stream is one fixed-Huffman deflate
//            block with greedy LZ77 matching: far smaller than stored blocks, far simpler than an
//            optimal encoder, and only ever run once per texture.
//   decode   8-bit greyscale, grey+alpha, RGB, RGBA and palette (with tRNS), non-interlaced, all
//            five filter types, a full inflate (stored, fixed and dynamic Huffman blocks). Chunk
//            CRCs and the zlib Adler-32 are checked. Everything else (16-bit, sub-byte depths,
//            Adam7) is refused with a message rather than misread.
//
// Pixels are packed as the texture decoder packs them: r | g << 8 | b << 16 | a << 24, which is
// R8G8B8A8 in memory on a little-endian host.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dream::render::png {

struct Image {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint32_t> pixels;  // width * height, row 0 at the top
};

// The largest image either direction accepts, in each dimension. Replacement textures are a few
// times the guest's 1024 at most; this only stops a corrupt header allocating gigabytes.
constexpr std::uint32_t kMaxDimension = 16384;

std::vector<std::uint8_t> encode(const std::uint32_t* pixels, std::uint32_t width,
                                 std::uint32_t height);
bool decode(const std::uint8_t* data, std::size_t size, Image& out, std::string* error = nullptr);

bool write_file(const std::filesystem::path& path, const std::uint32_t* pixels,
                std::uint32_t width, std::uint32_t height);
bool read_file(const std::filesystem::path& path, Image& out, std::string* error = nullptr);

// The pieces: checksums, and a zlib stream in and out.
std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0);
std::uint32_t adler32(const std::uint8_t* data, std::size_t size, std::uint32_t adler = 1);
// A zlib stream: header, deflate data, Adler-32.
std::vector<std::uint8_t> zlib_compress(const std::uint8_t* data, std::size_t size);
bool zlib_decompress(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out,
                     std::size_t expected_size = 0, std::string* error = nullptr);

}  // namespace dream::render::png
