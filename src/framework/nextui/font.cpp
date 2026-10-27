// Noto Sans Bold (SIL OFL 1.1, assets/fonts/OFL.txt), compiled in so the screens need no file
// beside the executable (C23/C++26 #embed, which GCC 15+ and Clang 19+ accept in C++ early).
#include <cstddef>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif

namespace dreamcomp::nextui {

extern const unsigned char kNotoSansBold[] = {
#embed "../../../assets/fonts/NotoSans-Bold.ttf"
};
extern const std::size_t kNotoSansBoldSize = sizeof kNotoSansBold;

}  // namespace dreamcomp::nextui

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
