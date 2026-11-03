#include "screenshot.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace sim_screenshot {

namespace {

void put_u16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void put_u32(std::vector<uint8_t>& out, uint32_t value) {
    put_u16(out, static_cast<uint16_t>(value));
    put_u16(out, static_cast<uint16_t>(value >> 16));
}

}  // namespace

bool save_bmp(lv_obj_t* obj, const char* path) {
    lv_draw_buf_t* snapshot = lv_snapshot_take(obj, LV_COLOR_FORMAT_RGB888);
    if (!snapshot) {
        return false;
    }

    const uint32_t width = snapshot->header.w;
    const uint32_t height = snapshot->header.h;
    // BMP rows are padded to a multiple of 4 bytes.
    const uint32_t row_bytes = (width * 3 + 3) & ~3u;
    constexpr uint32_t kHeaderBytes = 14 + 40;

    std::vector<uint8_t> file;
    file.reserve(kHeaderBytes + row_bytes * height);
    file.push_back('B');
    file.push_back('M');
    put_u32(file, kHeaderBytes + row_bytes * height);
    put_u32(file, 0);
    put_u32(file, kHeaderBytes);
    put_u32(file, 40);
    put_u32(file, width);
    put_u32(file, height);
    put_u16(file, 1);
    put_u16(file, 24);
    put_u32(file, 0);
    put_u32(file, row_bytes * height);
    put_u32(file, 2835);  // 72 DPI
    put_u32(file, 2835);
    put_u32(file, 0);
    put_u32(file, 0);

    // LVGL's RGB888 is stored blue first, as BMP expects, but BMP rows run
    // bottom to top.
    for (uint32_t row = height; row-- > 0;) {
        const uint8_t* pixels = snapshot->data + row * snapshot->header.stride;
        file.insert(file.end(), pixels, pixels + width * 3);
        file.resize(file.size() + (row_bytes - width * 3), 0);
    }
    lv_draw_buf_destroy(snapshot);

    std::FILE* out = std::fopen(path, "wb");
    if (!out) {
        return false;
    }
    const bool written = std::fwrite(file.data(), 1, file.size(), out) == file.size();
    return std::fclose(out) == 0 && written;
}

}  // namespace sim_screenshot
