// FSCA's hardware-captured coefficients, as Flycast uses them for its sin_table, so results match
// bit for bit. They give sin over the first half-turn; the second half is its negation (sign bit
// flipped) and cos(i) = sin(i + quarter turn). ops.h (fsca_sin_bits) reads them directly.
#include <cstdint>

#include "dream/runtime/sh4/ops.h"

namespace dream::sh4::detail {

extern const std::uint32_t kFscaCoefs[0x8000] = {
#include "fsca_coefs.inc"
};

}  // namespace dream::sh4::detail
