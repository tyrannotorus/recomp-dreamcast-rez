// Virtual clock and event queue. Time is SH-4 cycles at 200 MHz. Devices register an
// event once and re-arm it with a relative delay; `advance` moves the clock and runs due events in
// deadline order. Everything the runtime does on a timer goes through here, so a run is
// deterministic for a given guest instruction stream.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dream::sched {

constexpr std::uint64_t kSh4Clock = 200'000'000;
constexpr std::uint64_t kNever = ~std::uint64_t{0};

// `late` is how many cycles past the deadline the clock had moved when the event ran (0 when the
// scheduler stopped exactly on it; positive when a coarse `advance` overshot).
using Callback = std::function<void(std::uint64_t now, std::uint64_t late)>;

class Scheduler {
public:
    int add(std::string name, Callback cb);
    // A host-only event changes nothing the guest can see, such as the launcher's watchdog. A
    // jump of the guest's clock over idle time may pass its deadline: it then runs at the next
    // advance, in order, with now() its deadline, as if on time.
    void set_host_only(int id) { host_only_[static_cast<std::size_t>(id)] = 1; }
    // Arms the event `cycles` from now; 0 runs it on the next advance; kNever disarms.
    void request(int id, std::uint64_t cycles);
    void cancel(int id) { request(id, kNever); }
    bool armed(int id) const noexcept { return deadlines_[static_cast<std::size_t>(id)] != kNever; }
    std::uint64_t deadline(int id) const noexcept { return deadlines_[static_cast<std::size_t>(id)]; }

    std::uint64_t now() const noexcept { return now_; }
    std::uint64_t next_deadline() const noexcept;  // kNever when nothing is armed
    // The earliest deadline of the events that are not host-only: how far an idle skip may jump.
    std::uint64_t next_guest_deadline() const noexcept;
    // Runs every event whose deadline is <= target, in order, with now() set to each deadline,
    // then leaves now() == target.
    void advance_to(std::uint64_t target);
    void advance(std::uint64_t cycles) { advance_to(now_ + cycles); }

private:
    struct Event {
        std::string name;  // for a debugger
        Callback cb;
    };
    std::vector<Event> events_;
    // Deadlines apart from the events: the earliest-deadline scan runs for every event
    // fired (~80 000 a second) and touched a whole Event -- string, std::function, stats -- per
    // entry for one number. Same order: ties still go to the lowest id.
    std::vector<std::uint64_t> deadlines_;
    std::vector<std::uint8_t> host_only_;
    std::uint64_t now_ = 0;
    // The earliest deadline, cached between changes: most advance_to calls find nothing due. A
    // request that moves the earliest event later leaves it stale until next asked.
    mutable std::uint64_t earliest_ = kNever;
    mutable bool stale_ = false;
    void refresh() const noexcept;
};

}  // namespace dream::sched
