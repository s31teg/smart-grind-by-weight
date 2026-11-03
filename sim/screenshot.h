#pragma once

#include <lvgl.h>

namespace sim_screenshot {

// Renders the object and its children and saves them as a 24-bit BMP file.
// Returns false if the snapshot or the file write fails.
bool save_bmp(lv_obj_t* obj, const char* path);

}  // namespace sim_screenshot
