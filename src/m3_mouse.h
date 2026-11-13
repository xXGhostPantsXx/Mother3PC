#pragma once

#include "cstdint"

namespace gbarecomp {
struct TouchFrameInfo;
class HostOverlay;
}  // namespace gbarecomp

namespace m3mouse {
void install();
bool enabled();
void set_enabled(bool on);
int cursor_style();
void set_cursor_style(int style);
bool show_rings();
bool skip_logos();
void set_skip_logos(bool on);
void set_show_rings(bool on);
std::uint16_t input_frame(const gbarecomp::TouchFrameInfo* frame);
void render_overlay(gbarecomp::HostOverlay* overlay);
}  // namespace m3mouse
