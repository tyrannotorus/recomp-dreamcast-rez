// "recomp settings" in Rez's main menu, under "options": a page built from Rez's own options
// screen with one choice, the resolution Rez draws at (640x480 or the render_scale_high:
// 1024x768 on the Brick, 1280x960 on the PC). The game's tables are changed only while the menu or
// the page runs, and put back after. The setting recomp_settings_item (default on) turns it off.
#include "recomp_settings.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "dream/runtime/sh4/abi.h"
#include "dream/runtime/sh4/cost.h"
#include "dream/runtime/sh4/ctx.h"
#include "dreamcomp/hook.h"
#include "dreamcomp/port.h"

namespace rez {
namespace {

constexpr std::uint32_t kMenu = 0x8C3042BCu;  // nine 32-byte entries
constexpr std::uint32_t kReturnOne = 0x8C014EA4u;  // `rts; mov #1,r0` in the main program
constexpr unsigned kOrder[9] = {1, 2, 3, 4, 0, 5, 6, 7, 8};  // shown slot -> table slot
constexpr unsigned kItem = 4;                                  // where slot 0 is shown
constexpr std::uint32_t kItemId = 2, kOptionsId = 6;

constexpr std::uint32_t kRows = 0x8C3034A4u, kRowBytes = 40;  // 17 rows; the type at +24
constexpr std::uint32_t kHeading = 0x8C303B00u;               // six title pointers
constexpr std::uint32_t kSet0 = 0x8C303B78u;  // heading, choice A, choice B: six pointers each

struct Text {
    std::uint32_t at;
    const char* original;
    const char* page;
};
// Labels of rows the page hides (and sound mode's two), each long enough for its new text.
constexpr Text kSoundMode{0x8C303784u, "sound mode", "resolution"};
constexpr Text kSoundModeHead{0x8C303D1Cu, "sound mode", "resolution"};
constexpr Text kTitle{0x8C303804u, "sound effects vol.", "recomp settings"};
// The page's heading: its frame is a sprite as wide as about ten of the heading's letters.
constexpr Text kHead{0x8C30385Cu, "up/down", "recomp"};
constexpr Text k640{0x8C303974u, "heads up display", "640x480"};
char g_high[14] = "1280x960";  // the higher choice's size, at most as long as "point display"
constexpr Text k1280{0x8C3039C8u, "point display", g_high};

constexpr unsigned kRowCount = 17, kRowWords = kRowBytes / 4;

bool g_on = true, g_page = false, g_menu = false;
std::uint32_t g_menu_state = 0;              // the menu's state, from recomp_menu_begin to _end
std::uint32_t g_table[9][8];                 // the menu table as the game has it, while reordered
std::uint32_t g_rows[kRowCount][kRowWords];  // the options rows, while the page is open
std::uint32_t g_set0[18], g_heading[6];

bool text_is(dream::Memory& m, std::uint32_t a, const char* s) {
    for (std::size_t i = 0;; ++i) {
        const char ch = static_cast<char>(m.read8(a + static_cast<std::uint32_t>(i)));
        if (ch != s[i])
            return false;
        if (!ch)
            return true;
    }
}
void write_text(dream::Memory& m, std::uint32_t a, const char* s) {
    for (std::size_t i = 0;; ++i) {
        m.write8(a + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(s[i]));
        if (!s[i])
            return;
    }
}

// The title overlay as on the European and Japanese discs: its menu table's ids, the options
// screen's labels, rows and heading. Anything else and the item is not offered.
bool overlay_as_read(dream::Memory& m) {
    for (unsigned i = 0; i < 9; ++i)
        if (m.read32(kMenu + 32 * i + 24) != 2 + i)
            return false;
    if (!text_is(m, kSoundMode.at, kSoundMode.original) || !text_is(m, kSoundModeHead.at, kSoundModeHead.original) ||
        !text_is(m, kHead.at, kHead.original) || !text_is(m, k640.at, k640.original) ||
        !text_is(m, k1280.at, k1280.original))
        return false;
    if (!text_is(m, kTitle.at, kTitle.original) && !text_is(m, kTitle.at, kTitle.page))
        return false;
    return m.read32(kRows + 24) == 1 && m.read32(kRows + 36) == 0 && m.read32(kRows + 16 * kRowBytes + 24) == 4;
}

// The page is the options screen with the heading "recomp settings", row 0 "resolution" (sound
// mode's row, choice set 0: "640x480" or the higher size) and row 16 "return" as they were, and the
// rows between empty like the original's separators. The new words overwrite labels of rows the
// page hides.
void page_open(dream::Memory& m) {
    for (unsigned r = 0; r < kRowCount; ++r)
        for (unsigned w = 0; w < kRowWords; ++w) g_rows[r][w] = m.read32(kRows + r * kRowBytes + 4 * w);
    for (unsigned i = 0; i < 6; ++i) g_heading[i] = m.read32(kHeading + 4 * i);
    for (unsigned i = 0; i < 18; ++i) g_set0[i] = m.read32(kSet0 + 4 * i);
    int w = 0, h = 0;
    dreamcomp::render_size(dreamcomp::render_scale_high(), w, h);
    char size[32];
    const int len = w > 0 && h > 0 ? std::snprintf(size, sizeof size, "%dx%d", w, h) : 0;
    if (len > 0 && static_cast<std::size_t>(len) < sizeof g_high)
        std::memcpy(g_high, size, static_cast<std::size_t>(len) + 1);
    for (const Text& t : {kSoundMode, kSoundModeHead, kTitle, kHead, k640, k1280}) write_text(m, t.at, t.page);
    for (unsigned r = 1; r + 1 < kRowCount; ++r)
        for (unsigned word = 0; word < kRowWords; ++word) m.write32(kRows + r * kRowBytes + 4 * word, 0);
    for (unsigned i = 0; i < 6; ++i) {
        m.write32(kRows + 4 * i, kSoundMode.at);
        m.write32(kHeading + 4 * i, kHead.at);
        m.write32(kSet0 + 4 * i, kSoundModeHead.at);
        m.write32(kSet0 + 24 + 4 * i, k640.at);
        m.write32(kSet0 + 48 + 4 * i, k1280.at);
    }
    g_page = true;
    dreamcomp::render_size(dreamcomp::render_scale(), w, h);
    std::printf("rez: recomp settings opened (drawing at %dx%d; the choices 640x480 and %s)\n", w, h, g_high);
}

void page_close(dream::Memory& m) {
    for (unsigned r = 0; r < kRowCount; ++r)
        for (unsigned w = 0; w < kRowWords; ++w) m.write32(kRows + r * kRowBytes + 4 * w, g_rows[r][w]);
    for (unsigned i = 0; i < 6; ++i) m.write32(kHeading + 4 * i, g_heading[i]);
    for (unsigned i = 0; i < 18; ++i) m.write32(kSet0 + 4 * i, g_set0[i]);
    for (const Text& t : {kSoundMode, kSoundModeHead, kTitle, kHead, k640, k1280}) write_text(m, t.at, t.original);
    g_page = false;
}

}  // namespace

void recomp_settings_start(bool on) {
    g_on = on;
    std::printf("rez: recomp settings item %s\n", on ? "on" : "off");
}

// The main menu's table slot 0, "play" (id 2), is never shown (its availability function returns
// 0). While the menu's input and drawing run, the table is in the order travelling, play, score
// attack, options, slot 0 as "recomp settings", how to play, beyond, exit, DEMO, and the cursor
// follows that order; both are put back afterwards, so the places the game sets the cursor keep
// their meaning. Slot 8 stays DEMO: the drawing calls its availability function after its loop.
void recomp_menu_begin(dream::Memory& m, std::uint32_t menu) {
    if (!g_on || g_menu || !menu)
        return;
    if (g_page) {  // back from the page: its tables put back, the cursor on the item
        page_close(m);
        m.write32(menu + 32, 0);
    }
    if (!overlay_as_read(m))
        return;
    for (unsigned s = 0; s < 9; ++s)
        for (unsigned w = 0; w < 8; ++w) g_table[s][w] = m.read32(kMenu + 32 * s + 4 * w);
    write_text(m, kTitle.at, kTitle.page);
    for (unsigned i = 0; i < 9; ++i)
        for (unsigned w = 0; w < 8; ++w) m.write32(kMenu + 32 * i + 4 * w, g_table[kOrder[i]][w]);
    for (unsigned l = 0; l < 6; ++l) m.write32(kMenu + 32 * kItem + 4 * l, kTitle.at);
    m.write32(kMenu + 32 * kItem + 28, kReturnOne);
    const std::uint32_t cursor = m.read32(menu + 32);
    for (unsigned i = 0; i < 9; ++i)
        if (kOrder[i] == cursor) {
            m.write32(menu + 32, i);
            break;
        }
    g_menu = true;
    g_menu_state = menu;
}

void recomp_menu_chosen(dream::Memory& m) {
    if (!g_menu || m.read32(g_menu_state + 12) != kItemId)
        return;
    m.write32(g_menu_state + 12, kOptionsId);  // the options screen opens, as the page
    page_open(m);
}

void recomp_menu_end(dream::Memory& m) {
    if (!g_menu)
        return;
    const std::uint32_t cursor = m.read32(g_menu_state + 32);
    if (cursor < 9)
        m.write32(g_menu_state + 32, kOrder[cursor]);
    for (unsigned s = 0; s < 9; ++s)
        for (unsigned w = 0; w < 8; ++w) m.write32(kMenu + 32 * s + 4 * w, g_table[s][w]);
    if (!g_page)
        write_text(m, kTitle.at, kTitle.original);
    g_menu = false;
}

}  // namespace rez

// 0x8C301464 (set 0's getter, the sound mode's, a trampoline to 0x8C3010DC that only this panel
// calls): while the page is open, the resolution choice (0: 640x480, 1: any other) instead.
DC_HOOK_ENTRY(recomp_get) {
    if (!rez::g_page)
        return false;
    if (c.cycles >= c.next_event) {
        c.pc = 0x8C301464u;
        dream::sh4::deliver_irq(c, m);
    }
    c.r[0] = dreamcomp::render_scale() != 1 ? 1u : 0u;
    c.cycles += dream::sh4::cost(8);
    return true;
}

// 0x8C30146A (set 0's setter, likewise a trampoline to 0x8C3010E6; r4 the choice): while the page
// is open, sets the resolution, saved and applied from the next frame.
DC_HOOK_ENTRY(recomp_set) {
    if (!rez::g_page)
        return false;
    if (c.cycles >= c.next_event) {
        c.pc = 0x8C30146Au;
        dream::sh4::deliver_irq(c, m);
    }
    dreamcomp::set_render_scale(c.r[4] ? dreamcomp::render_scale_high() : 1);
    c.cycles += dream::sh4::cost(8);
    return true;
}
