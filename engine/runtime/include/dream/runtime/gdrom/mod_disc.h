// File-replacement mods on top of a disc image.
//
// A mod root mirrors the disc's ISO9660 tree: `<root>/STAGE.DAT` replaces the disc's STAGE.DAT,
// `<root>/DATA/FOO.BIN` replaces DATA/FOO.BIN (names match case-insensitively). The game keeps
// reading sectors through the GD-ROM HLE exactly as before; ModDisc serves different bytes:
//
// - a replacement that fits in the original file's sectors is served at the original LBAs, and
//   its directory record's size is patched (games that locate files by a fixed LBA still work);
// - a larger one is placed after the end of the data track, which ModDisc extends, and its
//   directory record's extent and size are patched (games that locate files through the
//   filesystem -- gdFs does -- follow it).
//
// Files that are not on the disc are ignored (adding files needs directory growth: not done).
// Roots are applied in order; the first root providing a file wins.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dream/runtime/gdrom/disc.h"

namespace dream::gdrom {

// One file or directory of a disc's ISO9660 filesystem (on a GD-ROM, the high-density one).
struct IsoEntry {
    std::string path;          // "STAGE.DAT", "DATA/FOO.BIN" (no ";1")
    std::uint32_t lba = 0;     // absolute LBA of the first sector
    std::uint32_t size = 0;    // bytes
    bool dir = false;
    std::uint32_t record_lba = 0;  // where its directory record lives
    std::uint32_t record_off = 0;  // byte offset of the record in that sector
};

// Walks the filesystem. Extents may be absolute LBAs, frame addresses (LBA + 150) or relative to
// the data track (all three exist in real dumps and tools); the convention whose root "." record
// points at itself is used. False if there is no readable ISO9660 filesystem.
bool list_iso9660(Disc& disc, std::vector<IsoEntry>& out);

class ModDisc final : public Disc {
public:
    // Wraps `base`. `log` receives one line per replaced/ignored file. Returns `base` unchanged
    // (no wrapper) when no root provides any file.
    static std::unique_ptr<Disc> wrap(std::unique_ptr<Disc> base,
                                      const std::vector<std::filesystem::path>& roots,
                                      std::string& log);

    bool read_raw(std::uint32_t lba, std::uint8_t* out) override;
    ~ModDisc() override;

private:
    struct Source {
        std::string path;
        std::uint64_t offset = 0;  // byte offset in the mod file for this sector
        std::uint32_t valid = 0;   // bytes of real data in the sector (rest is zero)
    };
    std::unique_ptr<Disc> base_;
    std::map<std::uint32_t, std::vector<std::uint8_t>> patched_;  // lba -> 2048 user bytes
    std::map<std::uint32_t, Source> sources_;                     // lba -> mod file bytes
    std::map<std::string, std::FILE*> open_;
    void make_raw(std::uint32_t lba, const std::uint8_t* user, std::uint8_t* out) const;
};

}  // namespace dream::gdrom
