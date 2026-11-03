#include <lvgl.h>

#include "config/constants.h"
#include "screenshot.h"
#include "sim_platform.h"
#include "ui/screens/grinding_screen_arc.h"
#include "ui/screens/grinding_screen_chart.h"
#include "ui/screens/grinding_screen_circle.h"
#include "ui/screens/ready_screen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

using sim_platform::now_ms;
using sim_platform::set_status;

constexpr int kDisplayWidth = 280;
constexpr int kDisplayHeight = 456;
constexpr float kTargetWeight = USER_SINGLE_ESPRESSO_WEIGHT_G;
constexpr uint64_t kArcPixelBudget = 800000;
constexpr uint64_t kChartPixelBudget = 3500000;
// A tab swipe should only redraw the 280 x 364 tab area (101,920 px) per frame.
// Budgeting per frame keeps the check independent of how many frames the host
// manages to render during the animation.
constexpr uint64_t kSwipePixelsPerRefreshBudget = 110000;
constexpr uint32_t kSwipeDurationBudgetMs = 300;

// A full grind on the circle layout takes about 9 seconds in the mock model.
constexpr uint32_t kCircleSmokeTimeoutMs = 15000;
// Lets the completion screen render before checking.
constexpr uint32_t kCircleSettleMs = 400;

ReadyScreen ready_screen;
GrindingScreenArc arc_screen;
GrindingScreenChart chart_screen;
GrindingScreenCircle circle_screen;
// Indexed by GrindScreenLayout, matching the firmware's layout order.
IGrindingScreen* const grind_screens[] = {&arc_screen, &chart_screen, &circle_screen};
lv_obj_t* grind_button = nullptr;
lv_obj_t* grind_icon = nullptr;
lv_obj_t* pulse_button = nullptr;
GrindScreenLayout layout = GrindScreenLayout::MINIMAL_ARC;
bool grinding = false;
bool settling = false;
bool manual_grinding = false;
bool grind_completed = false;
float weight_g = 0.0f;
float flow_gps = 0.0f;
uint32_t grind_started_ms = 0;
uint32_t last_update_ms = 0;

struct RenderMetrics {
    uint64_t flushed_pixels = 0;
    uint64_t render_time_ms = 0;
    uint32_t flush_count = 0;
    uint32_t refresh_count = 0;
    uint32_t render_started_ms = 0;
};

RenderMetrics render_metrics;

void reset_render_metrics() {
    render_metrics = {};
}

void print_render_metrics(const char* layout, uint32_t duration_ms) {
    std::printf(
        "[benchmark] %s duration=%lums refreshes=%lu flushes=%lu pixels=%llu render=%llums\n",
        layout,
        static_cast<unsigned long>(duration_ms),
        static_cast<unsigned long>(render_metrics.refresh_count),
        static_cast<unsigned long>(render_metrics.flush_count),
        static_cast<unsigned long long>(render_metrics.flushed_pixels),
        static_cast<unsigned long long>(render_metrics.render_time_ms));
    std::fflush(stdout);
}

void display_metrics_event(lv_event_t* event) {
    switch (lv_event_get_code(event)) {
        case LV_EVENT_RENDER_START:
            render_metrics.render_started_ms = now_ms();
            break;
        case LV_EVENT_RENDER_READY:
            if (render_metrics.render_started_ms != 0) {
                render_metrics.render_time_ms += now_ms() - render_metrics.render_started_ms;
                render_metrics.render_started_ms = 0;
            }
            break;
        case LV_EVENT_FLUSH_START: {
            const auto* area = static_cast<const lv_area_t*>(lv_event_get_param(event));
            if (area) {
                render_metrics.flushed_pixels += static_cast<uint64_t>(lv_area_get_size(area));
                render_metrics.flush_count++;
            }
            break;
        }
        case LV_EVENT_REFR_READY:
            render_metrics.refresh_count++;
            break;
        default:
            break;
    }
}

void set_grind_button(const char* symbol, uint32_t color) {
    if (grind_icon) {
        lv_image_set_src(grind_icon, symbol);
    }
    if (grind_button) {
        lv_obj_set_style_bg_color(grind_button, lv_color_hex(color), 0);
    }
}

// As in the firmware: on the circle layout the finished screen is the same
// green as the OK button, so the button gets a white outline.
void update_grind_button_outline() {
    const bool outline = grind_completed && layout == GrindScreenLayout::CIRCLE;
    lv_obj_set_style_border_color(grind_button, lv_color_hex(THEME_COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_border_width(grind_button, outline ? 4 : 0, 0);
}

void show_ready() {
    grinding = false;
    settling = false;
    manual_grinding = false;
    grind_completed = false;
    for (IGrindingScreen* screen : grind_screens) {
        screen->hide();
    }
    ready_screen.show();
    set_grind_button(LV_SYMBOL_PLAY, THEME_COLOR_PRIMARY);
    update_grind_button_outline();
    set_status("READY");
}

void show_active_grind_screen() {
    ready_screen.hide();
    IGrindingScreen* active = grind_screens[static_cast<int>(layout)];
    for (IGrindingScreen* screen : grind_screens) {
        if (screen != active) screen->hide();
    }
    active->show();
}

void reset_grind_views() {
    const bool manual = lv_tabview_get_tab_act(ready_screen.get_tabview()) ==
                        ReadyScreen::MANUAL_TAB_INDEX;
    circle_screen.set_outcome(GrindScreenOutcome::IN_PROGRESS);
    for (IGrindingScreen* screen : grind_screens) {
        screen->update_profile_name(manual ? "MANUAL" : "SINGLE");
        if (manual) {
            screen->update_target_weight_text("Elapsed: 0.0s");
        } else {
            screen->update_target_weight(kTargetWeight);
        }
        screen->update_current_weight(0.0f);
        screen->update_progress(0);
    }
    chart_screen.reset_chart_data();
}

void start_grind() {
    weight_g = 0.0f;
    flow_gps = 0.0f;
    grinding = true;
    settling = false;
    grind_completed = false;
    manual_grinding = lv_tabview_get_tab_act(ready_screen.get_tabview()) ==
                      ReadyScreen::MANUAL_TAB_INDEX;
    grind_started_ms = now_ms();
    last_update_ms = grind_started_ms;
    reset_grind_views();
    show_active_grind_screen();
    set_grind_button(LV_SYMBOL_STOP, THEME_COLOR_PRIMARY);
    update_grind_button_outline();
    set_status("GRINDING");
}

void toggle_layout() {
    layout = static_cast<GrindScreenLayout>(
        (static_cast<int>(layout) + 1) % static_cast<int>(GrindScreenLayout::COUNT));
    if (!ready_screen.is_visible()) {
        show_active_grind_screen();
    }
    update_grind_button_outline();
    const char* const names[] = {"ARC VIEW", "CHART VIEW", "CIRCLE VIEW"};
    set_status(names[static_cast<int>(layout)]);
}

void tare() {
    weight_g = 0.0f;
    for (IGrindingScreen* screen : grind_screens) {
        screen->update_tare_display();
    }
    set_status("TARE");
}

void grind_button_event(lv_event_t*) {
    if (ready_screen.is_visible()) {
        start_grind();
    } else if (grinding || settling) {
        grinding = false;
        settling = false;
        manual_grinding = false;
        set_grind_button(LV_SYMBOL_OK, THEME_COLOR_SUCCESS);
        set_status("STOPPED");
    } else {
        show_ready();
    }
}

void create_grinder_controls() {
    grind_button = lv_button_create(lv_screen_active());
    lv_obj_set_size(grind_button, 100, 100);
    lv_obj_align(grind_button, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_radius(grind_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(grind_button, lv_color_hex(THEME_COLOR_PRIMARY), 0);
    lv_obj_set_style_border_width(grind_button, 0, 0);
    lv_obj_set_style_shadow_width(grind_button, 0, 0);
    lv_obj_add_event_cb(grind_button, grind_button_event, LV_EVENT_CLICKED, nullptr);

    grind_icon = lv_image_create(grind_button);
    lv_image_set_src(grind_icon, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(grind_icon, &lv_font_montserrat_24, 0);
    lv_obj_center(grind_icon);

    pulse_button = lv_button_create(lv_screen_active());
    lv_obj_set_size(pulse_button, 100, 100);
    lv_obj_align(pulse_button, LV_ALIGN_BOTTOM_MID, 60, -10);
    lv_obj_set_style_radius(pulse_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pulse_button, lv_color_hex(THEME_COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(pulse_button, 0, 0);
    lv_obj_set_style_shadow_width(pulse_button, 0, 0);
    lv_obj_add_flag(pulse_button, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* pulse_icon = lv_image_create(pulse_button);
    lv_image_set_src(pulse_icon, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_font(pulse_icon, &lv_font_montserrat_32, 0);
    lv_obj_center(pulse_icon);
}

void update_mock_grind() {
    if (!grinding && !settling) {
        return;
    }

    const uint32_t current_ms = now_ms();
    const float dt_s = std::min(0.1f, static_cast<float>(current_ms - last_update_ms) / 1000.0f);
    last_update_ms = current_ms;
    const float elapsed_s = static_cast<float>(current_ms - grind_started_ms) / 1000.0f;

    if (grinding) {
        flow_gps = std::min(1.75f, 0.25f + elapsed_s * 0.55f);
        weight_g += flow_gps * dt_s;
        if (manual_grinding) {
            char elapsed_text[32];
            std::snprintf(elapsed_text, sizeof(elapsed_text), "Elapsed: %.1fs", elapsed_s);
            for (IGrindingScreen* screen : grind_screens) {
                screen->update_target_weight_text(elapsed_text);
            }
        } else if (weight_g >= kTargetWeight - 0.45f) {
            grinding = false;
            settling = true;
            set_status("SETTLING");
        }
    } else if (settling) {
        flow_gps *= std::pow(0.12f, dt_s);
        weight_g += flow_gps * dt_s;
        if (flow_gps < 0.015f) {
            settling = false;
            flow_gps = 0.0f;
            grind_completed = true;
            circle_screen.set_outcome(GrindScreenOutcome::COMPLETE);
            set_grind_button(LV_SYMBOL_OK, THEME_COLOR_SUCCESS);
            update_grind_button_outline();
            set_status("COMPLETE");
        }
    }

    const int progress = manual_grinding
                             ? 0
                             : std::clamp(static_cast<int>((weight_g / kTargetWeight) * 100.0f), 0, 100);
    for (IGrindingScreen* screen : grind_screens) {
        screen->update_current_weight(weight_g);
        screen->update_progress(progress);
    }
    chart_screen.add_chart_data_point(weight_g, flow_gps, current_ms);
}

bool has_arg(int argc, char** argv, const char* expected) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], expected) == 0) {
            return true;
        }
    }
    return false;
}

// The value after a flag, e.g. the folder in "--screenshots media/screens".
const char* arg_value(int argc, char** argv, const char* expected) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], expected) == 0) {
            return argv[i + 1];
        }
    }
    return nullptr;
}

// The "SCALE STARTING" line must sit between the profile target and the Grind
// button, which is drawn over the bottom of the Ready screen area.
bool check_scale_status_layout(lv_display_t* display) {
    lv_tabview_set_act(ready_screen.get_tabview(), ReadyScreen::PROFILE_TAB_START_INDEX, LV_ANIM_OFF);
    ready_screen.set_profile_scale_status("SCALE STARTING", THEME_COLOR_TEXT_SECONDARY, true);
    lv_refr_now(display);

    lv_obj_t* row = ready_screen.get_profile_status_row(0);
    lv_obj_t* weight = ready_screen.get_profile_weight_label(0);
    lv_area_t row_area;
    lv_area_t weight_area;
    lv_area_t button_area;
    lv_obj_get_coords(row, &row_area);
    lv_obj_get_coords(weight, &weight_area);
    lv_obj_get_coords(grind_button, &button_area);
    std::printf("[scale-status] target y=%d..%d status y=%d..%d x=%d..%d button y=%d..%d\n",
                static_cast<int>(weight_area.y1), static_cast<int>(weight_area.y2),
                static_cast<int>(row_area.y1), static_cast<int>(row_area.y2),
                static_cast<int>(row_area.x1), static_cast<int>(row_area.x2),
                static_cast<int>(button_area.y1), static_cast<int>(button_area.y2));

    constexpr int32_t kMinGapPx = 6;
    const bool visible = !lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN) && lv_obj_get_child_count(row) == 2;
    const bool clear_of_target = row_area.y1 >= weight_area.y2 + kMinGapPx;
    const bool clear_of_button = row_area.y2 + kMinGapPx <= button_area.y1;
    const bool on_screen = row_area.x1 >= 0 && row_area.x2 < kDisplayWidth;

    ready_screen.set_profile_scale_status(nullptr, THEME_COLOR_TEXT_SECONDARY, false);
    lv_refr_now(display);
    const bool hides = lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN) && lv_obj_get_child_count(row) == 1;

    if (!visible || !clear_of_target || !clear_of_button || !on_screen || !hides) {
        std::fprintf(stderr, "Scale status layout check failed: visible=%d target=%d button=%d "
                             "on_screen=%d hides=%d\n",
                     visible, clear_of_target, clear_of_button, on_screen, hides);
        return false;
    }
    std::puts("[scale-status] layout ok");
    return true;
}

bool areas_overlap(const lv_area_t& a, const lv_area_t& b) {
    return a.x1 <= b.x2 && b.x1 <= a.x2 && a.y1 <= b.y2 && b.y1 <= a.y2;
}

// On every layout's finished screen the result timer must clear the text
// beside it and the OK button, and stay inside the arc on the arc layout.
bool check_result_timer_layout(lv_display_t* display) {
    const char* const names[] = {"arc", "chart", "circle"};
    lv_area_t button_area;
    bool all_ok = true;

    ready_screen.hide();
    for (int i = 0; i < 3; ++i) {
        for (IGrindingScreen* screen : grind_screens) {
            screen->hide();
        }
        IGrindingScreen* screen = grind_screens[i];
        screen->update_profile_name("DOUBLE");
        screen->update_target_weight(18.0f);
        screen->update_current_weight(18.1f);
        screen->update_progress(100);
        screen->set_outcome(GrindScreenOutcome::COMPLETE);
        screen->update_result_timer("12:34");
        screen->show();
        lv_refr_now(display);
        lv_obj_get_coords(grind_button, &button_area);

        lv_obj_t* timer = screen->get_result_timer_label();
        lv_area_t timer_area;
        lv_obj_get_coords(timer, &timer_area);
        bool ok = !lv_obj_has_flag(timer, LV_OBJ_FLAG_HIDDEN) &&
                  timer_area.x1 >= 0 && timer_area.x2 < kDisplayWidth &&
                  timer_area.y2 + 4 <= button_area.y1;

        lv_obj_t* parent = lv_obj_get_parent(timer);
        for (uint32_t c = 0; c < lv_obj_get_child_count(parent); ++c) {
            lv_obj_t* sibling = lv_obj_get_child(parent, static_cast<int32_t>(c));
            if (sibling == timer || lv_obj_has_flag(sibling, LV_OBJ_FLAG_HIDDEN)) continue;
            if (!lv_obj_check_type(sibling, &lv_label_class) &&
                !lv_obj_check_type(sibling, &lv_spangroup_class)) continue;
            lv_area_t sibling_area;
            lv_obj_get_coords(sibling, &sibling_area);
            if (areas_overlap(timer_area, sibling_area)) ok = false;
        }

        if (lv_obj_check_type(parent, &lv_arc_class)) {
            lv_area_t arc_area;
            lv_obj_get_coords(parent, &arc_area);
            const float cx = (arc_area.x1 + arc_area.x2) / 2.0f;
            const float cy = (arc_area.y1 + arc_area.y2) / 2.0f;
            const float inner_radius = lv_area_get_width(&arc_area) / 2.0f - 12.0f;
            const float xs[] = {static_cast<float>(timer_area.x1), static_cast<float>(timer_area.x2)};
            const float ys[] = {static_cast<float>(timer_area.y1), static_cast<float>(timer_area.y2)};
            for (float x : xs) {
                for (float y : ys) {
                    if (std::hypot(x - cx, y - cy) > inner_radius) ok = false;
                }
            }
        }

        std::printf("[result-timer] %s timer y=%d..%d x=%d..%d button y=%d %s\n", names[i],
                    static_cast<int>(timer_area.y1), static_cast<int>(timer_area.y2),
                    static_cast<int>(timer_area.x1), static_cast<int>(timer_area.x2),
                    static_cast<int>(button_area.y1), ok ? "ok" : "FAIL");
        all_ok = all_ok && ok;

        screen->update_result_timer(nullptr);
        if (!lv_obj_has_flag(timer, LV_OBJ_FLAG_HIDDEN)) all_ok = false;

        // The removal stop's error screen must fit without scrolling and keep
        // its text clear of the button.
        screen->update_profile_name("STOPPED");
        screen->update_target_weight_text("Portafilter removed");
        screen->set_outcome(GrindScreenOutcome::FAILED);
        lv_refr_now(display);
        lv_obj_t* root = screen->get_screen();
        // The circle layout overhangs the display on purpose and cannot scroll.
        bool error_ok = !lv_obj_has_flag(root, LV_OBJ_FLAG_SCROLLABLE) ||
                        (lv_obj_get_scroll_top(root) <= 0 && lv_obj_get_scroll_bottom(root) <= 0);
        int32_t lowest_text_y = 0;
        for (uint32_t c = 0; c < lv_obj_get_child_count(root); ++c) {
            lv_obj_t* child = lv_obj_get_child(root, static_cast<int32_t>(c));
            if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) continue;
            if (!lv_obj_check_type(child, &lv_label_class) &&
                !lv_obj_check_type(child, &lv_spangroup_class)) continue;
            lv_area_t area;
            lv_obj_get_coords(child, &area);
            lowest_text_y = std::max(lowest_text_y, static_cast<int32_t>(area.y2));
            if (area.y1 < 0 || area.y2 + 4 > button_area.y1) error_ok = false;
        }
        std::printf("[result-timer] %s error screen lowest text y=%d scroll=%d/%d %s\n", names[i],
                    static_cast<int>(lowest_text_y),
                    static_cast<int>(lv_obj_get_scroll_top(root)),
                    static_cast<int>(lv_obj_get_scroll_bottom(root)), error_ok ? "ok" : "FAIL");
        all_ok = all_ok && error_ok;
        screen->set_outcome(GrindScreenOutcome::IN_PROGRESS);
    }
    if (!all_ok) std::fprintf(stderr, "Result timer layout check failed.\n");
    return all_ok;
}

// The firmware's status icons, placed exactly as StatusIndicatorController
// places them, with the widest glyphs it shows.
lv_obj_t* status_icons[4] = {};

void create_status_icons() {
    const char* symbols[4] = {LV_SYMBOL_BLUETOOTH, LV_SYMBOL_WIFI, LV_SYMBOL_WARNING, LV_SYMBOL_REFRESH};
    for (int i = 0; i < 4; ++i) {
        status_icons[i] = lv_label_create(lv_screen_active());
        lv_label_set_text(status_icons[i], symbols[i]);
        lv_obj_set_style_text_font(status_icons[i], &lv_font_montserrat_24, 0);
        lv_obj_align(status_icons[i], LV_ALIGN_TOP_RIGHT,
                     -(THEME_STATUS_ICON_RIGHT_PX + i * THEME_STATUS_ICON_SPACING_PX),
                     THEME_STATUS_ICON_TOP_PX);
    }
}

bool is_status_icon(lv_obj_t* obj) {
    for (lv_obj_t* icon : status_icons) {
        if (obj == icon) return true;
    }
    return false;
}

// Reports every visible label, span group, button, arc or chart that sits
// under a status icon, or (for content other than the button) under the
// Grind button.
int report_icon_overlaps(lv_obj_t* parent, const char* scenario) {
    int overlaps = 0;
    lv_area_t button_area;
    lv_obj_get_coords(grind_button, &button_area);
    for (uint32_t c = 0; c < lv_obj_get_child_count(parent); ++c) {
        lv_obj_t* child = lv_obj_get_child(parent, static_cast<int32_t>(c));
        if (is_status_icon(child) || child == grind_button || child == pulse_button ||
            lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        const bool text = lv_obj_check_type(child, &lv_label_class) ||
                          lv_obj_check_type(child, &lv_spangroup_class);
        const bool control = lv_obj_check_type(child, &lv_button_class);
        const bool content = lv_obj_check_type(child, &lv_arc_class) ||
                             lv_obj_check_type(child, &lv_chart_class);
        if (text || control || content) {
            lv_area_t area;
            lv_obj_get_coords(child, &area);
            const bool on_display = area.x2 >= 0 && area.x1 < kDisplayWidth;
            for (lv_obj_t* icon : status_icons) {
                lv_area_t icon_area;
                lv_obj_get_coords(icon, &icon_area);
                if (on_display && areas_overlap(area, icon_area)) {
                    std::printf("[status-icons] %s: icon over y=%d..%d x=%d..%d\n", scenario,
                                static_cast<int>(area.y1), static_cast<int>(area.y2),
                                static_cast<int>(area.x1), static_cast<int>(area.x2));
                    ++overlaps;
                    break;
                }
            }
            if (on_display && !control && areas_overlap(area, button_area) &&
                !lv_obj_has_flag(grind_button, LV_OBJ_FLAG_HIDDEN)) {
                std::printf("[status-icons] %s: Grind button over y=%d..%d\n", scenario,
                            static_cast<int>(area.y1), static_cast<int>(area.y2));
                ++overlaps;
            }
        }
        overlaps += report_icon_overlaps(child, scenario);
    }
    return overlaps;
}

int check_icons(lv_display_t* display, const char* scenario) {
    lv_refr_now(display);
    const int overlaps = report_icon_overlaps(lv_screen_active(), scenario);
    if (overlaps == 0) std::printf("[status-icons] %s: clear\n", scenario);
    return overlaps;
}

// Ready and grinding screens show the status icons, so nothing on them may
// sit under the icons.
bool check_status_icon_layout(lv_display_t* display) {
    create_status_icons();
    int overlaps = 0;

    for (IGrindingScreen* screen : grind_screens) screen->hide();
    ready_screen.show();
    const char* tab_names[ReadyScreen::TAB_COUNT] = {"ready manual", "ready single", "ready double",
                                                     "ready custom", "ready wi-fi", "ready menu"};
    for (int tab = 0; tab < ReadyScreen::TAB_COUNT; ++tab) {
        lv_tabview_set_act(ready_screen.get_tabview(), static_cast<uint32_t>(tab), LV_ANIM_OFF);
        overlaps += check_icons(display, tab_names[tab]);
    }
    ready_screen.hide();

    const char* layout_names[] = {"arc", "chart", "circle"};
    const char* profile_names[] = {"SINGLE", "DOUBLE", "CUSTOM", "MANUAL", "STOPPED"};
    char scenario[64];
    for (int i = 0; i < 3; ++i) {
        for (IGrindingScreen* screen : grind_screens) screen->hide();
        IGrindingScreen* screen = grind_screens[i];
        screen->show();
        screen->update_target_weight(18.0f);
        for (const char* name : profile_names) {
            screen->set_outcome(GrindScreenOutcome::IN_PROGRESS);
            screen->update_profile_name(name);
            screen->update_current_weight(12.3f);
            screen->update_progress(60);
            std::snprintf(scenario, sizeof(scenario), "%s grinding %s", layout_names[i], name);
            overlaps += check_icons(display, scenario);
        }
        screen->update_profile_name("DOUBLE");
        screen->set_outcome(GrindScreenOutcome::COMPLETE);
        screen->update_result_timer("12:34");
        std::snprintf(scenario, sizeof(scenario), "%s finished", layout_names[i]);
        overlaps += check_icons(display, scenario);
        screen->update_result_timer(nullptr);
        screen->update_profile_name("STOPPED");
        screen->update_target_weight_text("Portafilter removed");
        screen->set_outcome(GrindScreenOutcome::FAILED);
        std::snprintf(scenario, sizeof(scenario), "%s stopped", layout_names[i]);
        overlaps += check_icons(display, scenario);
        screen->set_outcome(GrindScreenOutcome::IN_PROGRESS);
        screen->hide();
    }

    // Time mode adds a "Time:" line to the chart readout and shows elapsed
    // seconds in place of the weight.
    arc_screen.set_time_mode(true);
    chart_screen.set_time_mode(true);
    circle_screen.set_time_mode(true);
    arc_screen.update_target_time(12.0f);
    chart_screen.update_target_time(12.0f);
    circle_screen.update_target_time(12.0f);
    for (int i = 0; i < 3; ++i) {
        IGrindingScreen* screen = grind_screens[i];
        screen->show();
        screen->update_profile_name("CUSTOM");
        screen->update_progress(40);
        std::snprintf(scenario, sizeof(scenario), "%s time grinding", layout_names[i]);
        overlaps += check_icons(display, scenario);
        screen->set_outcome(GrindScreenOutcome::COMPLETE);
        screen->update_result_timer("12:34");
        std::snprintf(scenario, sizeof(scenario), "%s time finished", layout_names[i]);
        overlaps += check_icons(display, scenario);
        screen->update_result_timer(nullptr);
        screen->set_outcome(GrindScreenOutcome::IN_PROGRESS);
        screen->hide();
    }
    arc_screen.set_time_mode(false);
    chart_screen.set_time_mode(false);
    circle_screen.set_time_mode(false);

    if (overlaps != 0) std::fprintf(stderr, "Status icon layout check failed.\n");
    return overlaps == 0;
}

bool save_screen(lv_display_t* display, const char* folder, const char* name) {
    lv_refr_now(display);
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s.bmp", folder, name);
    const bool saved = sim_screenshot::save_bmp(lv_screen_active(), path);
    std::printf("[screenshots] %s %s\n", path, saved ? "saved" : "FAILED");
    return saved;
}

// Feeds the chart the mock grinder's weight and flow curve up to the given
// weight, sampled at the chart's 50 ms interval. A settled grind also gets
// the run-on after the motor stops.
void fill_chart_until(float stop_weight_g, bool settled) {
    constexpr uint32_t kSampleMs = 50;
    constexpr uint32_t kSettleMs = 1500;
    chart_screen.reset_chart_data();
    float weight = 0.0f;
    float flow = 0.0f;
    uint32_t time_ms = 1;
    while (weight < stop_weight_g) {
        const float elapsed_s = time_ms / 1000.0f;
        flow = std::min(1.75f, 0.25f + elapsed_s * 0.55f);
        weight += flow * kSampleMs / 1000.0f;
        chart_screen.add_chart_data_point(weight, flow, time_ms);
        time_ms += kSampleMs;
    }
    if (!settled) return;
    for (uint32_t settle_ms = 0; settle_ms < kSettleMs; settle_ms += kSampleMs) {
        flow *= std::pow(0.12f, kSampleMs / 1000.0f);
        chart_screen.add_chart_data_point(weight, flow, time_ms);
        time_ms += kSampleMs;
    }
}

void show_only(IGrindingScreen* active) {
    ready_screen.hide();
    for (IGrindingScreen* screen : grind_screens) {
        if (screen != active) screen->hide();
    }
    active->show();
}

// Saves the screens shown in the documentation, with Bluetooth on and Wi-Fi
// connected.
bool save_documentation_screenshots(lv_display_t* display, const char* folder) {
    create_status_icons();
    lv_obj_set_style_text_color(status_icons[0], lv_color_hex(THEME_COLOR_ACCENT), 0);
    lv_obj_set_style_text_color(status_icons[1], lv_color_hex(THEME_COLOR_SUCCESS), 0);
    lv_obj_add_flag(status_icons[2], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_icons[3], LV_OBJ_FLAG_HIDDEN);
    bool ok = true;

    const uint32_t double_tab = ReadyScreen::tab_for_profile_index(1);
    lv_tabview_set_act(ready_screen.get_tabview(), double_tab, LV_ANIM_OFF);
    ok &= save_screen(display, folder, "ready");
    ready_screen.set_profile_scale_status("SCALE STARTING", THEME_COLOR_TEXT_SECONDARY, true);
    ok &= save_screen(display, folder, "scale-starting");
    ready_screen.set_profile_scale_status(nullptr, THEME_COLOR_TEXT_SECONDARY, false);

    constexpr float kTarget = 18.0f;
    constexpr float kGrindingWeight = 11.6f;
    constexpr float kFinalWeight = 18.02f;
    const char* const layout_names[] = {"arc", "chart", "circle"};
    char name[64];
    fill_chart_until(kGrindingWeight, false);
    for (int i = 0; i < 3; ++i) {
        IGrindingScreen* screen = grind_screens[i];
        layout = static_cast<GrindScreenLayout>(i);
        show_only(screen);
        screen->update_profile_name("DOUBLE");
        screen->update_target_weight(kTarget);
        screen->set_outcome(GrindScreenOutcome::IN_PROGRESS);
        screen->update_current_weight(kGrindingWeight);
        screen->update_progress(static_cast<int>(kGrindingWeight / kTarget * 100.0f));
        grind_completed = false;
        set_grind_button(LV_SYMBOL_STOP, THEME_COLOR_PRIMARY);
        update_grind_button_outline();
        std::snprintf(name, sizeof(name), "grinding-%s", layout_names[i]);
        ok &= save_screen(display, folder, name);
    }

    fill_chart_until(kFinalWeight - 0.4f, true);
    for (int i = 0; i < 3; ++i) {
        IGrindingScreen* screen = grind_screens[i];
        layout = static_cast<GrindScreenLayout>(i);
        show_only(screen);
        screen->update_current_weight(kFinalWeight);
        screen->update_progress(100);
        screen->set_outcome(GrindScreenOutcome::COMPLETE);
        screen->update_result_timer("0:42");
        grind_completed = true;
        set_grind_button(LV_SYMBOL_OK, THEME_COLOR_SUCCESS);
        update_grind_button_outline();
        std::snprintf(name, sizeof(name), "finished-%s", layout_names[i]);
        ok &= save_screen(display, folder, name);
        screen->update_result_timer(nullptr);
    }

    layout = GrindScreenLayout::MINIMAL_ARC;
    show_only(&arc_screen);
    arc_screen.update_profile_name("STOPPED");
    arc_screen.update_target_weight_text("Portafilter removed");
    arc_screen.update_current_weight(9.4f);
    arc_screen.update_progress(52);
    arc_screen.set_outcome(GrindScreenOutcome::FAILED);
    grind_completed = false;
    set_grind_button(LV_SYMBOL_OK, THEME_COLOR_SUCCESS);
    update_grind_button_outline();
    ok &= save_screen(display, folder, "portafilter-removed");
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    const bool smoke_test = has_arg(argc, argv, "--smoke");
    const bool benchmark = has_arg(argc, argv, "--benchmark");
    const bool swipe_benchmark = has_arg(argc, argv, "--swipe-benchmark");
    const bool manual_smoke = has_arg(argc, argv, "--manual-smoke");
    const bool circle_smoke = has_arg(argc, argv, "--circle-smoke");
    const bool scale_status_check = has_arg(argc, argv, "--scale-status-check");
    const bool result_timer_check = has_arg(argc, argv, "--result-timer-check");
    const bool status_icon_check = has_arg(argc, argv, "--status-icon-check");
    const char* screenshot_folder = arg_value(argc, argv, "--screenshots");

    if (smoke_test) {
        std::puts("[smoke] starting simulator");
        std::fflush(stdout);
    }

    lv_init();
    lv_tick_set_cb(now_ms);

    const bool automated_run =
        smoke_test || benchmark || swipe_benchmark || manual_smoke || circle_smoke ||
        scale_status_check || result_timer_check || status_icon_check || screenshot_folder;
    lv_display_t* display =
        sim_platform::create_display(kDisplayWidth, kDisplayHeight, automated_run);
    if (!display) {
        std::fprintf(stderr, "Could not create the simulator display.\n");
        return 1;
    }
    lv_display_add_event_cb(display, display_metrics_event, LV_EVENT_ALL, nullptr);

    // The Windows driver creates its framebuffer on the first LVGL timer pass.
    // Let that happen before sizing and styling the production screen objects.
    sim_platform::sleep_ms(LV_DEF_REFR_PERIOD + 1);
    lv_timer_handler();

    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(THEME_COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);

    ready_screen.create();
    arc_screen.create();
    chart_screen.create();
    circle_screen.create();
    create_grinder_controls();
    show_ready();
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(display);

    if (smoke_test) {
        std::puts("[smoke] UI created");
        std::fflush(stdout);
    }

    if (scale_status_check) {
        return check_scale_status_layout(display) ? 0 : 8;
    }

    if (result_timer_check) {
        return check_result_timer_layout(display) ? 0 : 9;
    }

    if (status_icon_check) {
        return check_status_icon_layout(display) ? 0 : 10;
    }

    if (screenshot_folder) {
        return save_documentation_screenshots(display, screenshot_folder) ? 0 : 11;
    }

    const uint32_t smoke_started_ms = now_ms();
    bool smoke_scenario_started = false;
    bool benchmark_switched_layout = false;
    uint64_t benchmark_arc_pixels = 0;
    bool swipe_started = false;
    uint32_t swipe_started_ms = 0;
    bool view_key_down = false;
    bool tare_key_down = false;
    bool manual_stop_sent = false;
    int32_t largest_grinding_diameter = 0;
    uint32_t circle_completed_ms = 0;

    while (sim_platform::is_window_open()) {
        if (swipe_benchmark && !swipe_started && now_ms() - smoke_started_ms > 100) {
            reset_render_metrics();
            swipe_started_ms = now_ms();
            lv_tabview_set_act(ready_screen.get_tabview(), 1, LV_ANIM_ON);
            swipe_started = true;
        }

        if ((smoke_test || benchmark) && !smoke_scenario_started && now_ms() - smoke_started_ms > 100) {
            lv_tabview_set_act(ready_screen.get_tabview(), ReadyScreen::PROFILE_TAB_START_INDEX, LV_ANIM_OFF);
            start_grind();
            smoke_scenario_started = true;
            reset_render_metrics();
        }

        if (manual_smoke && !smoke_scenario_started && now_ms() - smoke_started_ms > 100) {
            if (lv_tabview_get_tab_act(ready_screen.get_tabview()) != ReadyScreen::MANUAL_TAB_INDEX) {
                std::fprintf(stderr, "Manual page is not the first ready tab.\n");
                sim_platform::exit_process(5);
            }
            start_grind();
            smoke_scenario_started = true;
        }

        if (circle_smoke && !smoke_scenario_started && now_ms() - smoke_started_ms > 100) {
            lv_tabview_set_act(ready_screen.get_tabview(), ReadyScreen::PROFILE_TAB_START_INDEX, LV_ANIM_OFF);
            layout = GrindScreenLayout::CIRCLE;
            start_grind();
            smoke_scenario_started = true;
        }

        update_mock_grind();

        const bool view_pressed = sim_platform::is_key_down('V');
        if (view_pressed && !view_key_down) {
            toggle_layout();
        }
        view_key_down = view_pressed;

        const bool tare_pressed = sim_platform::is_key_down('T');
        if (tare_pressed && !tare_key_down) {
            tare();
        }
        tare_key_down = tare_pressed;
        const uint32_t wait_ms = std::clamp<uint32_t>(lv_timer_handler(), 1, 16);
        sim_platform::sleep_ms(wait_ms);

        if (swipe_benchmark && swipe_started &&
            !lv_obj_is_scrolling(lv_tabview_get_content(ready_screen.get_tabview()))) {
            const uint32_t duration_ms = now_ms() - swipe_started_ms;
            print_render_metrics("swipe", duration_ms);
            const uint64_t pixels_per_refresh = render_metrics.refresh_count > 0
                ? render_metrics.flushed_pixels / render_metrics.refresh_count
                : 0;
            if (lv_tabview_get_tab_act(ready_screen.get_tabview()) != 1 ||
                duration_ms > kSwipeDurationBudgetMs ||
                pixels_per_refresh > kSwipePixelsPerRefreshBudget) {
                std::fprintf(stderr, "Simulator swipe budget exceeded.\n");
                std::fflush(stderr);
                sim_platform::exit_process(4);
            }
            sim_platform::exit_process(0);
        }

        if (circle_smoke && smoke_scenario_started) {
            // The circle must grow while grinding, then the display must turn
            // solid green once the grind completes.
            lv_obj_t* circle = circle_screen.get_circle();
            const int32_t diameter = lv_obj_get_style_width(circle, LV_PART_MAIN);
            if (grinding) {
                largest_grinding_diameter = std::max(largest_grinding_diameter, diameter);
            }
            if (grind_completed && circle_completed_ms == 0) {
                circle_completed_ms = now_ms();
            }
            if (circle_completed_ms != 0 && now_ms() - circle_completed_ms > kCircleSettleMs) {
                // Both the circle and the corners around it must end up green.
                lv_obj_t* screen = circle_screen.get_screen();
                const lv_color_t success = lv_color_hex(THEME_COLOR_SUCCESS);
                const bool grew = largest_grinding_diameter > THEME_GRIND_CIRCLE_MIN_DIAMETER_PX;
                const bool covers_display = lv_obj_get_style_bg_opa(screen, LV_PART_MAIN) == LV_OPA_COVER &&
                                            lv_obj_get_width(screen) == kDisplayWidth &&
                                            lv_obj_get_height(screen) == kDisplayHeight;
                const bool green = lv_color_eq(lv_obj_get_style_bg_color(screen, LV_PART_MAIN), success) &&
                                   lv_color_eq(lv_obj_get_style_bg_color(circle, LV_PART_MAIN), success);
                std::printf("[circle-smoke] largest while grinding=%ldpx covered=%s green=%s\n",
                            static_cast<long>(largest_grinding_diameter),
                            covers_display ? "yes" : "no", green ? "yes" : "no");
                if (!grew || !covers_display || !green) {
                    std::fprintf(stderr, "Circle screen did not grow and fill green.\n");
                    sim_platform::exit_process(8);
                }
                sim_platform::exit_process(0);
            }
            if (now_ms() - smoke_started_ms > kCircleSmokeTimeoutMs) {
                std::fprintf(stderr, "Circle grind did not complete.\n");
                sim_platform::exit_process(9);
            }
        }

        if (smoke_test && now_ms() - smoke_started_ms > 750) {
            if (!arc_screen.is_visible() || ready_screen.is_visible() || weight_g <= 0.0f) {
                std::fprintf(stderr, "Simulator scenario did not advance.\n");
                std::fflush(stderr);
                sim_platform::exit_process(2);
            }
            std::printf("[smoke] scenario advanced to %.2fg\n", weight_g);
            std::fflush(stdout);
            sim_platform::exit_process(0);
        }


        if (manual_smoke && smoke_scenario_started && !manual_stop_sent &&
            now_ms() - smoke_started_ms > 650) {
            if (!manual_grinding || !arc_screen.is_visible() || weight_g <= 0.0f) {
                std::fprintf(stderr, "Manual grind did not advance.\n");
                sim_platform::exit_process(6);
            }
            grind_button_event(nullptr);
            manual_stop_sent = true;
        }

        if (manual_smoke && manual_stop_sent && now_ms() - smoke_started_ms > 750) {
            if (grinding || settling || manual_grinding) {
                std::fprintf(stderr, "Manual grind did not stop.\n");
                sim_platform::exit_process(7);
            }
            std::printf("[manual-smoke] start/stop completed at %.2fg\n", weight_g);
            sim_platform::exit_process(0);
        }

        if (benchmark && !benchmark_switched_layout && now_ms() - smoke_started_ms > 2600) {
            print_render_metrics("arc", 2500);
            benchmark_arc_pixels = render_metrics.flushed_pixels;
            toggle_layout();
            benchmark_switched_layout = true;
            reset_render_metrics();
        }

        if (benchmark && benchmark_switched_layout && now_ms() - smoke_started_ms > 5100) {
            print_render_metrics("chart", 2500);
            if (benchmark_arc_pixels > kArcPixelBudget ||
                render_metrics.flushed_pixels > kChartPixelBudget) {
                std::fprintf(stderr, "Simulator render budget exceeded.\n");
                std::fflush(stderr);
                sim_platform::exit_process(3);
            }
            sim_platform::exit_process(0);
        }
    }

    lv_deinit();
    return 0;
}
