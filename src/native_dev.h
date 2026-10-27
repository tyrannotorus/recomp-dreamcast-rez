// Native devices: the device registers Rez's libraries touch, answered by native code in front of
// the memory map.
#pragma once

#include <cstdint>

namespace dream {
class System;
}

namespace rez {

// Once, before the first guest instruction: puts the native device layer in front of the memory map.
void native_dev_start(dream::System& sys);

// True when the native device layer is in front of the memory map.
bool native_dev_on();

// A system block register as native code holds it (offset from 0x005F6800), e.g. LMMODE0/1.
std::uint32_t native_sb_reg(std::uint32_t offset);

}  // namespace rez
