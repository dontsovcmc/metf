#include "blinker.h"

namespace {

// Ритмы: сколько горит, сколько нет
constexpr uint32_t kSlowOnMs = 1000;
constexpr uint32_t kSlowOffMs = 1000;
constexpr uint32_t kFastOnMs = 250;
constexpr uint32_t kFastOffMs = 250;
constexpr uint32_t kHeartOnMs = 2900;
constexpr uint32_t kHeartOffMs = 100;

bool in_on_phase(uint32_t elapsed, uint32_t on_ms, uint32_t off_ms) {
    return elapsed % (on_ms + off_ms) < on_ms;
}

} // namespace

void Blinker::set(Rgb color, Pattern pattern) {
    if (color == color_ && pattern == pattern_) return;
    color_ = color;
    pattern_ = pattern;
    phase_valid_ = false;
}

void Blinker::hold(Rgb color) {
    held_color_.store(pack(color));
    held_.store(true);
}

void Blinker::release() { held_.store(false); }

bool Blinker::lit(uint32_t now_ms) const {
    const uint32_t elapsed = now_ms - phase_start_;
    switch (pattern_) {
    case Pattern::Off:
        return false;
    case Pattern::Solid:
        return true;
    case Pattern::Slow:
        return in_on_phase(elapsed, kSlowOnMs, kSlowOffMs);
    case Pattern::Fast:
        return in_on_phase(elapsed, kFastOnMs, kFastOffMs);
    case Pattern::Heartbeat:
        return in_on_phase(elapsed, kHeartOnMs, kHeartOffMs);
    }
    return false;
}

void Blinker::loop(uint32_t now_ms) {
    if (!phase_valid_) {
        phase_start_ = now_ms;
        phase_valid_ = true;
    }

    Rgb want;
    if (held_.load()) {
        want = unpack(held_color_.load());
    } else if (lit(now_ms)) {
        want = color_;
    }

    const bool again = refresh_.exchange(false);
    if (shown_valid_ && want == shown_ && !again) return;
    driver_.show(want);
    shown_ = want;
    shown_valid_ = true;
}
