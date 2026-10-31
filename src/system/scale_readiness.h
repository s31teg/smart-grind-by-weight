#pragma once

#include <cstdint>

// What the Ready screen tells the user about the load cell.
//
// After power-on the load cell needs several seconds to start and take its
// first tare. Until then a grind would only wait or be refused, so the Ready
// screen shows that the scale is starting and holds back the Grind button.
enum class ScaleReadiness : uint8_t {
    STARTING,     // Has not finished its first tare since power-on
    TARING,       // Was ready; a new zero reference is being collected
    READY,
    UNAVAILABLE,  // Hardware fault, readings stopped, or startup never finished
};

struct ScaleSignals {
    bool hardware_fault;
    bool tare_valid;
    bool tare_in_progress;
    bool recent_sample;
};

class ScaleReadinessTracker {
public:
    ScaleReadiness update(const ScaleSignals& signals, uint32_t uptime_ms,
                          uint32_t startup_timeout_ms) {
        if (signals.hardware_fault) {
            return ScaleReadiness::UNAVAILABLE;
        }
        if (signals.tare_valid && signals.recent_sample && !signals.tare_in_progress) {
            has_been_ready_ = true;
            return ScaleReadiness::READY;
        }
        if (!has_been_ready_) {
            return uptime_ms < startup_timeout_ms ? ScaleReadiness::STARTING
                                                  : ScaleReadiness::UNAVAILABLE;
        }
        return signals.tare_in_progress ? ScaleReadiness::TARING
                                        : ScaleReadiness::UNAVAILABLE;
    }

    bool has_been_ready() const { return has_been_ready_; }

private:
    bool has_been_ready_ = false;
};

// Every profile grind tares first, so none can start while the scale is
// starting or taring. A time grind needs no scale readings, so it stays
// available when the scale has failed; a weight grind does not.
constexpr bool scale_allows_profile_grind(ScaleReadiness readiness, bool weight_mode) {
    return readiness == ScaleReadiness::READY ||
           (readiness == ScaleReadiness::UNAVAILABLE && !weight_mode);
}
