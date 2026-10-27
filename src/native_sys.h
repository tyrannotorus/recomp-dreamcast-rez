// Native devices: Shinobi's hardware-facing functions, replaced by native code.
#pragma once

namespace dream {
class System;
}

namespace rez {

// Once, before the first guest instruction.
void native_sys_start(dream::System& sys);

// 60 (the default: every logic step drawn) or 30 (the original's pacing in the stages).
void native_sys_frame_rate(unsigned fps);

}  // namespace rez
