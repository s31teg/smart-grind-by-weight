#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

// Counts up on the finished screen from the moment a grind completes until
// the cup or portafilter is lifted, so you can see how long the grounds have
// been waiting. The finished screen stays up while it runs, up to a limit.
enum class ResultTimerEvent : uint8_t {
    NONE,
    CUP_REMOVED,    // Stopped: the cup was lifted
    LIMIT_REACHED,  // Stopped: the cup was left on for the whole limit
};

class ResultTimer {
public:
    void start(uint32_t now_ms) {
        started_ms_ = now_ms;
        stopped_elapsed_ms_ = 0;
        running_ = true;
    }

    void stop(uint32_t now_ms) {
        if (!running_) return;
        stopped_elapsed_ms_ = now_ms - started_ms_;
        running_ = false;
    }

    bool running() const { return running_; }

    uint32_t elapsed_ms(uint32_t now_ms) const {
        return running_ ? now_ms - started_ms_ : stopped_elapsed_ms_;
    }

    ResultTimerEvent update(uint32_t now_ms, bool cup_on_scale, uint32_t limit_ms) {
        if (!running_) return ResultTimerEvent::NONE;
        if (!cup_on_scale) {
            stop(now_ms);
            return ResultTimerEvent::CUP_REMOVED;
        }
        if (now_ms - started_ms_ >= limit_ms) {
            running_ = false;
            stopped_elapsed_ms_ = limit_ms;
            return ResultTimerEvent::LIMIT_REACHED;
        }
        return ResultTimerEvent::NONE;
    }

private:
    uint32_t started_ms_ = 0;
    uint32_t stopped_elapsed_ms_ = 0;
    bool running_ = false;
};

// A finished cup reads at least its dose (weight mode, tared with the cup on)
// or the whole cup (time mode, not tared). Lifting it reads about zero or
// below.
inline bool cup_on_scale(float weight_g, float removed_threshold_g) {
    return std::isfinite(weight_g) && weight_g > removed_threshold_g;
}

// "m:ss", for example "0:07" or "12:34".
inline void format_result_timer(char* buffer, size_t size, uint32_t elapsed_ms) {
    const uint32_t seconds = elapsed_ms / 1000U;
    std::snprintf(buffer, size, "%lu:%02lu",
                  static_cast<unsigned long>(seconds / 60U),
                  static_cast<unsigned long>(seconds % 60U));
}
