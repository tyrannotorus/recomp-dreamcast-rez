#include "dream/runtime/gdrom/mod_disc.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>

namespace dream::gdrom {

namespace {

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0] | p[1] << 8 | p[2] << 16) |
           static_cast<std::uint32_t>(p[3]) << 24;
}

void put_both32(std::uint8_t* p, std::uint32_t v) {  // ISO9660 both-endian field
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<std::uint8_t>(v >> (8 * i));
        p[7 - i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

std::string upper(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

}  // namespace

bool list_iso9660(Disc& disc, std::vector<IsoEntry>& out) {
    out.clear();
    const Track* hd = disc.hd_track();
    if (!hd)
        return false;
    std::uint8_t sec[kUserBytes];
    if (!disc.read_user(hd->lba + 16, sec) || std::memcmp(sec + 1, "CD001", 5) != 0)
        return false;
    const std::uint32_t root_ext = le32(sec + 156 + 2), root_size = le32(sec + 156 + 10);
    std::int64_t fix = 0;
    bool found = false;
    for (std::int64_t cand : {std::int64_t{0}, -std::int64_t{kFadOffset}, std::int64_t{hd->lba},
                              std::int64_t{hd->lba} - std::int64_t{kFadOffset}}) {
        const std::int64_t at = root_ext + cand;
        if (at < 0 || !disc.read_user(static_cast<std::uint32_t>(at), sec))
            continue;
        if (le32(sec + 2) == root_ext && sec[32] == 1 && sec[33] == 0) {
            fix = cand;
            found = true;
            break;
        }
    }
    if (!found)
        return false;
    std::function<bool(std::uint32_t, std::uint32_t, const std::string&, int)> walk =
        [&](std::uint32_t ext, std::uint32_t size, const std::string& prefix, int depth) {
            if (depth > 16)
                return false;
            for (std::uint32_t off = 0; off < size; off += kUserBytes) {
                const auto lba = static_cast<std::uint32_t>(ext + fix + off / kUserBytes);
                std::uint8_t s[kUserBytes];
                if (!disc.read_user(lba, s))
                    return false;
                for (std::uint32_t pos = 0; pos + 33 < kUserBytes && s[pos] != 0; pos += s[pos]) {
                    const std::uint8_t* r = s + pos;
                    if (r[32] == 1 && (r[33] == 0 || r[33] == 1))
                        continue;  // "." and ".."
                    std::string name(reinterpret_cast<const char*>(r + 33), r[32]);
                    name = name.substr(0, name.find(';'));
                    IsoEntry e;
                    e.path = prefix.empty() ? name : prefix + "/" + name;
                    e.lba = static_cast<std::uint32_t>(le32(r + 2) + fix);
                    e.size = le32(r + 10);
                    e.dir = (r[25] & 2) != 0;
                    e.record_lba = lba;
                    e.record_off = pos;
                    out.push_back(e);
                    if (e.dir && !walk(le32(r + 2), e.size, e.path, depth + 1))
                        return false;
                }
            }
            return true;
        };
    return walk(root_ext, root_size, "", 0);
}

std::unique_ptr<Disc> ModDisc::wrap(std::unique_ptr<Disc> base,
                                    const std::vector<std::filesystem::path>& roots,
                                    std::string& log) {
    std::vector<IsoEntry> files;
    if (!base || roots.empty() || !list_iso9660(*base, files))
        return base;
    std::map<std::string, const IsoEntry*> by_name;
    for (const auto& e : files)
        if (!e.dir)
            by_name[upper(e.path)] = &e;

    auto mod = std::unique_ptr<ModDisc>(new ModDisc());
    const Track* hd = base->hd_track();
    std::uint32_t next_free = hd->end_lba();
    std::map<std::string, bool> taken;  // first root wins
    std::size_t replaced = 0;
    for (const auto& root : roots) {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec))
            continue;
        for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (ec || !it->is_regular_file())
                continue;
            std::string rel = std::filesystem::relative(it->path(), root, ec).generic_string();
            const std::string key = upper(rel);
            if (taken[key])
                continue;
            const auto f = by_name.find(key);
            if (f == by_name.end()) {
                log += "mods: ignored " + rel + " (not on the disc)\n";
                continue;
            }
            taken[key] = true;
            const IsoEntry& orig = *f->second;
            const auto size = static_cast<std::uint32_t>(it->file_size(ec));
            const std::uint32_t orig_sectors = (orig.size + kUserBytes - 1) / kUserBytes;
            const std::uint32_t new_sectors = (size + kUserBytes - 1) / kUserBytes;
            std::uint32_t lba = orig.lba;
            if (new_sectors > orig_sectors) {
                lba = next_free;
                next_free += new_sectors;
            }
            for (std::uint32_t i = 0; i < new_sectors; ++i) {
                Source s;
                s.path = it->path().string();
                s.offset = static_cast<std::uint64_t>(i) * kUserBytes;
                s.valid = std::min<std::uint32_t>(kUserBytes, size - i * kUserBytes);
                mod->sources_[lba + i] = s;
            }
            // The rest of the original allocation reads as zeros, not as the old file's tail.
            for (std::uint32_t i = new_sectors; lba == orig.lba && i < orig_sectors; ++i)
                mod->patched_[lba + i] = std::vector<std::uint8_t>(kUserBytes, 0);
            // Patch the directory record (extent in the disc's own convention, and size).
            auto& dir = mod->patched_[orig.record_lba];
            if (dir.empty()) {
                dir.resize(kUserBytes);
                base->read_user(orig.record_lba, dir.data());
            }
            std::uint8_t* rec = dir.data() + orig.record_off;
            const std::uint32_t old_ext = le32(rec + 2);
            put_both32(rec + 2, old_ext + (lba - orig.lba));
            put_both32(rec + 10, size);
            ++replaced;
            log += "mods: " + rel + " <- " + it->path().string() + " (" + std::to_string(size) +
                   " bytes" + (lba == orig.lba ? ", in place" : ", relocated") + ")\n";
        }
    }
    if (!replaced)
        return base;
    mod->tracks_ = base->tracks();
    for (auto& t : mod->tracks_)
        if (t.number == hd->number)
            t.sectors = next_free - t.lba;
    mod->type_ = base->type();
    mod->path_ = base->path();
    mod->base_ = std::move(base);
    return mod;
}

void ModDisc::make_raw(std::uint32_t lba, const std::uint8_t* user, std::uint8_t* out) const {
    const Track* t = track_for(lba);
    if (t && t->sector_size == 2352) {
        // Mode 1 raw sector: sync, MSF header, mode byte, then user data. EDC/ECC are left zero:
        // the HLE reads user data only.
        std::memset(out, 0, 2352);
        std::memset(out + 1, 0xFF, 10);
        const std::uint32_t fad = lba + kFadOffset;
        auto bcd = [](std::uint32_t v) { return static_cast<std::uint8_t>((v / 10) << 4 | v % 10); };
        out[12] = bcd(fad / 75 / 60);
        out[13] = bcd(fad / 75 % 60);
        out[14] = bcd(fad % 75);
        out[15] = 1;
        std::memcpy(out + 16, user, kUserBytes);
    } else if (t && t->sector_size == 2336) {
        std::memset(out, 0, 2336);
        std::memcpy(out + 8, user, kUserBytes);
    } else {
        std::memcpy(out, user, kUserBytes);
    }
}

bool ModDisc::read_raw(std::uint32_t lba, std::uint8_t* out) {
    if (const auto p = patched_.find(lba); p != patched_.end()) {
        make_raw(lba, p->second.data(), out);
        return true;
    }
    if (const auto s = sources_.find(lba); s != sources_.end()) {
        std::uint8_t user[kUserBytes] = {};
        std::FILE*& f = open_[s->second.path];
        if (!f)
            f = std::fopen(s->second.path.c_str(), "rb");
        if (!f)
            return false;
#ifdef _WIN32
        _fseeki64(f, static_cast<long long>(s->second.offset), SEEK_SET);
#else
        fseeko(f, static_cast<off_t>(s->second.offset), SEEK_SET);
#endif
        if (std::fread(user, 1, s->second.valid, f) != s->second.valid)
            return false;
        make_raw(lba, user, out);
        return true;
    }
    return base_->read_raw(lba, out);
}

ModDisc::~ModDisc() {
    for (auto& [p, f] : open_)
        if (f)
            std::fclose(f);
}

}  // namespace dream::gdrom
