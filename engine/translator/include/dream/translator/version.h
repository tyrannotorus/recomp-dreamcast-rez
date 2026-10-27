#pragma once

#include <string_view>

namespace dream::translator {

// Semantic version of the translator, as `dream-translate --version` prints it.
std::string_view version() noexcept;

}  // namespace dream::translator
