// Native sound-bus guards: Shinobi's G2 suspend, resume and FIFO waits around sound-RAM access,
// and syG2's DMA into sound RAM, replaced by native code.
#pragma once

namespace dream {
class System;
}

namespace rez {

// Once, before the first guest instruction.
void native_snd_start(dream::System& sys);

}  // namespace rez
