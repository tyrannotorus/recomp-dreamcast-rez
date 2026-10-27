// The cycle charge for guest code at the CPU's speed.
//
// Emitted code and hooks charge `cost(n)` for work the SH-4 would take n cycles to do. At speed 1
// that is n, the SH-4's own timing. At speed K it is n / K rounded, and at least 1, so a loop
// waiting for an event still moves time forward. Devices keep the console's rates at any speed.
#pragma once

#include <array>
#include <cstdint>

namespace dream::sh4 {

namespace detail {
inline std::uint32_t g_speed = 1;
inline std::array<std::uint32_t, 256> g_cost = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < t.size(); ++i) t[i] = i;
    return t;
}();
constexpr std::uint32_t scaled(std::uint32_t n, std::uint32_t k) noexcept {
    if (n == 0 || k <= 1)
        return n;
    const std::uint32_t v = (n + k / 2) / k;
    return v ? v : 1;
}
}  // namespace detail

inline std::uint32_t cost(std::uint32_t n) noexcept {
    return n < detail::g_cost.size() ? detail::g_cost[n] : detail::scaled(n, detail::g_speed);
}

inline unsigned cpu_speed() noexcept {
    return detail::g_speed;
}

// 1 (the SH-4's timing) to 16. Set before the first guest instruction.
inline void set_cpu_speed(unsigned k) noexcept {
    detail::g_speed = k < 1 ? 1 : k > 16 ? 16 : k;
    for (std::uint32_t i = 0; i < detail::g_cost.size(); ++i) detail::g_cost[i] = detail::scaled(i, detail::g_speed);
}

}  // namespace dream::sh4
