// "recomp settings" in Rez's main menu, under "options".
#pragma once

#include <cstdint>

#include "dream/runtime/memory.h"

namespace rez {

// Once, before the first guest instruction: on (the default) or off (the original menu).
void recomp_settings_start(bool on);

// The main menu's input and drawing (title_exit.cpp's hooks), around each call: the menu table in
// the order with the item, and the cursor in that order; then put back. `menu`: the menu's state
// (r4 at the call's entry; kept until recomp_menu_end, as r4 is not at the exit).
void recomp_menu_begin(dream::Memory& m, std::uint32_t menu);
void recomp_menu_end(dream::Memory& m);
// After the input, before recomp_menu_end: the item chosen opens the page (the options screen with
// the page's tables).
void recomp_menu_chosen(dream::Memory& m);

}  // namespace rez
