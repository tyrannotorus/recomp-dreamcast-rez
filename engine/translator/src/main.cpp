// dream-translate: translates a game's SH-4 code to C++ as its TOML config describes.
//
//   dream-translate --version
//   dream-translate game --config ID.toml --out-dir DIR [--shards N]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "dream/translator/analysis/discover.h"
#include "dream/translator/config/game_config.h"
#include "dream/translator/emit.h"
#include "dream/translator/version.h"

namespace {

int usage(int code) {
    std::fprintf(stderr,
                 "dream-translate %s\n"
                 "usage:\n"
                 "  dream-translate --version\n"
                 "  dream-translate game --config ID.toml --out-dir DIR [--shards N]\n",
                 dream::translator::version().data());
    return code;
}

bool load_image(const std::string& path, std::uint32_t base, dream::translator::Image& img) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    img.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    img.base = base;
    return true;
}

// Maps an address given in the config (load-address spelling) into the image's link space.
std::uint32_t to_link_space(const dream::translator::GameConfig& cfg, std::uint32_t a) {
    return (a & 0x1FFFFFFFu) - (cfg.load_address & 0x1FFFFFFFu) + cfg.link_address;
}

// Translate one image view (the main binary or a relocated copy) into DIR/<stem>.{cpp,h}.
int translate_view(const dream::translator::GameConfig& cfg, const dream::translator::Image& img,
                   const std::vector<std::uint32_t>& seeds, const std::string& stem,
                   const std::filesystem::path& out_dir, const dream::translator::EmitOptions& opt,
                   const std::map<std::uint32_t, std::string>& hooks,
                   std::map<std::uint32_t, std::string>* exported = nullptr,
                   const std::vector<std::uint32_t>& late_seeds = {}) {
    dream::translator::DiscoverOptions dopt;
    dopt.seeds = seeds;
    dopt.late_seeds = late_seeds;
    dopt.follow_pointers = cfg.pointers;
    dopt.aggressive_sweep = cfg.sweep;
    auto r = dream::translator::discover(img, dopt);
    if (!cfg.exclude.empty()) {
        std::vector<dream::translator::DiscoveredFunction> kept;
        for (const auto& f : r.functions) {
            bool ex = false;
            for (std::uint32_t x : cfg.exclude) ex |= to_link_space(cfg, x) == f.entry;
            if (!ex)
                kept.push_back(f);
        }
        r.functions = kept;
    }
    auto fns = dream::translator::to_specs(r);
    // [hooks]: "name" hooks entry and exit, "name@entry" / "name@exit" one side. The main image
    // view takes [hooks]; a relocated view takes its own entry's `hooks`. An address that is not a
    // function entry in the view is fatal: a hook must never silently fail to fire.
    {
        for (const auto& [addr, spec] : hooks) {
            const std::uint32_t at = to_link_space(cfg, addr);
            std::string name = spec;
            bool entry = true, exit = true;
            if (const auto pos = spec.find('@'); pos != std::string::npos) {
                name = spec.substr(0, pos);
                const std::string side = spec.substr(pos + 1);
                entry = side == "entry";
                exit = side == "exit";
                if (!entry && !exit) {
                    std::fprintf(stderr, "[hooks] 0x%08x: \"%s\": side must be @entry or @exit\n",
                                 addr, spec.c_str());
                    return 2;
                }
            }
            bool found = false;
            for (auto& f : fns) {
                if (f.entry != at)
                    continue;
                f.hook = name;
                f.hook_entry = entry;
                f.hook_exit = exit;
                found = true;
            }
            if (!found) {
                std::fprintf(stderr,
                             "[hooks] 0x%08x (%s): not the entry of any discovered function; "
                             "seed it in [functions] extra or fix the address\n",
                             addr, name.c_str());
                return 2;
            }
        }
    }
    if (opt.overlay) {
        // Several overlay units can translate the same address: keep their symbols distinct.
        const auto pos = stem.find("_reloc_");
        const std::string suffix = "_" + (pos == std::string::npos ? stem : stem.substr(pos + 7));
        for (auto& f : fns) f.name += suffix;
    }
    if (exported)
        for (const auto& f : fns) (*exported)[f.entry] = f.name;
    const auto res = dream::translator::emit_unit(img, fns, opt);
    const auto base = out_dir / stem;
    {
        std::ofstream o(base.string() + ".cpp");
        o << res.source;
        for (std::size_t k = 0; k < res.parts.size(); ++k) {
            char suffix[32];
            std::snprintf(suffix, sizeof suffix, "_part%02zu.cpp", k);
            std::ofstream p(base.string() + suffix);
            p << res.parts[k];
        }
        std::ofstream h(base.string() + ".h");
        h << res.header;
    }
    return 0;
}

int cmd_game(int argc, char** argv) {
    std::string config, out_dir;
    dream::translator::EmitOptions opt;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--config") && i + 1 < argc)
            config = argv[++i];
        else if (!std::strcmp(argv[i], "--out-dir") && i + 1 < argc)
            out_dir = argv[++i];
        else if (!std::strcmp(argv[i], "--shards") && i + 1 < argc)
            opt.shards = static_cast<unsigned>(std::max(1, std::atoi(argv[++i])));
        else
            return usage(2);
    }
    if (config.empty() || out_dir.empty())
        return usage(2);
    dream::translator::GameConfig cfg;
    std::string err;
    if (!dream::translator::load_game_config(config, cfg, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    dream::translator::Image img;
    if (!load_image(cfg.binary_path.string(), cfg.link_address, img)) {
        std::fprintf(stderr, "cannot read %s\n", cfg.binary_path.string().c_str());
        return 2;
    }
    std::filesystem::create_directories(out_dir);
    std::vector<std::uint32_t> seeds{to_link_space(cfg, cfg.entry)};
    for (std::uint32_t a : cfg.extra_functions) seeds.push_back(to_link_space(cfg, a));
    // The addresses a relocation's bytes cover: the code that runs there may be another unit's.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> covered;
    for (const auto& rel : cfg.relocations) {
        std::uint64_t size = rel.size;
        if (!size && !rel.file.empty()) {
            std::error_code ec;
            const auto bytes = std::filesystem::file_size(rel.file, ec);
            size = ec || bytes <= rel.source ? 0x01000000u : bytes - rel.source;  // unknown: all of RAM
        }
        const std::uint32_t lo = rel.dest & 0x1FFFFFFFu;
        covered.emplace_back(lo, static_cast<std::uint32_t>(std::min<std::uint64_t>(lo + size, 0x20000000u)));
    }
    auto is_covered = [&covered](std::uint32_t a) {
        const std::uint32_t phys = a & 0x1FFFFFFFu;
        for (const auto& [lo, hi] : covered)
            if (phys >= lo && phys < hi)
                return true;
        return false;
    };
    // Relocated copies: the same bytes viewed at the address they execute from, and the entries
    // each is translated from.
    struct View {
        dream::translator::Image image;
        std::vector<std::uint32_t> seeds;
    };
    std::vector<View> views;
    for (const auto& rel : cfg.relocations) {
        View v;
        dream::translator::Image& view = v.image;
        view.base = (rel.dest & 0x1FFFFFFFu) | (cfg.link_address & 0xE0000000u);
        if (!rel.file.empty()) {
            // An overlay the program reads from disc: the bytes are the file's, from `source`.
            dream::translator::Image file;
            if (!load_image(rel.file.string(), view.base, file)) {
                std::fprintf(stderr, "cannot read %s\n", rel.file.string().c_str());
                return 2;
            }
            const std::size_t size =
                rel.size ? rel.size
                         : (file.bytes.size() > rel.source ? file.bytes.size() - rel.source : 0);
            if (size == 0 || rel.source + size > file.bytes.size()) {
                std::fprintf(stderr, "relocation %s+0x%x size 0x%zx is outside the file\n",
                             rel.file.string().c_str(), rel.source, size);
                return 2;
            }
            view.bytes.assign(file.bytes.begin() + rel.source,
                              file.bytes.begin() + rel.source + size);
        } else {
            const std::uint32_t off = rel.source - img.base;
            if (rel.source < img.base || off + rel.size > img.bytes.size()) {
                std::fprintf(stderr, "relocation source 0x%08x+0x%x is outside the image\n",
                             rel.source, rel.size);
                return 2;
            }
            view.bytes.assign(img.bytes.begin() + off, img.bytes.begin() + off + rel.size);
        }
        for (std::uint32_t e : rel.entries)
            v.seeds.push_back((e & 0x1FFFFFFFu) | (cfg.link_address & 0xE0000000u));
        if (v.seeds.empty())
            v.seeds.push_back(view.base);
        views.push_back(std::move(v));
    }
    // The views' calls into the main image are its functions too: a view's call through a constant
    // outside itself, to main-image code no relocation covers, even where nothing in the main image
    // reaches it. Walked as late seeds, so they only add to the main image's discovery.
    std::vector<std::uint32_t> view_calls;
    for (const View& v : views) {
        dream::translator::DiscoverOptions dopt;
        dopt.seeds = v.seeds;
        dopt.follow_pointers = cfg.pointers;
        dopt.aggressive_sweep = cfg.sweep;
        for (std::uint32_t t : dream::translator::discover(v.image, dopt).external_targets) {
            const std::uint32_t a = to_link_space(cfg, t);
            if ((a & 1u) || !img.contains(a, 2) || is_covered(a) ||
                std::find(view_calls.begin(), view_calls.end(), a) != view_calls.end())
                continue;
            view_calls.push_back(a);
        }
    }
    std::map<std::uint32_t, std::string> main_fns;
    int rc =
        translate_view(cfg, img, seeds, cfg.id, out_dir, opt, cfg.hooks, &main_fns, view_calls);
    // What the relocated views may call by name (EmitOptions::imports): the main image's
    // functions, but none at an address a relocation's bytes cover.
    std::map<std::uint32_t, std::string> imports;
    for (const auto& [entry, name] : main_fns)
        if (!is_covered(entry))
            imports.emplace(entry, name);
    // A destination that appears more than once (overlays) numbers its later units _2, _3, ...;
    // the game's CMakeLists derives the same names from the TOML.
    std::map<std::uint32_t, int> dest_count;
    for (std::size_t i = 0; i < cfg.relocations.size(); ++i) {
        const auto& rel = cfg.relocations[i];
        const dream::translator::Image& view = views[i].image;
        const std::vector<std::uint32_t>& seeds_r = views[i].seeds;
        char stem[80];
        const int nth = ++dest_count[rel.dest];
        if (nth == 1)
            std::snprintf(stem, sizeof stem, "%s_reloc_%08x", cfg.id.c_str(), rel.dest);
        else
            std::snprintf(stem, sizeof stem, "%s_reloc_%08x_%d", cfg.id.c_str(), rel.dest, nth);
        dream::translator::EmitOptions view_opt = opt;
        view_opt.shards = 1;  // relocated copies are small; only the main unit is split
        if (rel.no_fold)
            view_opt.fold_literals = false;
        view_opt.overlay = rel.overlay;
        view_opt.imports = &imports;
        rc |= translate_view(cfg, view, seeds_r, stem, out_dir, view_opt, rel.hooks);
    }
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && (!std::strcmp(argv[1], "--version") || !std::strcmp(argv[1], "-V"))) {
        std::printf("dream-translate %s\n", dream::translator::version().data());
        return 0;
    }
    if (argc >= 2 && !std::strcmp(argv[1], "game"))
        return cmd_game(argc, argv);
    return usage(argc >= 2 ? 2 : 0);
}
