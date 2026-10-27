// Widescreen HUD correction with per-element overrides.
//
// With a widened picture, the 2D HUD is found in each frame's display list (PortInfo::HudRule) and
// moved back to its 4:3 proportions, each element anchored to the left edge, the right edge or
// the centre. Overrides fix what the heuristic gets wrong: a rectangle in the console's 640x480
// coordinates (before any correction, so it holds at every window shape), the texture words of
// the element's pieces, and the anchor to use. A piece whose centre lies in the rectangle and
// whose texture is listed is forced into the HUD with that anchor -- or left stretched -- which is
// also how a 2D element the heuristic missed is added. The F1 editor (src/frontend/hud_editor.cpp)
// shows the result and edits the overrides.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "dreamcomp/port.h"

namespace dream::render {
struct Frame;
}

namespace dreamcomp::hud {

enum class Anchor : std::uint8_t { Auto, Left, Center, Right, Stretch };
const char* name(Anchor a) noexcept;  // "auto", "left", "center", "right", "stretch"
bool parse(const std::string& s, Anchor& out) noexcept;

struct Override {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // console coordinates, before correction
    // Texture words of the pieces; empty: any. 0 stands for untextured pieces: their TCW slot
    // holds leftover data that changes from frame to frame, so it is never compared.
    std::vector<std::uint32_t> tcws;
    Anchor anchor = Anchor::Auto;
    // `full=1`: also applies to pieces wider than the rule's full_width. Off by default, so a
    // large rectangle never pulls a fade or a full-screen backdrop into the HUD. For a
    // full-screen picture that is really 4:3 art.
    bool full = false;
    bool covers(float cx, float cy, std::uint32_t tcw) const noexcept;
};

// Later entries win, so a player's file loaded after the port's overrides it.
struct Overrides {
    std::vector<Override> items;
    // Appends the file's entries (missing file: nothing, true). Lines:
    //   rect=x0,y0,x1,y1 anchor=left|center|right|stretch [tcw=0x...,0x...] [full=1]  # comment
    bool load(const std::filesystem::path& file, std::string* error = nullptr);
    bool save(const std::filesystem::path& file, std::string* error = nullptr) const;
    // Index, or -1. `wide`: the piece is wider than full_width (only `full` overrides match).
    int match(float cx, float cy, std::uint32_t tcw, bool wide = false) const noexcept;
};

// What the editor shows: one box per element (pieces grouped as the correction groups them) and,
// for adding, one per flat 2D piece the correction left alone.
struct Box {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;      // where it is drawn (after correction)
    float ox0 = 0, oy0 = 0, ox1 = 0, oy1 = 0;  // before correction
    Anchor anchor = Anchor::Auto;  // resolved: left, center, right or stretch (never auto)
    bool hud = false;              // corrected (or forced) as HUD
    int override_index = -1;       // the override that decided it, or -1
    std::vector<std::uint32_t> tcws;
};
struct Snapshot {
    std::vector<Box> boxes;
    // Every HUD piece on its own (the F2 list): its rectangle, its texture word, the anchor its
    // element got. `boxes` groups them into elements (the F1 outlines).
    std::vector<Box> pieces;
    float image_aspect = 4.0f / 3.0f;  // the picture's shape (the 640x480 space spans it)
    bool edges = true;                 // HUD layout: anchored to edges (else centred 4:3)
    std::uint64_t frame = 0;  // a new number per snapshot (also while the game is paused)
    // Filled by the host (Core) for the editor's panel.
    std::size_t port_overrides = 0, user_overrides = 0;
    bool unsaved = false;
    std::string status, user_file;
};

// What a click in the editor does to the element (or 2D piece) `box`.
enum class Edit : std::uint8_t {
    Cycle,  // next anchor: left -> center -> right -> stretch -> left
    Reset,  // back to what the port (or the heuristic) does
    Add,    // a piece the heuristic left alone becomes HUD, anchored automatically
};

struct Stats {
    std::uint64_t corrected = 0;
};

// One frame's pass: classifies with `rule`, applies `overrides`, moves the HUD vertices for a
// picture of `aspect` (k = (4/3) / aspect) when `apply`, and fills `snapshot` when it is not null.
// Without `apply` (a 4:3 picture: nothing to correct) the frame is left exactly as it was, and the
// snapshot shows where each element would be anchored.
void correct(dream::render::Frame& frame, const PortInfo::HudRule& rule, float aspect, bool edges,
             bool apply, const Overrides& overrides, Snapshot* snapshot, Stats& stats);

}  // namespace dreamcomp::hud
