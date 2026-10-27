#include "nextui/disc_folder.h"

#include <algorithm>
#include <cctype>
#include <system_error>

#include "dream/runtime/gdrom/disc.h"

namespace dreamcomp::nextui {

namespace {

std::string lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

std::vector<DiscFile> scan_disc_folder(const std::filesystem::path& dir,
                                       const std::vector<dream::translator::ChdImage>& images) {
    namespace fs = std::filesystem;
    std::vector<DiscFile> out;
    std::error_code ec;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        const std::string name = p.filename().string();
        if (!name.empty() && name[0] == '.') {
            if (it->is_directory(ec))
                it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory(ec)) {
            if (it.depth() >= 1)
                it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec))
            continue;
        DiscFile f;
        f.path = p;
        f.shown = p.lexically_relative(dir).generic_string();
        if (lower(p.extension().string()) != ".chd") {
            f.why = "not a .chd file";
        } else {
            std::string err;
            f.sha1 = dream::gdrom::chd_header_sha1(p, err);
            for (std::size_t i = 0; i < images.size() && !f.sha1.empty(); ++i)
                if (images[i].sha1 == f.sha1)
                    f.image = static_cast<int>(i);
            if (f.sha1.empty())
                f.why = "damaged, or not a CHD";
            else if (f.image < 0)
                f.why = "a different image (SHA-1 " + f.sha1 + ")";
        }
        out.push_back(std::move(f));
    }
    std::sort(out.begin(), out.end(), [](const DiscFile& a, const DiscFile& b) { return a.shown < b.shown; });
    return out;
}

}  // namespace dreamcomp::nextui
