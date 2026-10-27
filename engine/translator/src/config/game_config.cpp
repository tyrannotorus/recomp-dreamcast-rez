#include "dream/translator/config/game_config.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <toml.hpp>

namespace dream::translator {
namespace {

bool get_u32(const toml::node_view<const toml::node>& n, const char* field, std::uint32_t& out,
             std::string& error, bool required) {
    if (!n) {
        if (required)
            error = std::string(field) + " is required";
        return !required;
    }
    const auto v = n.value<std::int64_t>();
    if (!v || *v < 0 || *v > 0xFFFFFFFFll) {
        error = std::string(field) + " must be an unsigned 32-bit integer";
        return false;
    }
    out = static_cast<std::uint32_t>(*v);
    return true;
}

bool get_str(const toml::node_view<const toml::node>& n, const char* field, std::string& out,
             std::string& error, bool required) {
    if (!n) {
        if (required)
            error = std::string(field) + " is required";
        return !required;
    }
    const auto v = n.value<std::string>();
    if (!v) {
        error = std::string(field) + " must be a string";
        return false;
    }
    out = *v;
    return true;
}

// [[disc.chd]] (sha1, name, label) and the single [disc] chd_sha1.
bool get_chd_images(const toml::table& t, std::vector<ChdImage>& out, std::string& error) {
    const auto sha1 = [&](std::string s, const char* field, std::string& hex) {
        for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (s.size() != 40 || s.find_first_not_of("0123456789abcdef") != std::string::npos) {
            error = std::string(field) + " must be 40 hex digits";
            return false;
        }
        hex = s;
        return true;
    };
    std::string one;
    if (!get_str(t["disc"]["chd_sha1"], "disc.chd_sha1", one, error, false))
        return false;
    if (!one.empty()) {
        ChdImage img;
        if (!sha1(one, "disc.chd_sha1", img.sha1))
            return false;
        out.push_back(img);
    }
    const auto n = t["disc"]["chd"];
    if (!n)
        return true;
    const auto* arr = n.as_array();
    if (!arr) {
        error = "disc.chd must be an array of tables ([[disc.chd]])";
        return false;
    }
    for (const auto& e : *arr) {
        const auto* tbl = e.as_table();
        if (!tbl) {
            error = "disc.chd entries must be tables with sha1 and name";
            return false;
        }
        ChdImage img;
        std::string s;
        if (!get_str((*tbl)["sha1"], "disc.chd.sha1", s, error, true) || !sha1(s, "disc.chd.sha1", img.sha1) ||
            !get_str((*tbl)["name"], "disc.chd.name", img.name, error, false) ||
            !get_str((*tbl)["label"], "disc.chd.label", img.label, error, false))
            return false;
        out.push_back(img);
    }
    return true;
}

bool get_u32_list(const toml::node_view<const toml::node>& n, const char* field,
                  std::vector<std::uint32_t>& out, std::string& error) {
    if (!n)
        return true;
    const auto* arr = n.as_array();
    if (!arr) {
        error = std::string(field) + " must be an array of addresses";
        return false;
    }
    for (const auto& e : *arr) {
        const auto v = e.value<std::int64_t>();
        if (!v || *v < 0 || *v > 0xFFFFFFFFll) {
            error = std::string(field) + " entries must be unsigned 32-bit integers";
            return false;
        }
        out.push_back(static_cast<std::uint32_t>(*v));
    }
    return true;
}

bool get_addr_map(const toml::node_view<const toml::node>& n, const char* field,
                  std::map<std::uint32_t, std::string>& out, std::string& error) {
    if (!n)
        return true;
    const auto* tbl = n.as_table();
    if (!tbl) {
        error = std::string(field) + " must be a table of \"0xADDRESS\" = \"name\"";
        return false;
    }
    for (const auto& [k, v] : *tbl) {
        const std::string key(k.str());
        char* end = nullptr;
        const unsigned long a = std::strtoul(key.c_str(), &end, 0);
        const auto name = v.value<std::string>();
        if (!end || *end != '\0' || key.empty() || !name) {
            error =
                std::string(field) + ": key " + key + " must be an address and the value a name";
            return false;
        }
        out[static_cast<std::uint32_t>(a)] = *name;
    }
    return true;
}

bool finish(const toml::table& t, const std::filesystem::path& dir, GameConfig& c,
            std::string& error) {
    c.dir = dir;
    if (!get_str(t["game"]["id"], "game.id", c.id, error, true) ||
        !get_str(t["game"]["title"], "game.title", c.title, error, true) ||
        !get_str(t["game"]["region"], "game.region", c.region, error, false) ||
        !get_str(t["game"]["product"], "game.product", c.product, error, false) ||
        !get_str(t["disc"]["sha1_1st_read"], "disc.sha1_1st_read", c.sha1_1st_read, error, false) ||
        !get_chd_images(t, c.chd_images, error))
        return false;
    std::string disc_image;
    if (!get_str(t["disc"]["image"], "disc.image", disc_image, error, false))
        return false;
    if (!disc_image.empty())
        c.disc_image = dir / disc_image;
    for (char ch : c.id)
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-')) {
            error = "game.id must be [A-Za-z0-9_-]+";
            return false;
        }
    std::string bin;
    if (!get_str(t["binary"]["path"], "binary.path", bin, error, true) ||
        !get_u32(t["binary"]["load_address"], "binary.load_address", c.load_address, error, true) ||
        !get_u32(t["binary"]["link_address"], "binary.link_address", c.link_address, error,
                 false) ||
        !get_u32(t["binary"]["entry"], "binary.entry", c.entry, error, true))
        return false;
    c.binary_path = dir / bin;
    if (!t["binary"]["link_address"])
        c.link_address = c.load_address;
    if ((c.load_address & 0x1FFFFFFFu) != (c.link_address & 0x1FFFFFFFu)) {
        error = "binary.load_address and binary.link_address must alias the same RAM";
        return false;
    }
    if (c.load_address & 1 || c.entry & 1) {
        error = "binary addresses must be even";
        return false;
    }
    if (const auto* relocs = t["relocations"].as_array()) {
        for (const auto& e : *relocs) {
            const auto* rt = e.as_table();
            if (!rt) {
                error = "relocations entries must be tables";
                return false;
            }
            Relocation r;
            const toml::table& rv = *rt;  // GCC finds node_view(const table&) ambiguous
            std::string file;
            if (!get_str(rv["file"], "relocations.file", file, error, false))
                return false;
            const bool from_file = !file.empty();  // an overlay read from disc at run time
            if (from_file)
                r.file = dir / file;
            if (!get_u32(rv["source"], "relocations.source", r.source, error, !from_file) ||
                !get_u32(rv["size"], "relocations.size", r.size, error, !from_file) ||
                !get_u32(rv["dest"], "relocations.dest", r.dest, error, true))
                return false;
            std::uint32_t en = 0;
            if (rv["entry"]) {
                if (!get_u32(rv["entry"], "relocations.entry", en, error, true))
                    return false;
                r.entries.push_back(en);
            }
            if (!get_u32_list(rv["entries"], "relocations.entries", r.entries, error))
                return false;
            r.no_fold = rv["no_fold"].value_or(false);
            r.overlay = rv["overlay"].value_or(false);
            if (!get_addr_map(rv["hooks"], "relocations.hooks", r.hooks, error))
                return false;
            c.relocations.push_back(r);
        }
    }
    if (!get_u32_list(t["functions"]["extra"], "functions.extra", c.extra_functions, error) ||
        !get_u32_list(t["functions"]["exclude"], "functions.exclude", c.exclude, error))
        return false;
    c.pointers = t["functions"]["pointers"].value_or(true);
    c.sweep = t["functions"]["sweep"].value_or(true);
    if (!get_addr_map(t["hle"], "hle", c.hle, error) ||
        !get_addr_map(t["hooks"], "hooks", c.hooks, error))
        return false;
    return true;
}

}  // namespace

bool parse_game_config(const std::string& text, const std::filesystem::path& dir, GameConfig& out,
                       std::string& error) {
    toml::parse_result r = toml::parse(text);
    if (!r) {
        std::ostringstream o;
        o << "TOML parse error: " << r.error().description() << " at line "
          << r.error().source().begin.line;
        error = o.str();
        return false;
    }
    out = GameConfig{};
    return finish(r.table(), dir, out, error);
}

bool load_game_config(const std::filesystem::path& path, GameConfig& out, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path.string();
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!parse_game_config(text, path.parent_path(), out, error)) {
        error = path.string() + ": " + error;
        return false;
    }
    return true;
}

}  // namespace dream::translator
