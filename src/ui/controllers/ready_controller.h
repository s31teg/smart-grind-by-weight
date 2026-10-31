#pragma once
#include <lvgl.h>
#include "../event_bridge_lvgl.h"
#include "../../system/scale_readiness.h"

class UIManager;

// Handles profile tab navigation, long-press editing, and swipe mode switching

class ReadyUIController {
public:
    explicit ReadyUIController(UIManager* manager);

    void register_events();
    void update();
    void refresh_profiles();
    void handle_tab_change(int tab);
    void handle_profile_long_press();
    void handle_manual_tare();
    void toggle_mode();

    // Called every UI cycle; keeps the scale status and Grind button current.
    void refresh_scale_readiness();
    ScaleReadiness scale_readiness() const { return scale_readiness_; }
    bool profile_grind_allowed() const;

private:
    bool profile_mode_is_weight() const;
    void show_profile_scale_status();

    UIManager* ui_manager_;
    uint32_t last_network_update_ms_ = 0;
    uint32_t last_manual_scale_update_ms_ = 0;
    ScaleReadinessTracker scale_tracker_;
    ScaleReadiness scale_readiness_ = ScaleReadiness::STARTING;
    bool status_shown_ = false;
    ScaleReadiness shown_readiness_ = ScaleReadiness::STARTING;
    bool shown_weight_mode_ = false;
};
