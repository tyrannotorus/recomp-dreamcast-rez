#include "dream/runtime/sched/scheduler.h"

#include <chrono>
#include <utility>

namespace dream::sched {

int Scheduler::add(std::string name, Callback cb) {
    events_.push_back(Event{std::move(name), std::move(cb)});
    deadlines_.push_back(kNever);
    host_only_.push_back(0);
    return static_cast<int>(events_.size() - 1);
}

void Scheduler::request(int id, std::uint64_t cycles) {
    std::uint64_t& d = deadlines_[static_cast<std::size_t>(id)];
    const std::uint64_t next = cycles == kNever ? kNever : now_ + cycles;
    if (!stale_) {
        if (next < earliest_)
            earliest_ = next;
        else if (d == earliest_ && next != d)
            stale_ = true;  // it may have been the earliest: look again when asked
    }
    d = next;
}

void Scheduler::refresh() const noexcept {
    std::uint64_t best = kNever;
    for (const std::uint64_t d : deadlines_)
        if (d < best)
            best = d;
    earliest_ = best;
    stale_ = false;
}

std::uint64_t Scheduler::next_deadline() const noexcept {
    if (stale_)
        refresh();
    return earliest_;
}

std::uint64_t Scheduler::next_guest_deadline() const noexcept {
    std::uint64_t best = kNever;
    for (std::size_t i = 0; i < deadlines_.size(); ++i)
        if (!host_only_[i] && deadlines_[i] < best)
            best = deadlines_[i];
    return best;
}

void Scheduler::advance_to(std::uint64_t target) {
    if (next_deadline() > target) {  // nothing due: the common case
        if (target > now_)
            now_ = target;
        return;
    }
    for (;;) {
        const std::size_t n = deadlines_.size();
        std::size_t idx = n;
        std::uint64_t best = kNever;
        for (std::size_t i = 0; i < n; ++i) {
            if (deadlines_[i] < best) {
                best = deadlines_[i];
                idx = i;
            }
        }
        if (idx == n || best > target) {
            earliest_ = best;  // the scan just found it
            stale_ = false;
            break;
        }
        const std::uint64_t late = now_ > best ? now_ - best : 0;
        if (now_ < best)
            now_ = best;
        deadlines_[idx] = kNever;  // the callback may re-arm
        stale_ = true;
        events_[idx].cb(now_, late);
    }
    if (target > now_)
        now_ = target;
}

}  // namespace dream::sched
