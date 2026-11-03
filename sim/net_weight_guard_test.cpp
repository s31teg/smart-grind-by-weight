// The simulator builds in Release mode, so make sure the checks stay compiled in.
#undef NDEBUG
#include "controllers/net_weight_guard.h"

#include <cassert>

int main() {
    NetWeightRemovalGuard guard;

    // The reported -315.6 g spike must not look like removal of a 427 g
    // portafilter. Its removal threshold is -384.3 g.
    guard.reset(427.0f);
    assert(!guard.update(-315.6f));

    // One threshold-crossing sample is not enough to stop a grind.
    assert(!guard.update(-390.0f));
    assert(!guard.update(2.0f));

    // Leaving an active grind phase also clears a partly confirmed removal.
    assert(!guard.update(-390.0f));
    guard.cancel_pending();
    assert(!guard.update(-391.0f));

    // A sustained reading close to the full pre-tare weight is a removal.
    guard.reset(427.0f);
    assert(!guard.update(-390.0f));
    assert(!guard.update(-391.0f));
    assert(guard.update(-392.0f));

    // With no meaningful vessel weight, the removal guard stays disabled.
    guard.reset(0.0f);
    assert(!guard.has_reference());
    assert(!guard.update(-100.0f));

    // Time and manual grinds do not tare: the reading starts at the vessel
    // weight and grounds add to it. Lifting the portafilter drops it to about
    // zero; a reading that stays near the starting weight is not a removal.
    guard.reset(410.0f, 410.0f);
    assert(guard.removal_threshold_g() > 40.0f && guard.removal_threshold_g() < 42.0f);
    assert(!guard.update(410.0f));
    assert(!guard.update(425.0f));
    assert(!guard.update(120.0f));  // pushed up hard in the holder, still not removed
    assert(!guard.update(0.5f));
    assert(!guard.update(0.2f));
    assert(guard.update(0.1f));

    // A manual grind after taring with the cup on has no vessel reference.
    guard.reset(0.3f, 0.3f);
    assert(!guard.has_reference());
    assert(!guard.update(-410.0f));

    return 0;
}
