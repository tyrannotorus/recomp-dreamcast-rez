// Native peripherals: Rez's input, rumble, VMU screen and save libraries, replaced at library level
// with no Maple bus underneath.
#pragma once

namespace dream {
class System;
}

namespace rez {

// Once, before the first guest instruction.
void native_periph_start(dream::System& sys);

}  // namespace rez
