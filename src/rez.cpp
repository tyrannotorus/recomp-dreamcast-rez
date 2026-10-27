// Rez: the port's description, settings and hooks.
#include <cstdint>
#include <cstdlib>

#include "dream/runtime/system.h"
#include "dreamcomp/hook.h"
#include "dreamcomp/port.h"
#include "dreamcomp/settings.h"
#include "native_dev.h"
#include "native_gdfs.h"
#include "native_gfx.h"
#include "native_irq.h"
#include "native_periph.h"
#include "native_sound.h"
#include "native_snd.h"
#include "native_sys.h"
#include "recomp_settings.h"
#include "title_exit.h"

namespace {

// Widescreen (REZ_WIDESCREEN=1, off by default): Rez's projection setup (0x8C0C5CB8) hands its
// helper (0x8C01DB2C) the horizontal factor, width * 1/640, in fr4 (the vertical in fr5). For a
// picture A wide the hook scales fr4 by (4/3)/A, so the view widens while the game still renders
// into its 640x480 framebuffer (anamorphic); the engine renders to a wider target to undo the
// squeeze. The engine reports the aspect each vblank.
bool g_widescreen = false;
float g_aspect = 4.0f / 3.0f;

void widescreen(dream::System&, float aspect) {
    g_aspect = aspect;
}

const dreamcomp::PortInfo kInfo = [] {
    dreamcomp::PortInfo p;
    p.id = "rez";
    p.folder = "rez_recompiled";
    p.title = "Rez";
    // Guest code is charged a quarter of the SH-4's cycle estimates: the headroom to draw every
    // logic step at 60 fps (native_sys.cpp). --cpu-speed 1 (or the setting cpu_speed=1) runs the
    // console's own timing.
    p.cpu_speed = 4;
    // The Brick's no-disc screen: the two images game/rez.toml accepts.
    p.disc_request = "Copy the Japanese or European .chd releases of Rez into this folder on the SD card:";
    // Its "Starting Rez..." screen gives way at Rez's first framebuffer flip.
    p.pictures_shown = rez::native_gfx_frame_swaps;
    // Shown on leaving, until NextUI draws again: a nod to Rez's lore.
    p.exit_message = "Exiting the system...";
    if (const char* w = std::getenv("REZ_WIDESCREEN"); w && w[0] == '1') {
        g_widescreen = true;
        p.widescreen = widescreen;
        p.widescreen_anamorphic = true;
        p.max_aspect = 16.0f / 9.0f;
        // The HUD: Rez draws it as textured sprites at a fixed depth (1/w = 2.22: the life
        // squares, the overdrive gauge), while scene sprites (the horizon, the reticle) often share
        // a scene depth. So only the depth threshold decides; the shared-depth test is off, as it
        // would take the horizon for HUD.
        p.hud.enabled = true;
        p.hud.overlay_z = 2.0f;
        p.hud.min_shared = 1u << 30;
    }
    // The sound up to the stop, while the System is still alive.
    p.on_stop = [](dream::System&, dreamcomp::Settings&) { rez::native_sound_stop(); };
    p.on_start = [](dream::System& sys, dreamcomp::Settings& settings) {
        // frame_rate: 60 by default, 30 for the original's pacing.
        rez::native_sys_frame_rate(static_cast<unsigned>(settings.get_int("frame_rate", 60)));
        // exit_item: "exit" in the main menu, on by default.
        rez::title_exit_start(settings.get_bool("exit_item", true));
        // recomp_settings_item: "recomp settings" under "options", on by default.
        rez::recomp_settings_start(settings.get_bool("recomp_settings_item", true));
        rez::native_gfx_start(sys);
        rez::native_sys_start(sys);
        rez::native_snd_start(sys);
        rez::native_dev_start(sys);
        rez::native_gdfs_start(sys);
        rez::native_periph_start(sys);
        rez::native_sound_start(sys);
        rez::native_irq_start(sys);
    };
    return p;
}();
dreamcomp::RegisterPort g_register(kInfo);

}  // namespace

DC_HOOK_ENTRY(projection) {  // 0x8C01DB2C: (fr4 horizontal factor, fr5 vertical factor)
    if (g_widescreen && g_aspect > 0.0f)
        c.fr[4] *= (4.0f / 3.0f) / g_aspect;
    return false;
}
