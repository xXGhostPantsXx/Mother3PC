#include "m3_mouse.h"

#define SDL_MAIN_HANDLED
#include "SDL.h"

#include "algorithm"
#include "atomic"
#include "cmath"
#include "cstdint"

#include "gba_bus.h"
#include "host_overlay.h"
#include "runtime_bus_bridge.h"
#include "touch_input.h"

namespace m3mouse {
namespace {

constexpr std::uint16_t kKeyA      = 1u << 0;
constexpr std::uint16_t kKeyB      = 1u << 1;
constexpr std::uint16_t kKeySelect = 1u << 2;
constexpr std::uint16_t kKeyStart  = 1u << 3;
constexpr std::uint16_t kKeyRight  = 1u << 4;
constexpr std::uint16_t kKeyLeft   = 1u << 5;
constexpr std::uint16_t kKeyUp     = 1u << 6;
constexpr std::uint16_t kKeyDown   = 1u << 7;
constexpr std::uint16_t kKeyR      = 1u << 8;
constexpr std::uint16_t kKeyL      = 1u << 9;

// Cursor style: 0 = Feather, 1 = Dot, 2 = Default
std::atomic_bool g_enabled{true};
std::atomic_int g_cursor_style{0};
std::atomic_bool g_show_rings{true};
std::atomic_bool g_skip_logos{true};
int g_boot_frames = 0;
std::atomic_int g_wheel_y{0};
std::atomic_int g_wheel_x{0};

SDL_Cursor* g_feather_cursor = nullptr;
SDL_Cursor* g_dot_cursor = nullptr;
SDL_Cursor* g_arrow_cursor = nullptr;
bool g_feather_tried = false;
bool g_dot_tried = false;

bool g_prev_left = false;
bool g_prev_mid = false;
std::uint32_t g_left_down_ms = 0;
float g_left_down_vx = 0.0f;
float g_left_down_vy = 0.0f;
bool g_left_dragged = false;
int g_tap_a_frames = 0;
int g_tap_start_frames = 0;

int g_step_hold_frames = 0;
int g_step_gap_frames = 0;
std::uint16_t g_step_dir = 0;
int g_wheel_steps = 0;
std::uint16_t g_wheel_dir = 0;

float g_cur_vx = -1000.0f;
float g_cur_vy = -1000.0f;
bool g_moving = false;
bool g_dashing = false;
bool g_prev_dashing = false;
int g_dash_priming_frames = 0;

const char* const kFeather8x8[] = {
    "....OOO.",
    "...OWWWO",
    "..OWWSWO",
    ".OWSWWO.",
    ".OWQWOO.",
    "OQQWO...",
    "OQOO....",
    "OO......"
};

SDL_Cursor* build_hardware_feather_cursor() {
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return nullptr;
    constexpr int kScale = 3;
    constexpr int kSize = 8 * kScale;
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, kSize, kSize, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surf) return nullptr;

    const Uint32 col_clear   = SDL_MapRGBA(surf->format, 0, 0, 0, 0);
    const Uint32 col_outline = SDL_MapRGBA(surf->format, 22, 20, 32, 255);
    const Uint32 col_white   = SDL_MapRGBA(surf->format, 255, 255, 255, 255);
    const Uint32 col_shade   = SDL_MapRGBA(surf->format, 215, 228, 242, 255);
    const Uint32 col_quill   = SDL_MapRGBA(surf->format, 240, 195, 85, 255);

    SDL_FillRect(surf, nullptr, col_clear);
    for (int r = 0; r < 8; ++r) {
        const char* row = kFeather8x8[r];
        for (int c = 0; c < 8; ++c) {
            const char ch = row[c];
            if (ch == '.') continue;
            Uint32 col = col_outline;
            if (ch == 'W') col = col_white;
            else if (ch == 'S') col = col_shade;
            else if (ch == 'Q') col = col_quill;
            SDL_Rect px{c * kScale, r * kScale, kScale, kScale};
            SDL_FillRect(surf, &px, col);
        }
    }

    SDL_Cursor* cur = SDL_CreateColorCursor(surf, 0, kSize - 1);
    SDL_FreeSurface(surf);
    return cur;
}

SDL_Cursor* build_hardware_dot_cursor() {
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return nullptr;
    constexpr int kSize = 10;
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, kSize, kSize, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surf) return nullptr;

    const Uint32 col_clear   = SDL_MapRGBA(surf->format, 0, 0, 0, 0);
    const Uint32 col_outline = SDL_MapRGBA(surf->format, 20, 20, 30, 235);
    const Uint32 col_white   = SDL_MapRGBA(surf->format, 255, 255, 255, 250);

    SDL_FillRect(surf, nullptr, col_clear);
    SDL_Rect outer{2, 2, 6, 6};
    SDL_Rect inner{3, 3, 4, 4};
    SDL_FillRect(surf, &outer, col_outline);
    SDL_FillRect(surf, &inner, col_white);

    SDL_Cursor* cur = SDL_CreateColorCursor(surf, kSize / 2, kSize / 2);
    SDL_FreeSurface(surf);
    return cur;
}

void ensure_hardware_cursor() {
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return;
    if (!g_arrow_cursor) {
        g_arrow_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    }

    const bool on = g_enabled.load(std::memory_order_relaxed);
    const int style = g_cursor_style.load(std::memory_order_relaxed);

    if (!on || style == 2) {
        if (g_arrow_cursor && SDL_GetCursor() != g_arrow_cursor) {
            SDL_SetCursor(g_arrow_cursor);
        }
        SDL_ShowCursor(SDL_ENABLE);
        return;
    }

    if (style == 0) {
        if (!g_feather_cursor && !g_feather_tried) {
            g_feather_cursor = build_hardware_feather_cursor();
            g_feather_tried = true;
        }
        if (g_feather_cursor) {
            if (SDL_GetCursor() != g_feather_cursor) {
                SDL_SetCursor(g_feather_cursor);
            }
            SDL_ShowCursor(SDL_ENABLE);
        } else {
            SDL_ShowCursor(SDL_DISABLE);
        }
    } else {
        if (!g_dot_cursor && !g_dot_tried) {
            g_dot_cursor = build_hardware_dot_cursor();
            g_dot_tried = true;
        }
        if (g_dot_cursor) {
            if (SDL_GetCursor() != g_dot_cursor) {
                SDL_SetCursor(g_dot_cursor);
            }
            SDL_ShowCursor(SDL_ENABLE);
        } else {
            SDL_ShowCursor(SDL_DISABLE);
        }
    }
}

void draw_feather_overlay(gbarecomp::HostOverlay* overlay, float tip_x, float tip_y, bool dashing) {
    using gbarecomp::OverlayColor;
    using gbarecomp::OverlaySpace;

    const OverlayColor col_outline{22, 20, 32, 240};
    const OverlayColor col_white = dashing ? OverlayColor{255, 245, 195, 250} : OverlayColor{255, 255, 255, 250};
    const OverlayColor col_shade = dashing ? OverlayColor{245, 215, 130, 250} : OverlayColor{215, 228, 242, 250};
    const OverlayColor col_quill = dashing ? OverlayColor{255, 195, 65, 255}  : OverlayColor{240, 195, 85, 255};

    const float ox = std::floor(tip_x);
    const float oy = std::floor(tip_y) - 7.0f;

    for (int r = 0; r < 8; ++r) {
        const char* row = kFeather8x8[r];
        for (int c = 0; c < 8; ++c) {
            const char ch = row[c];
            if (ch == '.') continue;
            OverlayColor col = col_outline;
            if (ch == 'W') col = col_white;
            else if (ch == 'S') col = col_shade;
            else if (ch == 'Q') col = col_quill;
            overlay->fill_rect(ox + float(c), oy + float(r), 1.05f, 1.05f, 0.0f, col, OverlaySpace::View);
        }
    }
}

int SDLCALL event_watcher(void*, SDL_Event* event) {
    if (!event || !g_enabled.load(std::memory_order_relaxed)) return 0;
    if (event->type == SDL_MOUSEWHEEL) {
        int y = event->wheel.y;
        int x = event->wheel.x;
        if (event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            y = -y;
            x = -x;
        }
        const SDL_Keymod mod = SDL_GetModState();
        if (x != 0) {
            g_wheel_x.fetch_add(x, std::memory_order_relaxed);
        } else if (y != 0) {
            if (mod & KMOD_SHIFT) {
                g_wheel_x.fetch_add(-y, std::memory_order_relaxed);
            } else {
                g_wheel_y.fetch_add(y, std::memory_order_relaxed);
            }
        }
    } else if (event->type == SDL_WINDOWEVENT &&
               (event->window.event == SDL_WINDOWEVENT_ENTER || event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED)) {
        ensure_hardware_cursor();
    }
    return 0;
}

}  // namespace

void install() {
    static bool s_installed = false;
    if (!s_installed) {
        SDL_AddEventWatch(&event_watcher, nullptr);
        s_installed = true;
    }
    ensure_hardware_cursor();
}

bool enabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

void set_enabled(bool on) {
    g_enabled.store(on, std::memory_order_relaxed);
    ensure_hardware_cursor();
    if (!on) {
        g_moving = false;
        g_dashing = false;
        g_prev_dashing = false;
        g_prev_left = false;
        g_wheel_y.store(0, std::memory_order_relaxed);
        g_wheel_x.store(0, std::memory_order_relaxed);
    }
}

int cursor_style() {
    return g_cursor_style.load(std::memory_order_relaxed);
}

void set_cursor_style(int style) {
    if (style < 0) style = 0;
    if (style > 2) style = 2;
    g_cursor_style.store(style, std::memory_order_relaxed);
    ensure_hardware_cursor();
}

bool show_rings() {
    return g_show_rings.load(std::memory_order_relaxed);
}

void set_show_rings(bool on) {
    g_show_rings.store(on, std::memory_order_relaxed);
}

bool skip_logos() {
    return g_skip_logos.load(std::memory_order_relaxed);
}

void set_skip_logos(bool on) {
    g_skip_logos.store(on, std::memory_order_relaxed);
}

std::uint16_t input_frame(const gbarecomp::TouchFrameInfo* frame) {
    if (!g_enabled.load(std::memory_order_relaxed) || !frame) return 0x03FFu;

    ensure_hardware_cursor();

    int mx = 0, my = 0;
    const Uint32 buttons = SDL_GetMouseState(&mx, &my);
    const bool left  = (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
    const bool right = (buttons & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0;
    const bool mid   = (buttons & SDL_BUTTON(SDL_BUTTON_MIDDLE)) != 0;
    const bool x1    = (buttons & SDL_BUTTON(SDL_BUTTON_X1)) != 0;
    const bool x2    = (buttons & SDL_BUTTON(SDL_BUTTON_X2)) != 0;

    const auto& rect = frame->game_rect;
    const float rw = rect.width > 0 ? float(rect.width) : float(frame->drawable_width);
    const float rh = rect.height > 0 ? float(rect.height) : float(frame->drawable_height);
    const float vx = (rw > 0.0f)
        ? ((float(mx) - float(rect.x)) / rw) * float(frame->view_width)
        : float(frame->view_width) * 0.5f;
    const float vy = (rh > 0.0f)
        ? ((float(my) - float(rect.y)) / rh) * float(frame->view_height)
        : float(frame->view_height) * 0.5f;

    g_cur_vx = vx;
    g_cur_vy = vy;

    std::uint16_t pressed = 0;
    g_moving = false;
    g_dashing = false;

    if (left) {
        if (!g_prev_left) {
            g_left_down_ms = frame->host_ms;
            g_left_down_vx = vx;
            g_left_down_vy = vy;
            g_left_dragged = false;
            g_prev_dashing = false;
g_dash_priming_frames = 0;
}
const float drag_dx = vx - g_left_down_vx;
const float drag_dy = vy - g_left_down_vy;
const float drag_dist = std::hypot(drag_dx, drag_dy);
if (drag_dist > 4.0f) g_left_dragged = true;
if (g_left_dragged && drag_dist > 5.0f) {
g_moving = true;
std::uint16_t dir_bits = 0;
const float ax = std::fabs(drag_dx);
const float ay = std::fabs(drag_dy);
if (ax > ay * 0.4142f) dir_bits |= (drag_dx < 0.0f) ? kKeyLeft : kKeyRight;
if (ay > ax * 0.4142f) dir_bits |= (drag_dy < 0.0f) ? kKeyUp : kKeyDown;
if (drag_dist > 36.0f) {
g_dashing = true;
if (!g_prev_dashing) {
g_dash_priming_frames = 2;
}
pressed |= kKeyB;
if (g_dash_priming_frames > 0) {
--g_dash_priming_frames;
} else {
pressed |= dir_bits;
}
} else {
g_dash_priming_frames = 0;
pressed |= dir_bits;
}
}
} else if (g_prev_left) {
if (!g_left_dragged && (frame->host_ms - g_left_down_ms) < 260u) {
g_tap_a_frames = 4;
}
g_prev_dashing = false;
g_dash_priming_frames = 0;
}
g_prev_left = left;
g_prev_dashing = g_dashing;
if (g_tap_a_frames > 0) {
pressed |= kKeyA;
--g_tap_a_frames;
}
const int wy = g_wheel_y.exchange(0, std::memory_order_relaxed);
const int wx = g_wheel_x.exchange(0, std::memory_order_relaxed);
if (wy != 0) {
g_wheel_dir = (wy > 0) ? kKeyUp : kKeyDown;
g_wheel_steps = std::min(std::abs(wy), 6);
} else if (wx != 0) {
g_wheel_dir = (wx > 0) ? kKeyRight : kKeyLeft;
g_wheel_steps = std::min(std::abs(wx), 6);
}
if (g_step_hold_frames > 0) {
pressed |= g_step_dir;
--g_step_hold_frames;
if (g_step_hold_frames == 0) g_step_gap_frames = 2;
} else if (g_step_gap_frames > 0) {
--g_step_gap_frames;
} else if (g_wheel_steps > 0) {
g_step_dir = g_wheel_dir;
g_step_hold_frames = 3;
--g_wheel_steps;
pressed |= g_step_dir;
}
if (right) pressed |= kKeyB;
if (mid && !g_prev_mid) g_tap_start_frames = 4;
g_prev_mid = mid;
if (g_tap_start_frames > 0) {
pressed |= kKeyStart;
--g_tap_start_frames;
}
if (x1) pressed |= kKeyL;
if (x2) pressed |= kKeyR;
    if (g_skip_logos.load(std::memory_order_relaxed) && g_boot_frames < 600) {
        ++g_boot_frames;
        if (pressed != 0) {
            // Stop auto-skipping immediately if the user presses a button.
            g_boot_frames = 600;
        } else {
            int map_id = 0;
            if (gba::GbaBus* bus = gbarecomp::active_bus()) {
                if (const std::uint8_t* ew = bus->ewram_ptr()) {
                    const std::uint32_t o = (0x020281E4u - 0x02000000u) & 0x3FFFFu;
                    map_id = int(*(ew + o) | (*(ew + o + 1) << 8));
                }
            }
            if (map_id == 0 && g_boot_frames >= 170 && g_boot_frames <= 595 && ((g_boot_frames % 8) < 2)) {
                pressed |= kKeyA;
            }
        }
    }
return std::uint16_t(~pressed) & 0x03FFu;
}
void render_overlay(gbarecomp::HostOverlay* overlay) {
if (!g_enabled.load(std::memory_order_relaxed) || !overlay) return;
if (g_cur_vx < -50.0f || g_cur_vy < -50.0f) return;
using gbarecomp::OverlayColor;
using gbarecomp::OverlaySpace;
if (g_moving && g_show_rings.load(std::memory_order_relaxed)) {
const OverlayColor line_col = g_dashing
? OverlayColor{255, 210, 80, 160}
: OverlayColor{120, 235, 170, 140};
overlay->stroke_circle(g_left_down_vx, g_left_down_vy, 5.0f, 0.9f, OverlayColor{255, 255, 255, 85}, OverlaySpace::View);
overlay->line(g_left_down_vx, g_left_down_vy, g_cur_vx, g_cur_vy, 1.1f, line_col, OverlaySpace::View);
overlay->stroke_circle(g_cur_vx, g_cur_vy, 3.8f, 0.9f, line_col, OverlaySpace::View);
}
const int style = g_cursor_style.load(std::memory_order_relaxed);
if (style == 0 && !g_feather_cursor) {
draw_feather_overlay(overlay, g_cur_vx, g_cur_vy, g_dashing);
} else if (style == 1 && !g_dot_cursor) {
overlay->fill_circle(g_cur_vx, g_cur_vy, 2.2f, OverlayColor{15, 15, 25, 190}, OverlaySpace::View);
overlay->fill_circle(g_cur_vx, g_cur_vy, 1.3f,
g_dashing ? OverlayColor{255, 220, 90, 235} : OverlayColor{255, 255, 255, 220},
OverlaySpace::View);
}
}
}  // namespace m3mouse
