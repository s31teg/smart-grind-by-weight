#pragma once
#include <lvgl.h>

class UIManager;

// Shows Bluetooth and Wi-Fi connection status icons with color coding
// Shows diagnostic warning icon when issues detected
// Only on screens laid out around them (Ready, grinding and results, update
// progress); menus, editors and pop-ups use the top edge for their own titles.

class StatusIndicatorController {
public:
    explicit StatusIndicatorController(UIManager* manager);

    void build();
    void update();
    void prompt_firmware_update();

private:
    void update_ble_status_icon();
    void update_wifi_status_icon();
    void update_warning_icon();
    void update_firmware_update_icon();
    bool icons_allowed_on_current_screen() const;
    bool on_result_color_screen() const;
    lv_color_t icon_color(uint32_t color) const;
    void set_icon_visible(lv_obj_t* icon, bool visible) const;

    UIManager* ui_manager_;
    lv_obj_t* ble_status_icon_ = nullptr;
    lv_obj_t* wifi_status_icon_ = nullptr;
    lv_obj_t* warning_icon_ = nullptr;
    lv_obj_t* firmware_update_icon_ = nullptr;
    int8_t last_ble_state_ = -1;
    int8_t last_wifi_state_ = -1;
    int8_t last_warning_state_ = -1;
    int8_t last_firmware_update_state_ = -1;
    bool icons_allowed_ = true;
    bool on_result_color_ = false;
};
