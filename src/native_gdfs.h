// Native gdFs: Sega's Katana file library, replaced by native code reading the disc directly.
#pragma once

namespace dream {
class System;
}

namespace rez {

// Once, before the first guest instruction.
void native_gdfs_start(dream::System& sys);

}  // namespace rez
