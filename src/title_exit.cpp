// "exit" at the bottom of Rez's main menu: choosing it ends the run as closing the window does.
// The title overlay's menu table (0x8C3042BC, nine 32-byte entries: six label pointers, the id at
// +24, an availability function at +28) ends with two debug entries hidden by 0x8C3030DE, "REPLAY"
// (id 9) and "DEMO" (id 10). REPLAY is shown as "exit", and choosing it ends the run instead of
// starting the debug replay. The setting exit_item (default on) turns it off.
#include "title_exit.h"
#include "recomp_settings.h"

#include <cstdint>
#include <cstdio>

#include "dream/runtime/host_ext.h"
#include "dream/runtime/memory.h"
#include "dream/runtime/sh4/ctx.h"
#include "dreamcomp/hook.h"

namespace rez {
namespace {

constexpr std::uint32_t kTable = 0x8C3042BCu;           // the title overlay's menu entries
constexpr std::uint32_t kExitEntry = kTable + 7 * 32u;  // "REPLAY" (id 9), last but one
constexpr std::uint32_t kExitId = 9;
constexpr std::uint32_t kReturnOne = 0x8C014EA4u;       // `rts; mov #1,r0` in the main program

bool g_on = true;
std::uint32_t g_menu = 0;  // the input function's menu state (its r4)

// Shows the REPLAY entry, its label (a string only it points to) rewritten to "exit", while it is
// still REPLAY or already exit. Not DEMO: the menu's drawing calls DEMO's availability function
// directly after its loop and draws debug information when it says yes.
void show_exit(dream::Memory& m) {
    if (m.read32(kExitEntry + 24) != kExitId)
        return;
    const std::uint32_t label = m.read32(kExitEntry + 4);  // the English label; all six point to it
    static constexpr char kReplay[5] = {'R', 'E', 'P', 'L', 'A'}, kExit[5] = {'e', 'x', 'i', 't', 0};
    bool replay = true, done = true;
    for (unsigned i = 0; i < 5; ++i) {
        const char ch = static_cast<char>(m.read8(label + i));
        replay &= ch == kReplay[i];
        done &= ch == kExit[i];
    }
    if (!replay && !done)
        return;
    if (replay)
        for (unsigned i = 0; i < 5; ++i) m.write8(label + i, static_cast<std::uint8_t>(kExit[i]));
    if (m.read32(kExitEntry + 28) != kReturnOne)
        m.write32(kExitEntry + 28, kReturnOne);  // always shown
}

}  // namespace

void title_exit_start(bool on) {
    g_on = on;
    std::printf("rez: main menu exit item %s\n", on ? "on" : "off");
}

}  // namespace rez

// 0x8C3026C4 (r4 = the menu's state): the menu's input. Before it: the exit entry shown. After it:
// a choice of the exit entry (id 9) ends the run.
DC_HOOK_ENTRY(title_menu_input) {
    rez::g_menu = c.r[4];
    if (rez::g_on)
        rez::show_exit(m);
    rez::recomp_menu_begin(m, c.r[4]);  // "recomp settings" (recomp_settings.cpp)
    return false;
}
DC_HOOK_EXIT(title_menu_input) {
    rez::recomp_menu_chosen(m);
    rez::recomp_menu_end(m);
    if (!rez::g_on || !rez::g_menu || m.read32(rez::g_menu + 12) != rez::kExitId)
        return;
    m.write32(rez::g_menu + 12, 0);  // nothing chosen: the title carries on until the run ends
    std::printf("rez: exit chosen in the main menu\n");
    // The launcher's own stop (host_controls().quit), at the next frame, window or not.
    auto& hc = dream::host::host_controls();
    if (hc.quit)
        hc.quit();
}

// 0x8C302876 (r4 = the menu's state, as the input's): the menu's drawing, which walks the same
// entries; before it the exit entry shown and the "recomp settings" order, after it the order
// put back.
DC_HOOK_ENTRY(title_menu_draw) {
    if (rez::g_on)
        rez::show_exit(m);
    rez::recomp_menu_begin(m, c.r[4]);
    return false;
}
DC_HOOK_EXIT(title_menu_draw) {
    (void)c;
    rez::recomp_menu_end(m);
}
