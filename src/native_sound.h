// Native sound: Rez's sound driver and the sound chip replaced by native code below the sd
// library's command packets.
#pragma once

namespace dream {
class System;
namespace mem {
class MmioHandler;
}
}  // namespace dream

namespace rez {

// The sound chip's register block as the game sees it; the device layer maps it before start.
dream::mem::MmioHandler* native_sound_regs();

// Once, before the first guest instruction.
void native_sound_start(dream::System& sys);
// At the run's stop (PortInfo::on_stop): the sound made up to the present.
void native_sound_stop();

}  // namespace rez
