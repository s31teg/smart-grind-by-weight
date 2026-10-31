// The simulator builds in Release mode, so make sure the checks stay compiled in.
#undef NDEBUG
#include <cassert>
#include <cstdint>

#include "system/scale_readiness.h"

namespace {

constexpr uint32_t kTimeoutMs = 20000;

constexpr ScaleSignals kNoReadings{false, false, false, false};
constexpr ScaleSignals kBootTare{false, false, true, true};
constexpr ScaleSignals kReady{false, true, false, true};
constexpr ScaleSignals kLaterTare{false, false, true, true};
constexpr ScaleSignals kReadingsStopped{false, true, false, false};
constexpr ScaleSignals kFault{true, false, false, false};

void boot_sequence_reaches_ready() {
    ScaleReadinessTracker tracker;
    assert(tracker.update(kNoReadings, 500, kTimeoutMs) == ScaleReadiness::STARTING);
    assert(tracker.update(kBootTare, 5000, kTimeoutMs) == ScaleReadiness::STARTING);
    assert(!tracker.has_been_ready());
    assert(tracker.update(kReady, 7000, kTimeoutMs) == ScaleReadiness::READY);
    assert(tracker.has_been_ready());
}

void later_tare_is_not_startup() {
    ScaleReadinessTracker tracker;
    assert(tracker.update(kReady, 7000, kTimeoutMs) == ScaleReadiness::READY);
    assert(tracker.update(kLaterTare, 60000, kTimeoutMs) == ScaleReadiness::TARING);
    assert(tracker.update(kReady, 62000, kTimeoutMs) == ScaleReadiness::READY);
}

void startup_that_never_finishes_is_unavailable() {
    ScaleReadinessTracker tracker;
    assert(tracker.update(kBootTare, kTimeoutMs - 1, kTimeoutMs) == ScaleReadiness::STARTING);
    assert(tracker.update(kBootTare, kTimeoutMs, kTimeoutMs) == ScaleReadiness::UNAVAILABLE);
    // A slow tare that does finish still makes the scale ready.
    assert(tracker.update(kReady, kTimeoutMs + 3000, kTimeoutMs) == ScaleReadiness::READY);
}

void fault_and_lost_readings_are_unavailable() {
    ScaleReadinessTracker booting;
    assert(booting.update(kFault, 3000, kTimeoutMs) == ScaleReadiness::UNAVAILABLE);

    ScaleReadinessTracker running;
    assert(running.update(kReady, 7000, kTimeoutMs) == ScaleReadiness::READY);
    assert(running.update(kReadingsStopped, 90000, kTimeoutMs) == ScaleReadiness::UNAVAILABLE);
    assert(running.update(kFault, 91000, kTimeoutMs) == ScaleReadiness::UNAVAILABLE);
}

void grind_permission() {
    assert(scale_allows_profile_grind(ScaleReadiness::READY, true));
    assert(scale_allows_profile_grind(ScaleReadiness::READY, false));
    assert(!scale_allows_profile_grind(ScaleReadiness::STARTING, true));
    assert(!scale_allows_profile_grind(ScaleReadiness::STARTING, false));
    assert(!scale_allows_profile_grind(ScaleReadiness::TARING, true));
    assert(!scale_allows_profile_grind(ScaleReadiness::TARING, false));
    assert(!scale_allows_profile_grind(ScaleReadiness::UNAVAILABLE, true));
    assert(scale_allows_profile_grind(ScaleReadiness::UNAVAILABLE, false));
}

}  // namespace

int main() {
    boot_sequence_reaches_ready();
    later_tare_is_not_startup();
    startup_that_never_finishes_is_unavailable();
    fault_and_lost_readings_are_unavailable();
    grind_permission();
    return 0;
}
