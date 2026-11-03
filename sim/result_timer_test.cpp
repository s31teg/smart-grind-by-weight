// The simulator builds in Release mode, so make sure the checks stay compiled in.
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "system/result_timer.h"

namespace {

constexpr uint32_t kLimitMs = 30U * 60U * 1000U;
constexpr float kRemovedThresholdG = 2.0f;

void counts_until_the_cup_is_lifted() {
    ResultTimer timer;
    assert(!timer.running());
    timer.start(1000);
    assert(timer.update(1500, true, kLimitMs) == ResultTimerEvent::NONE);
    assert(timer.elapsed_ms(43000) == 42000);
    assert(timer.update(43000, false, kLimitMs) == ResultTimerEvent::CUP_REMOVED);
    assert(!timer.running());
    // The stopped time stays on screen.
    assert(timer.elapsed_ms(90000) == 42000);
    assert(timer.update(91000, true, kLimitMs) == ResultTimerEvent::NONE);
}

void stops_at_the_limit() {
    ResultTimer timer;
    timer.start(0);
    assert(timer.update(kLimitMs - 1, true, kLimitMs) == ResultTimerEvent::NONE);
    assert(timer.update(kLimitMs, true, kLimitMs) == ResultTimerEvent::LIMIT_REACHED);
    assert(!timer.running());
    assert(timer.elapsed_ms(kLimitMs + 5000) == kLimitMs);
}

void restart_after_a_pulse() {
    ResultTimer timer;
    timer.start(0);
    assert(timer.update(20000, true, kLimitMs) == ResultTimerEvent::NONE);
    timer.start(20000);
    assert(timer.elapsed_ms(21000) == 1000);
}

void survives_millis_wraparound() {
    ResultTimer timer;
    timer.start(UINT32_MAX - 500);
    assert(timer.update(1500, true, kLimitMs) == ResultTimerEvent::NONE);
    assert(timer.elapsed_ms(1500) == 2001);
}

void cup_detection() {
    assert(cup_on_scale(18.2f, kRemovedThresholdG));     // weight mode: the dose
    assert(cup_on_scale(428.0f, kRemovedThresholdG));    // time mode: the whole cup
    assert(!cup_on_scale(-410.0f, kRemovedThresholdG));  // weight mode after lifting
    assert(!cup_on_scale(0.3f, kRemovedThresholdG));     // time mode after lifting
    assert(!cup_on_scale(NAN, kRemovedThresholdG));
}

void formatting() {
    char text[16];
    format_result_timer(text, sizeof(text), 0);
    assert(std::strcmp(text, "0:00") == 0);
    format_result_timer(text, sizeof(text), 7999);
    assert(std::strcmp(text, "0:07") == 0);
    format_result_timer(text, sizeof(text), 754000);
    assert(std::strcmp(text, "12:34") == 0);
    format_result_timer(text, sizeof(text), kLimitMs);
    assert(std::strcmp(text, "30:00") == 0);
}

}  // namespace

int main() {
    counts_until_the_cup_is_lifted();
    stops_at_the_limit();
    restart_after_a_pulse();
    survives_millis_wraparound();
    cup_detection();
    formatting();
    return 0;
}
