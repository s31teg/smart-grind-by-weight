#pragma once

#include <cstdint>

// Detects a cup or portafilter lifted off the scale during a grind.
//
// The vessel weight is measured just before the grind starts, relative to an
// empty scale. Removal is declared once the reading falls to the starting
// baseline minus most of that vessel weight and stays there for several
// samples. Weight grinds tare first, so their baseline is zero; time and
// manual grinds do not, so their baseline is the starting weight itself.
//
// The ratio stays high on purpose: a portafilter pushed up in its holder can
// read several hundred grams negative while still in place (issue #24).
class NetWeightRemovalGuard {
public:
    static constexpr float MINIMUM_REFERENCE_WEIGHT_G = 5.0f;
    static constexpr float REMOVAL_THRESHOLD_RATIO = 0.90f;
    static constexpr uint8_t REQUIRED_CONSECUTIVE_SAMPLES = 3;

    void reset(float vessel_weight_g, float baseline_g = 0.0f) {
        reference_weight_g_ = vessel_weight_g >= MINIMUM_REFERENCE_WEIGHT_G
                                  ? vessel_weight_g
                                  : 0.0f;
        baseline_g_ = baseline_g;
        consecutive_samples_ = 0;
    }

    bool update(float weight_g) {
        if (!has_reference() || weight_g > removal_threshold_g()) {
            consecutive_samples_ = 0;
            return false;
        }

        if (consecutive_samples_ < REQUIRED_CONSECUTIVE_SAMPLES) {
            ++consecutive_samples_;
        }
        return consecutive_samples_ >= REQUIRED_CONSECUTIVE_SAMPLES;
    }

    bool has_reference() const { return reference_weight_g_ > 0.0f; }
    void cancel_pending() { consecutive_samples_ = 0; }
    float reference_weight_g() const { return reference_weight_g_; }
    float removal_threshold_g() const {
        return baseline_g_ - reference_weight_g_ * REMOVAL_THRESHOLD_RATIO;
    }

private:
    float reference_weight_g_ = 0.0f;
    float baseline_g_ = 0.0f;
    uint8_t consecutive_samples_ = 0;
};
