// Calls emitted code makes rarely (an access's slow path, an interrupt poll that fires, a
// non-local return, a store-queue flush that is not to RAM). With clang the callee keeps every
// register, so the caller's values stay in scratch registers and the hot path saves no
// callee-saved registers for them. GCC has no such convention.
#pragma once

#if defined(__clang__)
#define DREAM_RARE_CALL [[clang::preserve_all]]
#else
#define DREAM_RARE_CALL
#endif
