// What a port tells dreamcomp about its game. One registration per executable, from the port's
// own sources:
//
//     static const dreamcomp::PortInfo kInfo = [] {
//         dreamcomp::PortInfo p;
//         p.id = "rez";
//         p.title = "Rez";
//         p.widescreen = &apply_widescreen;
//         return p;
//     }();
//     static dreamcomp::RegisterPort reg(kInfo);
//
// Every field is optional except id/title; a missing capability simply hides that option.
#pragma once

#include <cstdint>

namespace dream {
class System;
}

namespace dreamcomp {

class Settings;

struct PortInfo {
    const char* id = nullptr;     // names its config and pipeline list, e.g. "rez" (rez.toml)
    // The per-user folder under the platform's config directory: settings, memory card,
    // bindings, pipeline cache, mods and textures. Null: `id`.
    const char* folder = nullptr;
    const char* title = nullptr;  // shown in the window title and menus
    unsigned players = 4;         // player cards in the Controls tab (1..4, Maple ports A-D)
    // The default CPU speed (--cpu-speed, dream/runtime/sh4/cost.h): 1 is the console's timing;
    // more lets frames that overran a VBlank on the SH-4 finish in time. The `cpu_speed` setting
    // and the flag override it.
    unsigned cpu_speed = 1;
    // NextUI builds: the sentence on the no-disc screen telling the player what to copy into the
    // disc folder shown under it. Null: a generic sentence over the image list.
    const char* disc_request = nullptr;
    // NextUI builds: how many pictures the game has put on screen so far (its framebuffer flips),
    // so the "Starting ..." screen gives way at the first. Null: the PowerVR model's count.
    std::uint64_t (*pictures_shown)() = nullptr;
    // NextUI builds: the page shown from the game's end until NextUI draws its menu again. Null:
    // none, and the panel shows the page it held when the game started.
    const char* exit_message = nullptr;

    // Widescreen. Called every vblank on the guest thread with the target aspect ratio (4/3 means
    // off: restore the original), so it can re-assert values the game recomputes. `anamorphic`
    // means the game still renders into its 640x480 framebuffer with a horizontally squeezed
    // view: the engine then draws into a render target of the target's shape (--render-aspect),
    // which undoes the squeeze at full horizontal resolution. `max_aspect` caps what the port has
    // been verified with.
    void (*widescreen)(dream::System& sys, float aspect) = nullptr;
    bool widescreen_anamorphic = true;
    float max_aspect = 32.0f / 9.0f;

    // Widescreen HUD correction (anamorphic ports). The game draws its 2D HUD in 640x480 screen
    // space, so the horizontal stretch that widens the 3D view also widens the HUD. Primitives
    // the rule matches are scaled back around the screen centre: the HUD keeps its original 4:3
    // layout and proportions, centred. Measured per game; off when !enabled.
    // The rule: a sprite (PCW para type 5) drawn at a single depth is HUD when another sprite in
    // the same frame shares that exact depth (2D layers are drawn at a common 1/w; 3D particles
    // each have their own), or when its depth is at least `overlay_z` (a fixed overlay layer).
    struct HudRule {
        bool enabled = false;
        unsigned min_shared = 2;      // sprites at one depth for that depth to count as a 2D layer
        float overlay_z = 1000.0f;    // 1/w at or above this is always a 2D overlay
        float full_width = 600.0f;    // wider than this: a fade or flash covering the screen; left
        // Flat polygons too, not only sprites (dreamcomp): some 2D screens draw their pieces as
        // ordinary polygons at one depth.
        bool polygons = false;
        // A depth whose flat pieces together cover more than this share of the 640x480 screen is
        // a backdrop (tiled full-screen art),
        // not HUD; it is left to fill the widened picture (dreamcomp).
        float backdrop_cover = 0.6f;
    };
    HudRule hud;

    // Anything else the port wants each vblank (cheats, debug overlays...).
    void (*on_vblank)(dream::System& sys, Settings& settings) = nullptr;
    // Once, before the first guest instruction.
    void (*on_start)(dream::System& sys, Settings& settings) = nullptr;
    // Once, after the guest stopped and before the launcher's report, while the System is still
    // alive (unlike in an atexit handler): for a port's own counts.
    void (*on_stop)(dream::System& sys, Settings& settings) = nullptr;
};

const PortInfo* port();
Settings& settings();
// The resolution the game is drawn at, as a multiple of 640x480: the setting `scale`, or this
// build's default. set_render_scale saves it and applies it from the next frame. kScalePanel (the
// setting "panel"): the largest 4:3 picture the display's own pixels hold. render_size gives a
// scale's size in pixels. render_scale_high: the higher of a two-way choice against 640x480, the
// panel's own on a NextUI handheld and 2 (1280x960) elsewhere.
constexpr int kScalePanel = 0;
int render_scale();
void set_render_scale(int scale);
void render_size(int scale, int& w, int& h);
int render_scale_high();

struct RegisterPort {
    explicit RegisterPort(const PortInfo& info);
};

}  // namespace dreamcomp
