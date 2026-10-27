#include "dream/runtime/gdrom/disc.h"
#include "dream/runtime/gdrom/mod_disc.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include <libchdr/chd.h>

namespace dream::gdrom {

// ---- Disc --------------------------------------------------------------------------------------

const Track* Disc::track_for(std::uint32_t lba) const noexcept {
    for (const auto& t : tracks_)
        if (lba >= t.lba && lba < t.end_lba())
            return &t;
    return nullptr;
}

const Track* Disc::hd_track() const noexcept {
    for (const auto& t : tracks_)
        if (t.data && t.lba >= kHdAreaLba)
            return &t;
    for (const auto& t : tracks_)
        if (t.data)
            return &t;
    return nullptr;
}

std::uint32_t Disc::leadout_lba() const noexcept {
    return tracks_.empty() ? 0 : tracks_.back().end_lba();
}

void Disc::finish_tracks() {
    std::sort(tracks_.begin(), tracks_.end(),
              [](const Track& a, const Track& b) { return a.number < b.number; });
    bool hd = false;
    for (const auto& t : tracks_) hd |= t.lba >= kHdAreaLba;
    type_ = hd ? DiscType::GdRom : DiscType::CdRomXa;
}

bool Disc::read_user(std::uint32_t lba, std::uint8_t* out) {
    const Track* t = track_for(lba);
    if (!t)
        return false;
    std::uint8_t raw[2352];
    if (!read_raw(lba, raw))
        return false;
    switch (t->sector_size) {
        case 2048:
            std::memcpy(out, raw, kUserBytes);
            return true;
        case 2352:
            std::memcpy(out, raw + 16, kUserBytes);
            return true;  // mode 1: sync 12, header 4
        case 2336:
            std::memcpy(out, raw + 8, kUserBytes);
            return true;  // mode 2 form 1 without sync
        default:
            return false;
    }
}

void Disc::toc(unsigned area, std::uint32_t out[102]) const {
    for (int i = 0; i < 102; ++i) out[i] = 0xFFFFFFFFu;
    if (tracks_.empty())
        return;
    const bool gd = type_ == DiscType::GdRom;
    if (area == 1 && !gd)
        return;
    std::size_t first = 1, last = tracks_.size();
    if (area == 1)
        first = 3;
    else if (gd)
        last = 2;
    if (first > tracks_.size() || last > tracks_.size())
        return;
    auto entry = [](const Track& t, std::uint32_t low24) {
        const std::uint32_t ctrl_adr = (t.data ? 0x4u : 0x0u) << 4 | 1u;
        return (ctrl_adr << 24) | (low24 & 0xFFFFFFu);
    };
    for (std::size_t i = first; i <= last; ++i)
        out[i - 1] = entry(tracks_[i - 1], tracks_[i - 1].lba + kFadOffset);
    out[99] = entry(tracks_[first - 1], static_cast<std::uint32_t>(first) << 16);
    out[100] = entry(tracks_[last - 1], static_cast<std::uint32_t>(last) << 16);
    const std::uint32_t leadout =
        (gd && area == 0) ? tracks_[1].end_lba() + kFadOffset : leadout_lba() + kFadOffset;
    Track lo;
    lo.data = true;
    out[101] = entry(lo, leadout);
}

// ---- GDI ---------------------------------------------------------------------------------------

namespace {

class GdiDisc final : public Disc {
public:
    bool open(const std::filesystem::path& path, std::string& error) {
        path_ = path.string();
        std::ifstream in(path);
        if (!in) {
            error = "cannot open " + path_;
            return false;
        }
        std::string line;
        if (!std::getline(in, line)) {
            error = "empty GDI";
            return false;
        }
        const auto dir = path.parent_path();
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            unsigned number = 0, lba = 0, type = 0, ssize = 0;
            std::string file;
            std::uint64_t offset = 0;
            if (!(ls >> number >> lba >> type >> ssize))
                continue;
            // The file name may be quoted and contain spaces.
            ls >> std::ws;
            if (ls.peek() == '"') {
                ls.get();
                std::getline(ls, file, '"');
            } else {
                ls >> file;
            }
            ls >> offset;
            const auto fpath = dir / file;
            std::error_code ec;
            const auto fsize = std::filesystem::file_size(fpath, ec);
            if (ec) {
                error = "GDI track file missing: " + fpath.string();
                return false;
            }
            Track t;
            t.number = number;
            t.lba = lba;
            t.sector_size = ssize;
            t.sectors = static_cast<std::uint32_t>((fsize - offset) / ssize);
            t.data = type == 4;
            tracks_.push_back(t);
            files_[number] = {fpath.string(), offset};
        }
        if (tracks_.empty()) {
            error = "GDI lists no tracks";
            return false;
        }
        finish_tracks();
        return true;
    }

    // A Redump-style .cue. Tracks are laid out back to back from LBA 0 in
    // the single-density area; "REM HIGH-DENSITY AREA" restarts the layout at LBA 45000. A
    // track's LBA is its INDEX 01; anything before that in its file (an audio track's INDEX 00
    // pregap) is skipped through the file offset, which is how a GDI describes the same disc.
    bool open_cue(const std::filesystem::path& path, std::string& error) {
        path_ = path.string();
        std::ifstream in(path);
        if (!in) {
            error = "cannot open " + path_;
            return false;
        }
        const auto dir = path.parent_path();
        std::filesystem::path file;
        std::uint32_t file_lba = 0, next_lba = 0;
        struct Pending {
            unsigned number = 0;
            bool data = false;
            std::uint32_t ssize = 2352;
            std::filesystem::path file;
            std::uint32_t file_lba = 0;
        };
        std::vector<Pending> pending;
        std::map<unsigned, std::uint32_t> index01;  // track -> frames into its file
        auto msf = [](const std::string& s) -> std::uint32_t {
            unsigned m = 0, sec = 0, f = 0;
            std::sscanf(s.c_str(), "%u:%u:%u", &m, &sec, &f);
            return (m * 60u + sec) * 75u + f;
        };
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            std::string kw;
            ls >> kw;
            for (auto& ch : kw) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (kw == "REM") {
                std::string rest;
                std::getline(ls, rest);
                if (rest.find("HIGH-DENSITY AREA") != std::string::npos)
                    next_lba = kHdAreaLba;
            } else if (kw == "FILE") {
                ls >> std::ws;
                std::string name;
                if (ls.peek() == '"') {
                    ls.get();
                    std::getline(ls, name, '"');
                } else {
                    ls >> name;
                }
                file = dir / name;
                std::error_code ec;
                const auto fsize = std::filesystem::file_size(file, ec);
                if (ec) {
                    error = "cue track file missing: " + file.string();
                    return false;
                }
                file_lba = next_lba;
                next_lba += static_cast<std::uint32_t>(fsize / 2352);
            } else if (kw == "TRACK") {
                Pending p;
                std::string mode;
                ls >> p.number >> mode;
                p.data = mode != "AUDIO";
                p.ssize = mode.find("2048") != std::string::npos ? 2048u : 2352u;
                p.file = file;
                p.file_lba = file_lba;
                pending.push_back(p);
            } else if (kw == "INDEX" && !pending.empty()) {
                unsigned idx = 0;
                std::string at;
                ls >> idx >> at;
                if (idx == 1)
                    index01[pending.back().number] = msf(at);
            }
        }
        for (const auto& p : pending) {
            const std::uint32_t skip = index01.count(p.number) ? index01[p.number] : 0;
            std::error_code ec;
            const auto fsize = std::filesystem::file_size(p.file, ec);
            const std::uint64_t offset = static_cast<std::uint64_t>(skip) * p.ssize;
            Track t;
            t.number = p.number;
            t.lba = p.file_lba + skip;
            t.sector_size = p.ssize;
            t.sectors = static_cast<std::uint32_t>((fsize - offset) / p.ssize);
            t.data = p.data;
            tracks_.push_back(t);
            files_[p.number] = {p.file.string(), offset};
        }
        if (tracks_.empty()) {
            error = "cue lists no tracks";
            return false;
        }
        finish_tracks();
        return true;
    }

    bool read_raw(std::uint32_t lba, std::uint8_t* out) override {
        const Track* t = track_for(lba);
        if (!t)
            return false;
        auto& f = files_[t->number];
        if (!f.handle) {
            f.handle = std::fopen(f.path.c_str(), "rb");
            if (!f.handle)
                return false;
        }
        const std::uint64_t pos =
            f.offset + static_cast<std::uint64_t>(lba - t->lba) * t->sector_size;
        // 64-bit seek: `long` is 32 bits on Windows and a GD-ROM data track passes 1 GB.
#ifdef _WIN32
        if (_fseeki64(f.handle, static_cast<long long>(pos), SEEK_SET) != 0)
            return false;
#else
        if (fseeko(f.handle, static_cast<off_t>(pos), SEEK_SET) != 0)
            return false;
#endif
        return std::fread(out, 1, t->sector_size, f.handle) == t->sector_size;
    }

    ~GdiDisc() override {
        for (auto& [n, f] : files_)
            if (f.handle)
                std::fclose(f.handle);
    }

private:
    struct File {
        std::string path;
        std::uint64_t offset = 0;
        FILE* handle = nullptr;
    };
    std::map<unsigned, File> files_;
};

// ---- CHD ---------------------------------------------------------------------------------------

constexpr std::uint32_t kFrameBytes = 2448;  // 2352 sector + 96 subchannel
constexpr std::uint32_t kTrackPadding = 4;   // chdman pads each track to a multiple of 4 frames

class ChdDisc final : public Disc {
public:
    struct ChdTrack {
        Track track;
        std::uint32_t chd_frame = 0;  // frame index of the track's first sector in the CHD
    };

    bool open(const std::filesystem::path& path, std::string& error) {
        path_ = path.string();
        const chd_error err = chd_open(path_.c_str(), CHD_OPEN_READ, nullptr, &chd_);
        if (err != CHDERR_NONE) {
            error = std::string("chd_open: ") + chd_error_string(err);
            return false;
        }
        const chd_header* h = chd_get_header(chd_);
        hunk_bytes_ = h->hunkbytes;
        frames_per_hunk_ = hunk_bytes_ / kFrameBytes;
        if (frames_per_hunk_ == 0) {
            error = "CHD hunk smaller than one CD frame";
            return false;
        }
        hunk_buf_.resize(hunk_bytes_);
        return parse_tracks(error);
    }

    bool read_raw(std::uint32_t lba, std::uint8_t* out) override {
        for (const auto& ct : layout_) {
            const Track& t = ct.track;
            if (lba < t.lba || lba >= t.end_lba())
                continue;
            const std::uint32_t frame = ct.chd_frame + (lba - t.lba);
            const std::uint8_t* f = frame_ptr(frame);
            if (!f)
                return false;
            std::memcpy(out, f, t.sector_size);
            if (!t.data)  // chdman stores audio big-endian; the disc is little-endian
                for (std::uint32_t i = 0; i + 1 < t.sector_size; i += 2)
                    std::swap(out[i], out[i + 1]);
            return true;
        }
        return false;
    }

    ~ChdDisc() override {
        if (worker_.joinable()) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stop_ = true;
            }
            cv_.notify_all();
            worker_.join();
        }
        if (ahead_chd_)
            chd_close(ahead_chd_);
        if (chd_)
            chd_close(chd_);
    }

    // Read-ahead: the hunks holding the sectors are unpacked by a worker with its own handle on
    // the file into a small cache, which frame_ptr takes them from.
    void prefetch(std::uint32_t lba, std::uint32_t count) override {
        if (count == 0)
            return;
        std::uint32_t first = 0xFFFFFFFFu, last = 0;
        for (std::uint32_t s = lba; s < lba + count; ++s) {
            const std::uint32_t frame = chd_frame(s);
            if (frame == 0xFFFFFFFFu)
                continue;
            first = std::min(first, frame / frames_per_hunk_);
            last = std::max(last, frame / frames_per_hunk_);
        }
        if (first == 0xFFFFFFFFu)
            return;
        if (!worker_.joinable()) {
            if (chd_open(path_.c_str(), CHD_OPEN_READ, nullptr, &ahead_chd_) != CHDERR_NONE) {
                ahead_chd_ = nullptr;
                return;
            }
            cache_.resize(kAheadHunks);
            for (auto& c : cache_) c.data.resize(hunk_bytes_);
            worker_ = std::thread([this] { work(); });
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (std::uint32_t h = first; h <= last && wanted_.size() < kAheadHunks; ++h)
                if (h != cached_hunk_ && !in_cache(h) &&
                    std::find(wanted_.begin(), wanted_.end(), h) == wanted_.end())
                    wanted_.push_back(h);
        }
        cv_.notify_all();
    }

private:
    static constexpr std::size_t kAheadHunks = 64;  // 1.2 MB of 19,584-byte hunks
    struct AheadHunk {
        std::uint32_t hunk = 0xFFFFFFFFu;
        std::uint64_t stamp = 0;
        std::vector<std::uint8_t> data;
    };
    bool in_cache(std::uint32_t hunk) const {
        for (const auto& c : cache_)
            if (c.hunk == hunk)
                return true;
        return false;
    }
    void work() {
        std::vector<std::uint8_t> buf(hunk_bytes_);
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            cv_.wait(lock, [this] { return stop_ || !wanted_.empty(); });
            if (stop_)
                return;
            const std::uint32_t hunk = wanted_.front();
            wanted_.pop_front();
            if (in_cache(hunk))
                continue;
            decoding_ = hunk;
            lock.unlock();
            const bool ok = chd_read(ahead_chd_, hunk, buf.data()) == CHDERR_NONE;
            lock.lock();
            decoding_ = 0xFFFFFFFFu;
            if (ok) {
                AheadHunk* oldest = &cache_[0];
                for (auto& c : cache_)
                    if (c.stamp < oldest->stamp)
                        oldest = &c;
                oldest->hunk = hunk;
                oldest->stamp = ++stamp_;
                oldest->data.swap(buf);
            }
            cv_.notify_all();
        }
    }
    // A hunk the read-ahead has (waited for while it unpacks it), into `out`; false if not there.
    bool take_ahead(std::uint32_t hunk, std::uint8_t* out) {
        if (!worker_.joinable())
            return false;
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return decoding_ != hunk; });
        for (auto& c : cache_)
            if (c.hunk == hunk) {
                std::memcpy(out, c.data.data(), hunk_bytes_);
                c.stamp = ++stamp_;
                return true;
            }
        // Not unpacked yet: the reader unpacks it, and the worker need not.
        const auto it = std::find(wanted_.begin(), wanted_.end(), hunk);
        if (it != wanted_.end())
            wanted_.erase(it);
        return false;
    }
    // A sector's frame in the CHD, or 0xFFFFFFFF outside every track.
    std::uint32_t chd_frame(std::uint32_t lba) const {
        for (const auto& ct : layout_)
            if (lba >= ct.track.lba && lba < ct.track.end_lba())
                return ct.chd_frame + (lba - ct.track.lba);
        return 0xFFFFFFFFu;
    }

    chd_file* ahead_chd_ = nullptr;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::uint32_t> wanted_;
    std::vector<AheadHunk> cache_;
    std::uint32_t decoding_ = 0xFFFFFFFFu;
    std::uint64_t stamp_ = 0;
    bool stop_ = false;

    bool parse_tracks(std::string& error) {
        struct Meta {
            unsigned track = 0;
            std::string type, subtype, pgtype;
            std::uint32_t frames = 0, pad = 0, pregap = 0, postgap = 0;
            bool gd = false;
        };
        std::vector<Meta> metas;
        const std::uint32_t tags[] = {CDROM_TRACK_METADATA_TAG, CDROM_TRACK_METADATA2_TAG,
                                      GDROM_OLD_METADATA_TAG, GDROM_TRACK_METADATA_TAG};
        char buf[512];
        for (std::uint32_t tag : tags) {
            for (std::uint32_t idx = 0;; ++idx) {
                std::uint32_t len = 0, rtag = 0;
                std::uint8_t flags = 0;
                if (chd_get_metadata(chd_, tag, idx, buf, sizeof buf - 1, &len, &rtag, &flags) !=
                    CHDERR_NONE)
                    break;
                buf[std::min<std::uint32_t>(len, sizeof buf - 1)] = 0;
                Meta m;
                m.gd = tag == GDROM_OLD_METADATA_TAG || tag == GDROM_TRACK_METADATA_TAG;
                std::istringstream ts(buf);
                std::string tok;
                while (ts >> tok) {
                    const auto colon = tok.find(':');
                    if (colon == std::string::npos)
                        continue;
                    const std::string k = tok.substr(0, colon), v = tok.substr(colon + 1);
                    if (k == "TRACK")
                        m.track = static_cast<unsigned>(std::stoul(v));
                    else if (k == "TYPE")
                        m.type = v;
                    else if (k == "SUBTYPE")
                        m.subtype = v;
                    else if (k == "FRAMES")
                        m.frames = static_cast<std::uint32_t>(std::stoul(v));
                    else if (k == "PAD")
                        m.pad = static_cast<std::uint32_t>(std::stoul(v));
                    else if (k == "PREGAP")
                        m.pregap = static_cast<std::uint32_t>(std::stoul(v));
                    else if (k == "PGTYPE")
                        m.pgtype = v;
                    else if (k == "POSTGAP")
                        m.postgap = static_cast<std::uint32_t>(std::stoul(v));
                }
                if (m.track)
                    metas.push_back(m);
            }
        }
        if (metas.empty()) {
            error = "CHD has no CD track metadata";
            return false;
        }
        std::sort(metas.begin(), metas.end(),
                  [](const Meta& a, const Meta& b) { return a.track < b.track; });
        std::uint32_t chd_frame = 0, lba = 0;
        for (const auto& m : metas) {
            // A pregap whose type starts with 'V' has its frames stored in the CHD; otherwise it is
            // implied and only shifts the LBA. GDI-derived images have neither.
            const bool stored_pregap = !m.pgtype.empty() && m.pgtype[0] == 'V';
            if (!stored_pregap)
                lba += m.pregap;
            ChdTrack ct;
            ct.track.number = m.track;
            ct.track.lba = lba;
            ct.track.sectors = m.frames > m.pad ? m.frames - m.pad : 0;  // PAD frames are the gap
            ct.track.data = m.type != "AUDIO";
            ct.track.sector_size = m.type == "AUDIO" || m.type.find("RAW") != std::string::npos ||
                                           m.type.find("2352") != std::string::npos
                                       ? 2352
                                       : (m.type.find("2336") != std::string::npos ? 2336 : 2048);
            ct.chd_frame = chd_frame + (stored_pregap ? m.pregap : 0);
            layout_.push_back(ct);
            tracks_.push_back(ct.track);
            const std::uint32_t padded =
                (m.frames + kTrackPadding - 1) / kTrackPadding * kTrackPadding;
            chd_frame += padded;
            lba += m.frames + m.postgap;
        }
        finish_tracks();
        return true;
    }

    const std::uint8_t* frame_ptr(std::uint32_t frame) {
        const std::uint32_t hunk = frame / frames_per_hunk_;
        if (hunk != cached_hunk_) {
            const bool ok = take_ahead(hunk, hunk_buf_.data()) || chd_read(chd_, hunk, hunk_buf_.data()) == CHDERR_NONE;
            if (!ok)
                return nullptr;
            cached_hunk_ = hunk;
        }
        return hunk_buf_.data() + static_cast<std::size_t>(frame % frames_per_hunk_) * kFrameBytes;
    }

    chd_file* chd_ = nullptr;
    std::uint32_t hunk_bytes_ = 0, frames_per_hunk_ = 0;
    std::uint32_t cached_hunk_ = 0xFFFFFFFFu;
    std::vector<std::uint8_t> hunk_buf_;
    std::vector<ChdTrack> layout_;
};

}  // namespace

bool read_root_file(Disc& disc, std::string name, std::vector<std::uint8_t>& out,
                    std::string* resolved_name) {
    const Track* hd = disc.hd_track();
    if (!hd)
        return false;
    std::uint8_t sec[kUserBytes];
    auto upper = [](std::string s) {
        for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return s;
    };
    if (name.empty()) {  // IP.BIN boot file name, 16 bytes at 0x60 of the first HD sector
        if (!disc.read_user(hd->lba, sec))
            return false;
        name.assign(reinterpret_cast<const char*>(sec + 0x60), 16);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\0')) name.pop_back();
    }
    name = upper(name);
    std::vector<IsoEntry> files;
    if (!list_iso9660(disc, files))
        return false;
    for (const auto& e : files) {
        if (e.dir || upper(e.path) != name)
            continue;
        out.resize(e.size);
        for (std::uint32_t done = 0; done < e.size; done += kUserBytes) {
            if (!disc.read_user(e.lba + done / kUserBytes, sec))
                return false;
            std::memcpy(out.data() + done, sec, std::min<std::uint32_t>(kUserBytes, e.size - done));
        }
        if (resolved_name)
            *resolved_name = name;
        return true;
    }
    return false;
}

std::string chd_header_sha1(const std::filesystem::path& path, std::string& error) {
    chd_file* f = nullptr;
    const chd_error err = chd_open(path.string().c_str(), CHD_OPEN_READ, nullptr, &f);
    if (err != CHDERR_NONE) {
        error = std::string("not a CHD (") + chd_error_string(err) + ")";
        return {};
    }
    static const char kHex[] = "0123456789abcdef";
    std::string hex;
    const chd_header* h = chd_get_header(f);
    for (unsigned i = 0; i < CHD_SHA1_BYTES; ++i) {
        hex += kHex[h->sha1[i] >> 4];
        hex += kHex[h->sha1[i] & 15];
    }
    chd_close(f);
    return hex;
}

std::unique_ptr<Disc> open_disc(const std::filesystem::path& path, std::string& error) {
    std::string ext = path.extension().string();
    for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ext == ".gdi") {
        auto d = std::make_unique<GdiDisc>();
        return d->open(path, error) ? std::unique_ptr<Disc>(std::move(d)) : nullptr;
    }
    if (ext == ".cue") {
        auto d = std::make_unique<GdiDisc>();
        return d->open_cue(path, error) ? std::unique_ptr<Disc>(std::move(d)) : nullptr;
    }
    if (ext == ".chd") {
        auto d = std::make_unique<ChdDisc>();
        return d->open(path, error) ? std::unique_ptr<Disc>(std::move(d)) : nullptr;
    }
    error = "unsupported disc image type: " + ext;
    return nullptr;
}

}  // namespace dream::gdrom
