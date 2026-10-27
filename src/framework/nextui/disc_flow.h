// The disc screens of a NextUI build, shown before the game starts: no supported image in the
// disc folder (what to copy where), two or more (a picker), then "Starting <title>..." from the
// disc's choice until the game's first picture.
#pragma once

#include <cstdint>
#include <filesystem>

namespace dreamcomp::nextui {

struct DiscFlow {
    std::filesystem::path config;    // the port's <id>.toml: its title and [[disc.chd]] images
    std::filesystem::path disc_dir;  // where the player puts the image
    const char* request = nullptr;   // PortInfo::disc_request, or a generic sentence
    const char* exiting = nullptr;   // PortInfo::exit_message: the page shown as the game ends
};

// The image to run, with the start screen left up; or an empty path when the player closed the
// screens (or nothing could be shown; the reason is logged).
std::filesystem::path choose_disc(const DiscFlow& flow);

bool start_screen_up();

// The game has ended (the launcher's stop, before the renderer hands the panel back): the exit
// page (DiscFlow::exiting) goes up at once.
void game_ending();

// The game's first picture, at guest frame `frame`. With a picture of its own (a renderer) the
// start screen closes for it; without one, a page says the game started and how long it took,
// and waits for A or B.
void game_started(std::uint64_t frame, bool has_picture);

}  // namespace dreamcomp::nextui
