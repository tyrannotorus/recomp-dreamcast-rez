// The disc folder of a NextUI build: each file the player put there, identified against the
// port's [[disc.chd]] images by the SHA-1 its CHD header records.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "dream/translator/config/game_config.h"

namespace dreamcomp::nextui {

struct DiscFile {
    std::filesystem::path path;
    std::string shown;  // the path relative to the folder, as the screens show it
    int image = -1;     // index into the port's chd_images; -1: not one of them
    std::string sha1;   // a readable CHD's header SHA-1 (what chdman info prints as SHA1)
    std::string why;    // when image < 0, why not
};

// Every file in `dir` and its subfolders one level down, sorted by name. Hidden files (macOS's
// "._" copies among them) and notes (*.txt) are skipped; a missing folder holds nothing.
std::vector<DiscFile> scan_disc_folder(const std::filesystem::path& dir,
                                       const std::vector<dream::translator::ChdImage>& images);

}  // namespace dreamcomp::nextui
