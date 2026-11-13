#pragma once

namespace gbarecomp {
class HostOverlay;
}  // namespace gbarecomp

namespace m3presence {
bool enabled();
void set_enabled(bool on);
void tick();
void render_overlay(gbarecomp::HostOverlay* overlay);
}  // namespace m3presence
