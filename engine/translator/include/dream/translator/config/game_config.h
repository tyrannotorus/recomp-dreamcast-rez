// Per-game configuration: one TOML file describing the disc, the executable's load and link
// addresses, relocated regions, extra discovery seeds and HLE/hook overrides.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dream::translator {

struct Relocation {
    // Where the bytes come from: by default a slice of the main binary (`source` is a link-space
    // address); with `file`, a separate file the program loads at run time (an overlay read from
    // disc), `source` then being an offset into it (default 0) and `size` 0 meaning "to the end".
    std::filesystem::path file;
    std::uint32_t source = 0;  // link-space address of the first byte copied (offset with `file`)
    std::uint32_t size = 0;
    std::uint32_t dest = 0;              // address the copy executes at
    std::vector<std::uint32_t> entries;  // discovery seeds inside the copy (`entry` or `entries`)
    bool no_fold =
        false;  // read every literal at run time: the copy is a template the program patches
    bool overlay = false;  // other code occupies `dest` at other times: dispatch checks the bytes
    // Guest address -> hook name, as [hooks] for the main program, applied to this relocation's
    // unit only (several overlays can share an address).
    std::map<std::uint32_t, std::string> hooks;
};

// A disc image a port accepts: `[[disc.chd]]` names a CHD by the SHA-1 its header records over
// data and metadata (contents and track layout, independent of compression). When any are listed,
// the launcher and `tools/dc.py setup` refuse every other image. A single [disc] `chd_sha1` is one
// entry without a name.
struct ChdImage {
    std::string sha1;  // 40 lowercase hex digits
    std::string name;  // shown when an image is refused
    std::string label;  // what players call it ("Japanese"), shown on NextUI's disc screens
};

struct GameConfig {
    std::filesystem::path dir;  // directory of the config file; relative paths resolve against it

    std::string id, title, region, product;
    std::string sha1_1st_read;
    std::vector<ChdImage> chd_images;  // [[disc.chd]] and [disc] chd_sha1
    std::optional<std::filesystem::path>
        disc_image;  // [disc] image, resolved; the GDI/CHD to mount

    std::filesystem::path binary_path;  // resolved
    std::uint32_t load_address = 0, link_address = 0, entry = 0;

    std::vector<Relocation> relocations;
    std::vector<std::uint32_t> extra_functions, exclude;
    bool pointers = true, sweep = true;

    std::map<std::uint32_t, std::string> hle;    // guest address -> runtime handler name
    std::map<std::uint32_t, std::string> hooks;  // guest address -> hook name
};

// Loads and validates. On failure returns false and sets `error` to one line naming the field.
bool load_game_config(const std::filesystem::path& path, GameConfig& out, std::string& error);

// Parses from text (for tests and tools); `dir` is what relative paths resolve against.
bool parse_game_config(const std::string& text, const std::filesystem::path& dir, GameConfig& out,
                       std::string& error);

}  // namespace dream::translator
