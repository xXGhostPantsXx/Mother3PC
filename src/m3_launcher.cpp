#include "m3_launcher.h"

#include "m3_mods.h"
#include "m3_discord_auth.h"
#include "m3_updater.h"
#include "m3_setup.h"

#include <SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

#if defined(GBAGAME_RECOMP_UI)
#include "gba_pad_binds.h"
#endif

namespace m3launcher {
namespace {

// 16:9 canvas; deliberately not 240x160 so the in-game bezel hook ignores it.
constexpr int kW = 320, kH = 180;
constexpr std::uint32_t kInk = 0xFF383040u, kDim = 0xFF8A7F86u, kHot = 0xFFC0304Au;

// Scene: the crossroads by the bell tower north of Tazmily (map 36, 64 tiles wide).
constexpr int kSceneMap = 36, kSceneWidthTiles = 64;
struct Point { int x, y; };
// The camera holds on the crossroads (feet at 512,384 sit mid-screen between the bars).
constexpr Point kCamera = {352, 286};
// Walks through the crossroads along the dirt roads: each pass starts just out of view
// and ends once the last member has left it, so the party never turns back on itself.
struct Pass { Point pts[3]; int count; };
constexpr Pass kPasses[] = {
    {{{310, 384}, {774, 384}}, 2},              // west to east
    {{{512, 290}, {512, 554}}, 2},              // north to south
    {{{714, 384}, {512, 384}, {512, 230}}, 3},  // east, turning north
    {{{512, 494}, {512, 384}, {250, 384}}, 3},  // south, turning west
};
constexpr int kPassCount = sizeof kPasses / sizeof kPasses[0];

// stride: frame offsets for step, stand, step, stand; null uses the sprite's ROM walk cycle.
// Duster's ROM cycle is his limp, so the launcher steps him between his two stride poses.
constexpr int kDusterStride[4] = {16, 0, 40, 0};
struct Member { const char* name; int sprite; const int* stride; };
constexpr Member kParty[] = {{"Lucas", 4, nullptr}, {"Kumatora", 13, nullptr},
                             {"Duster", 15, kDusterStride}, {"Boney", 10, nullptr}};
constexpr int kPartySize = 4;
constexpr int kFollowGap = 18;  // frames of leader history between party members

// Window flavors: 7 sets of 6 menu palettes in the ROM; the first of each is the window.
const char* const kFlavors[] = {"Plain", "Mint", "Strawberry", "Banana", "Peanut", "Grape", "Melon"};
const char* const kDisplayModes[] = {"Windowed", "Fullscreen", "Exclusive fullscreen"};
const char* const kScreens[] = {"raw", "backlit", "frontlit", "unlit", "classic"};
const char* const kScreenNames[] = {"Raw", "Backlit", "Frontlit", "Unlit", "Classic"};
const char* const kPromptStyles[] = {"Auto", "Xbox", "PlayStation", "Nintendo", "Keyboard"};

// Standing frames run clockwise from facing down; walk cycles are +8 and +16.
enum Dir { kDown = 0, kLeft = 2, kUp = 4, kRight = 6 };

struct Canvas {
    std::vector<std::uint32_t> px = std::vector<std::uint32_t>(kW * kH);
    void fill(int x, int y, int w, int h, std::uint32_t c) {
        for (int j = std::max(y, 0); j < std::min(y + h, kH); ++j)
            for (int i = std::max(x, 0); i < std::min(x + w, kW); ++i) px[j * kW + i] = c;
    }
    // Draws img with its top-left at (x, y); tint != 0 recolours opaque pixels.
    void draw(const m3assets::Image& img, int x, int y, std::uint32_t tint = 0) {
        for (int j = std::max(0, -y); j < img.h && y + j < kH; ++j)
            for (int i = std::max(0, -x); i < img.w && x + i < kW; ++i) {
                const std::uint32_t c = img.px[j * img.w + i];
                if (c >> 24) px[(y + j) * kW + x + i] = tint ? tint : c;
            }
    }
    // Opaque blit of a kW x kH window of a larger image.
    void blit_view(const m3assets::Image& img, int sx, int sy) {
        for (int j = 0; j < kH && sy + j < img.h; ++j)
            std::copy_n(&img.px[(sy + j) * img.w + sx], std::min(kW, img.w - sx), &px[j * kW]);
    }
};

struct Ui {
    const m3assets::Rom& rom;
    Canvas cv;
    int width(const std::string& s) const {
        int w = 0, adv;
        for (char c : s) { rom.glyph(c, &adv); w += adv; }
        return w;
    }
    void text(const std::string& s, int x, int y, std::uint32_t color = kInk) {
        int adv;
        for (char c : s) {
            cv.draw(rom.glyph(c, &adv), x, y, color);
            x += adv;
        }
    }
    void text_right(const std::string& s, int right, int y, std::uint32_t color = kInk) {
        text(s, right - width(s), y, color);
    }
    // Right-pointing triangle; the ROM font has no usable '<' / '>'. Callers pass the
    // text's y + 4; the 7px arrow is lifted 3px so it centres on the 8px capitals.
    void cursor(int x, int y, std::uint32_t color = kInk) {
        for (int k = 0; k < 4; ++k) cv.fill(x + k, y - 3 + k, 1, 7 - 2 * k, color);
    }
    void cursor_left(int x, int y, std::uint32_t color) {
        for (int k = 0; k < 4; ++k) cv.fill(x + 3 - k, y - 3 + k, 1, 7 - 2 * k, color);
    }
    // "< value >" right-aligned at `right`, used for adjustable settings.
    void choice(const std::string& v, int right, int y, bool active) {
        const std::uint32_t c = active ? kHot : kInk;
        if (!active) { text_right(v, right, y, c); return; }
        cursor(right - 4, y + 4, c);
        text_right(v, right - 8, y, c);
        cursor_left(right - 8 - width(v) - 8, y + 4, c);
    }
};

enum class Input { None, Up, Down, Left, Right, Confirm, Back, PrevPage, NextPage };

Input map_key(SDL_Keycode k) {
    switch (k) {
        case SDLK_UP: case SDLK_w: return Input::Up;
        case SDLK_DOWN: case SDLK_s: return Input::Down;
        case SDLK_LEFT: case SDLK_a: return Input::Left;
        case SDLK_RIGHT: case SDLK_d: return Input::Right;
        case SDLK_RETURN: case SDLK_x: case SDLK_SPACE: return Input::Confirm;
        case SDLK_ESCAPE: case SDLK_z: case SDLK_BACKSPACE: return Input::Back;
        case SDLK_q: case SDLK_PAGEUP: return Input::PrevPage;
        case SDLK_e: case SDLK_PAGEDOWN: return Input::NextPage;
        default: return Input::None;
    }
}

Input map_button(Uint8 b) {
    switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return Input::Up;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return Input::Down;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return Input::Left;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return Input::Right;
        case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_START: return Input::Confirm;
        case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_BACK: return Input::Back;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return Input::PrevPage;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return Input::NextPage;
        default: return Input::None;
    }
}

// The left stick drives menus like the D-pad: one step per push past the threshold.
struct Stick {
    bool held[2] = {false, false};
    Input input(const SDL_ControllerAxisEvent& a) {
        if (a.axis != SDL_CONTROLLER_AXIS_LEFTX && a.axis != SDL_CONTROLLER_AXIS_LEFTY) return Input::None;
        const int k = a.axis == SDL_CONTROLLER_AXIS_LEFTY;
        if (std::abs(a.value) < 12000) { held[k] = false; return Input::None; }
        if (held[k] || std::abs(a.value) < 20000) return Input::None;
        held[k] = true;
        if (k) return a.value < 0 ? Input::Up : Input::Down;
        return a.value < 0 ? Input::Left : Input::Right;
    }
};

struct Step { Point at; Dir dir; };

// Moves the leader 1px per frame toward the next point of the current pass.
struct Walker {
    int pass = 0, target = 1;
    Point at = kPasses[0].pts[0];
    Dir dir = kRight;
    std::deque<Step> history;
    void tick() {
        const Point t = kPasses[pass].pts[target];
        if (at.x == t.x && at.y == t.y && ++target == kPasses[pass].count) {
            pass = (pass + 1) % kPassCount;  // everyone is out of view: start the next road
            target = 1;
            at = kPasses[pass].pts[0];
            history.clear();
        }
        const Point n = kPasses[pass].pts[target];
        if (n.x != at.x) { dir = n.x > at.x ? kRight : kLeft; at.x += n.x > at.x ? 1 : -1; }
        else if (n.y != at.y) { dir = n.y > at.y ? kDown : kUp; at.y += n.y > at.y ? 1 : -1; }
        history.push_front({at, dir});
        if (history.size() > std::size_t(kFollowGap * kPartySize)) history.pop_back();
    }
    Step member(int k) const {
        const std::size_t i = std::size_t(k * kFollowGap);
        return i < history.size() ? history[i] : (history.empty() ? Step{at, dir} : history.back());
    }
};

// Names an SDL gamepad source ("a", "leftshoulder", "lefty-") the way the
// connected controller labels it.
std::string pad_name(const std::string& src, SDL_GameControllerType type) {
    const bool ps = type == SDL_CONTROLLER_TYPE_PS3 || type == SDL_CONTROLLER_TYPE_PS4 ||
                    type == SDL_CONTROLLER_TYPE_PS5;
    const bool nin = type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO;
    struct Row { const char *sdl, *xbox, *playstation, *nintendo; };
    // SDL names face buttons by position, so Nintendo's labels are swapped.
    static const Row kRows[] = {
        {"a", "A", "Cross", "B"},          {"b", "B", "Circle", "A"},
        {"x", "X", "Square", "Y"},         {"y", "Y", "Triangle", "X"},
        {"leftshoulder", "LB", "L1", "L"}, {"rightshoulder", "RB", "R1", "R"},
        {"lefttrigger", "LT", "L2", "ZL"}, {"righttrigger", "RT", "R2", "ZR"},
        {"lefttrigger+", "LT", "L2", "ZL"}, {"righttrigger+", "RT", "R2", "ZR"},
        {"triggerleft", "LT", "L2", "ZL"}, {"triggerright", "RT", "R2", "ZR"},
        {"triggerleft+", "LT", "L2", "ZL"}, {"triggerright+", "RT", "R2", "ZR"},
        {"back", "View", "Share", "-"},    {"start", "Menu", "Options", "+"},
        {"guide", "Xbox", "PS", "Home"},   {"leftstick", "LS", "L3", "LS"},
        {"rightstick", "RS", "R3", "RS"},  {"dpup", "D-Up", "D-Up", "D-Up"},
        {"dpdown", "D-Down", "D-Down", "D-Down"},
        {"dpleft", "D-Left", "D-Left", "D-Left"},
        {"dpright", "D-Right", "D-Right", "D-Right"},
        {"leftx-", "LS Left", "LS Left", "LS Left"},
        {"leftx+", "LS Right", "LS Right", "LS Right"},
        {"lefty-", "LS Up", "LS Up", "LS Up"},
        {"lefty+", "LS Down", "LS Down", "LS Down"},
    };
    for (const Row& r : kRows)
        if (src == r.sdl) return ps ? r.playstation : nin ? r.nintendo : r.xbox;
    return src.empty() ? "-" : src;
}

// Gamepad binds live in input.ini through recomp-ui's per-controller store.
struct Pads {
    std::string path, guid;
    SDL_GameControllerType type = SDL_CONTROLLER_TYPE_UNKNOWN;
    void refresh() {
        guid.clear();
        for (int i = 0; i < SDL_NumJoysticks(); ++i)
            if (SDL_IsGameController(i)) {
                char buf[64];
                SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), buf, sizeof buf);
                guid = buf;
                type = SDL_GameControllerTypeForIndex(i);
                break;
            }
    }
#if defined(GBAGAME_RECOMP_UI)
    bool available() const { return !guid.empty(); }
    std::string label(int b) const {
        char out[64] = {};
        rui_pad_binds_label(&kGbaPadSpec, path.c_str(), guid.c_str(), b, out, sizeof out);
        return pad_name(out, type);
    }
    void write_guid_header() const {
        if (guid.empty() || path.empty()) return;
        std::vector<std::string> lines;
        {
            std::ifstream in(path);
            std::string line;
            bool in_ctrl = false;
            while (std::getline(in, line)) {
                while (!line.empty() && line.back() == '\r') line.pop_back();
                const size_t st = line.find_first_not_of(" \t");
                if (st != std::string::npos && line[st] == '[')
                    in_ctrl = (line.compare(st, 12, "[controller]") == 0);
                if (in_ctrl) continue;
                lines.push_back(line);
            }
        }
        while (!lines.empty() && lines.back().empty()) lines.pop_back();
        std::ofstream out(path, std::ios::trunc);
        if (!out) return;
        out << "[controller]\nguid = " << guid << "\n";
        for (const auto& l : lines) out << l << "\n";
    }
    void set(int b, int kind, int code, int dir) {
        rui_pad_binds_set(&kGbaPadSpec, path.c_str(), guid.c_str(), b, kind, code, dir);
        write_guid_header();
    }
    void reset() { rui_pad_binds_reset(&kGbaPadSpec, path.c_str(), guid.c_str()); }
#else
    bool available() const { return false; }
    std::string label(int) const { return "-"; }
    void set(int, int, int, int) {}
    void reset() {}
#endif
};

enum class Page { Main, Settings, Controls, Mods, Account, Updates };
const char* const kItems[] = {"Play", "Settings", "Controls", "Mods", "Account", "Quit"};
constexpr int kItemCount = 6;
constexpr int kSettingRows = 12;  // 10 options + Import a save + Change game files
constexpr int kControlRows = m3settings::kButtonCount + 1;  // buttons + Reset

// Smooth motion only shows on a display of 100 Hz or more (m3_smooth.cpp).
int display_hz(SDL_Window* win) {
    SDL_DisplayMode mode{};
    const int index = SDL_GetWindowDisplayIndex(win);
    return SDL_GetCurrentDisplayMode(index < 0 ? 0 : index, &mode) == 0 ? mode.refresh_rate : 0;
}

void apply_display(SDL_Window* win, int mode) {
    SDL_SetWindowFullscreen(win, mode == 1 ? SDL_WINDOW_FULLSCREEN_DESKTOP : mode == 2 ? SDL_WINDOW_FULLSCREEN : 0);
}

}  // namespace

Result run(const m3assets::Rom& rom, m3settings::Settings& st, const std::string& dir) {
    const m3assets::Image scene = rom.map(kSceneMap, kSceneWidthTiles);
    if (scene.w < kW || scene.h < kH) return Result::Play;  // can't draw the launcher; just boot
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) return Result::Play;
    SDL_Window* win = SDL_CreateWindow("MOTHER 3", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       kW * 4, kH * 4, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    SDL_Texture* tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kW, kH) : nullptr;
    if (!tex) {
        if (ren) SDL_DestroyRenderer(ren);
        if (win) SDL_DestroyWindow(win);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
        return Result::Play;  // fall back to booting the game rather than blocking the player
    }
    SDL_RenderSetLogicalSize(ren, kW, kH);
    SDL_RenderSetIntegerScale(ren, SDL_TRUE);
    apply_display(win, st.fullscreen);
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
    Pads pads{dir + "/input.ini", {}};
    pads.refresh();
#if defined(GBAGAME_RECOMP_UI)
    rui_pad_binds_init(&kGbaPadSpec, pads.path.c_str());
#endif

    // Windows are rebuilt whenever the flavor changes.
    int built_flavor = -1;
    m3assets::Image command_bar, status_box, note, page_win, upd_box;
    auto build_windows = [&] {
        const int pal = st.flavor * 6;
        command_bar = rom.window(kW / 8, 3, pal);
        status_box = rom.window(kW / 8 / kPartySize, 3, pal);  // same height as the command bar
        note = rom.window(24, 5, pal);
        page_win = rom.window(38, 19, pal);
        upd_box = rom.window(28, 7, pal);
        built_flavor = st.flavor;
    };

    // Test hooks: M3_LAUNCHER_PAGE opens a page; M3_LAUNCHER_SHOT saves the canvas and quits;
    // M3_LAUNCHER_PADS="frame:button,..." (SDL button numbers) presses controller buttons.
    const char* shot = std::getenv("M3_LAUNCHER_SHOT");
    const char* pad_script = std::getenv("M3_LAUNCHER_PADS");
    const int shot_frame = std::getenv("M3_LAUNCHER_SHOT_FRAME") ? std::atoi(std::getenv("M3_LAUNCHER_SHOT_FRAME")) : 150;
    Page page = Page::Main;
    int cursor_at = 0;
    bool hover_updates = false;
    if (const char* p = std::getenv("M3_LAUNCHER_PAGE")) {
        page = static_cast<Page>(std::clamp(std::atoi(p), 0, 3));
        cursor_at = static_cast<int>(page);
    }

    Ui ui{rom, {}};
    std::vector<int> item_x;
    for (int i = 0, x = 14; i < kItemCount; ++i) {  // same column as the page text below
        item_x.push_back(x);
        x += ui.width(kItems[i]) + 14;
    }

    Walker walker;
    Stick stick;
    int row = 0, frame = 0;
    double ticks_due = 0;  // 60 Hz steps owed to the party walk
    Uint64 last_tick = SDL_GetPerformanceCounter();
    bool capturing = false, running = true, using_pad = false;
    Result result = Result::Quit;

    std::vector<m3mods::Mod> mods;
    int mods_scroll = 0; m3discord::Config discord_cfg = m3discord::load_config(dir); m3discord::Session discord_session = m3discord::load_session(dir);
    m3updater::Config upd_cfg = m3updater::load_config(dir);
    m3updater::State upd_state;
    upd_state.patch_notes = m3updater::load_local_notes(dir);
    bool update_prompt = false;
    int prompt_row = 0;
    if (!upd_cfg.manifest_url.empty()) m3updater::begin_check(dir);
    int notes_scroll = 0;
    auto open_mods = [&] {
        mods = m3mods::list(dir);
        m3mods::classify(dir, rom.bytes(), mods);
        mods_scroll = 0;
    };
    if (page == Page::Mods) open_mods();
    auto leave_page = [&] {
        if (page == Page::Mods) m3mods::save_enabled(dir, mods);
        else m3settings::save(dir, st);
        page = Page::Main;
        capturing = false;
    };
    auto open_page = [&](Page p) {
        page = p;
        cursor_at = static_cast<int>(p);
        row = 0;
        if (p == Page::Mods) open_mods();
        if (p == Page::Updates) notes_scroll = 0;
    };
    auto try_play = [&] {
        if (discord_cfg.require_playtester && !discord_session.is_playtester) { open_page(Page::Account); return; }
        if (upd_state.update_available && !update_prompt) { update_prompt = true; prompt_row = 0; return; }
        result = Result::Play; running = false;
    };
    // Bottom-of-page help in the labels of whatever the player last pressed.
    auto hint = [&](const char* confirm) {
        if (!using_pad) return std::string("X ") + confirm + "    Z Back    Q / E Page";
        return pad_name("a", pads.type) + " " + confirm + "    " + pad_name("b", pads.type) + " Back    " +
               pad_name("leftshoulder", pads.type) + " / " + pad_name("rightshoulder", pads.type) + " Page";
    };
    auto change_setting = [&](int delta) {
        switch (row) {
            case 0: st.flavor = (st.flavor + 7 + delta) % 7; break;
            case 1: st.fullscreen = (st.fullscreen + 3 + delta) % 3; apply_display(win, st.fullscreen); break;
            case 2: st.scale = std::clamp(st.scale + delta, 1, 10); break;
            case 3: st.sharp_filter = !st.sharp_filter; break;
            case 4: {
                int i = 0;
                while (i < 5 && st.screen != kScreens[i]) ++i;
                st.screen = kScreens[((i % 5) + 5 + delta) % 5];
                break;
            }
            case 5: st.volume = std::clamp(st.volume + delta * 10, 0, 100); break;
            case 6: st.prompt_style = (st.prompt_style + 5 + delta) % 5; break;
            case 7: st.widescreen = !st.widescreen; break;
            case 8: st.smooth = (st.smooth + 3 + delta) % 3; break;
            case 9: st.hq_audio = !st.hq_audio; break;
        }
    };

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { running = false; break; }
            if (e.type == SDL_CONTROLLERDEVICEADDED) { SDL_GameControllerOpen(e.cdevice.which); pads.refresh(); }
            if (e.type == SDL_CONTROLLERDEVICEREMOVED) pads.refresh();

            if (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEWHEEL) {
                float lx = -1.0f, ly = -1.0f;
                int mx = 0, my = 0;
                SDL_GetMouseState(&mx, &my);
                SDL_RenderWindowToLogical(ren, mx, my, &lx, &ly);
                const int ix = static_cast<int>(lx);
                const int iy = static_cast<int>(ly);

                if (capturing) {
                    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT) {
                        capturing = false;
                    }
                    continue;
                }

                constexpr int mPx = 8, mPy = 26, mLabelX = mPx + 18, mRight = mPx + 38 * 8 - 14;

                // Top command bar hover & click (works from Main or any sub-page)
                int hit_tab = -1;
                if (iy >= 0 && iy < 24) {
                    for (int i = 0; i < kItemCount; ++i) {
                        const int x0 = item_x[i] - 10;
                        const int x1 = item_x[i] + ui.width(kItems[i]) + 6;
                        if (ix >= x0 && ix <= x1) { hit_tab = i; break; }
                    }
                }
                if (hit_tab != -1) {
                    hover_updates = false;
                    using_pad = false;
                    if (page == Page::Main) cursor_at = hit_tab;
                    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                        if (page != Page::Main && hit_tab != static_cast<int>(page)) leave_page();
                        if (hit_tab == 0) {
                            try_play();
                        } else if (hit_tab == kItemCount - 1) {
                            running = false;
                        } else if (page == Page::Main || hit_tab != static_cast<int>(page)) {
                            open_page(static_cast<Page>(hit_tab));
                        }
                        continue;
                    }
                }

                // Update Available modal popup mouse handling
                if (update_prompt && page == Page::Main) {
                    const int ux = (kW - upd_box.w) / 2, uy = (kH - upd_box.h) / 2;
                    for (int i = 0; i < 2; ++i) {
                        const int ry = uy + 24 + i * 12;
                        if (ix >= ux + 8 && ix <= ux + upd_box.w - 8 && iy >= ry - 2 && iy < ry + 10) {
                            prompt_row = i;
                            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                                using_pad = false;
                                update_prompt = false;
                                if (prompt_row == 0) {
                                    open_page(Page::Updates);
                                    if (!upd_state.downloading) m3updater::begin_download_and_apply(dir);
                                } else {
                                    result = Result::Play; running = false;
                                }
                            }
                            break;
                        }
                    }
                    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT) update_prompt = false;
                    continue;
                }

                // Bottom bar hover & click on Page::Main (Updates button)
                if (page == Page::Main) {
                    const std::string upd_lbl = upd_state.update_available ? "Update!" : "Updates";
                    const bool over_upd = (iy >= kH - command_bar.h && iy < kH && ix >= 4 && ix <= 18 + ui.width(upd_lbl));
                    if (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN) {
                        hover_updates = over_upd;
                        if (over_upd) using_pad = false;
                    }
                    if (over_upd && e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                        hover_updates = false;
                        open_page(Page::Updates);
                        continue;
                    }
                }

                // Mouse wheel scrolling on Mods or Updates page
                if (e.type == SDL_MOUSEWHEEL && page == Page::Updates) {
                    const int max_scroll = std::max(0, static_cast<int>(upd_state.patch_notes.size()) - 6);
                    if (e.wheel.y > 0) notes_scroll = std::max(0, notes_scroll - 1);
                    else if (e.wheel.y < 0) notes_scroll = std::min(max_scroll, notes_scroll + 1);
                    continue;
                }
                if (e.type == SDL_MOUSEWHEEL && page == Page::Mods) {
                    const int rows = static_cast<int>(mods.size()) + 1;
                    if (e.wheel.y > 0) row = std::max(0, row - 1);
                    else if (e.wheel.y < 0) row = std::min(rows - 1, row + 1);
                    continue;
                }

                // Sub-page hover & click interactions
                if (page != Page::Main && ix >= mLabelX - 12 && ix <= mRight + 4) {
                    if (page == Page::Settings) {
                        for (int i = 0; i < kSettingRows; ++i) {
                            const int y0 = mPy + 5 + i * 10;
                            if (iy >= y0 && iy < y0 + 10) {
                                row = i;
                                if (e.type == SDL_MOUSEBUTTONDOWN) {
                                    using_pad = false;
                                    if (e.button.button == SDL_BUTTON_LEFT) {
                                        if (row == kSettingRows - 1) {
                                            leave_page();
                                            result = Result::ChangeFiles;
                                            running = false;
                                        } else if (row == kSettingRows - 2) {
                                            m3setup::import_save(dir);
                                        } else {
                                            change_setting(1);
                                        }
                                    } else if (e.button.button == SDL_BUTTON_RIGHT && row < kSettingRows - 2) {
                                        change_setting(-1);
                                    }
                                }
                                break;
                            }
                        }
                    } else if (page == Page::Controls) {
                        for (int i = 0; i < kControlRows; ++i) {
                            const int y0 = mPy + 15 + i * 10;
                            if (iy >= y0 && iy < y0 + 10) {
                                row = i;
                                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                                    using_pad = false;
                                    if (row == kControlRows - 1) {
                                        st.keys = m3settings::defaults().keys;
                                        pads.reset();
                                    } else {
                                        capturing = true;
                                    }
                                }
                                break;
                            }
                        }
                    } else if (page == Page::Mods) {
                        constexpr int kVisible = 7;
                        const int count = static_cast<int>(mods.size());
                        for (int v = 0; v < std::min(kVisible, count); ++v) {
                            const int idx = mods_scroll + v;
                            if (idx >= count) break;
                            const int y0 = mPy + 9 + v * 13;
                            if (iy >= y0 && iy < y0 + 13) {
                                row = idx;
                                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT && mods[row].valid) {
                                    using_pad = false;
                                    mods[row].enabled = !mods[row].enabled;
                                }
                                break;
                            }
                        }
                        const int y_open = mPy + 10 + kVisible * 13 + 3;
                        if (iy >= y_open && iy < y_open + 13) {
                            row = count;
                            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                                using_pad = false;
                                const std::string url = "file:///" + m3mods::folder(dir);
                                SDL_OpenURL(url.c_str());
                            }
                        }
                    } else if (page == Page::Updates) {
                        for (int i = 0; i < 2; ++i) {
                            const int y0 = mPy + 35 + i * 12;
                            if (iy >= y0 && iy < y0 + 12) {
                                row = i;
                                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                                    using_pad = false;
                                    if (row == 0) {
                                        upd_cfg = m3updater::load_config(dir);
                                        m3updater::begin_check(dir);
                                    } else if (row == 1 && upd_state.update_available && !upd_state.downloading) {
                                        m3updater::begin_download_and_apply(dir);
                                    }
                                }
                                break;
                            }
                        }
                    } else if (page == Page::Account) {
                        for (int i = 0; i < 3; ++i) {
                            const int y0 = mPy + 71 + i * 13;
                            if (iy >= y0 && iy < y0 + 13) {
                                row = i;
                                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                                    using_pad = false;
                                    if (row == 0) {
                                        discord_cfg = m3discord::load_config(dir);
                                        m3discord::begin_login(dir);
                                    } else if (row == 1) {
                                        discord_cfg = m3discord::load_config(dir);
                                        m3discord::begin_refresh(dir);
                                    } else if (row == 2) {
                                        m3discord::cancel_login();
                                        m3discord::clear_session(dir);
                                        discord_session = m3discord::Session{};
                                    }
                                }
                                break;
                            }
                        }
                    }
                }

                // Right-click outside interactive rows on any sub-page goes Back
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT && page != Page::Main) {
                    if (page == Page::Account) m3discord::cancel_login();
                    leave_page();
                }
                continue;
            }

            if (capturing) {  // a key rebinds the keyboard, a button/trigger the controller; Escape cancels
                const int b = row;
                if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                    if (e.key.keysym.scancode != SDL_SCANCODE_ESCAPE)
                        st.keys[b] = SDL_GetScancodeName(e.key.keysym.scancode);
                    capturing = false;
                } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                    pads.set(b, 1, e.cbutton.button, 0);
                    capturing = false;
                } else if (e.type == SDL_CONTROLLERAXISMOTION) {
                    const bool is_trigger = (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
                                             e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
                    const int val = is_trigger ? e.caxis.value : std::abs(e.caxis.value);
                    if (val > 20000) {
                        pads.set(b, 2, e.caxis.axis, is_trigger ? 1 : (e.caxis.value < 0 ? -1 : 1));
                        capturing = false;
                    }
                }
                continue;
            }

            Input in = Input::None;
            if (e.type == SDL_KEYDOWN && !e.key.repeat) in = map_key(e.key.keysym.sym);
            else if (e.type == SDL_CONTROLLERBUTTONDOWN) in = map_button(e.cbutton.button);
            else if (e.type == SDL_CONTROLLERAXISMOTION) in = stick.input(e.caxis);
            if (in == Input::None) continue;
            using_pad = e.type != SDL_KEYDOWN;

            if (in == Input::PrevPage || in == Input::NextPage) {  // cycle Settings, Controls, Mods
                const int at = page == Page::Main ? (in == Input::NextPage ? 0 : 5) : static_cast<int>(page);
                const int next = (at - 1 + (in == Input::NextPage ? 1 : 3)) % 4 + 1;
                if (page != Page::Main) leave_page();
                open_page(static_cast<Page>(next));
                continue;
            }

            switch (page) {
                case Page::Main:
                    if (update_prompt) {
                        if (in == Input::Up || in == Input::Down) prompt_row = 1 - prompt_row;
                        if (in == Input::Back) update_prompt = false;
                        if (in == Input::Confirm) {
                            update_prompt = false;
                            if (prompt_row == 0) {
                                open_page(Page::Updates);
                                if (!upd_state.downloading) m3updater::begin_download_and_apply(dir);
                            } else {
                                result = Result::Play; running = false;
                            }
                        }
                        break;
                    }
                    if (in == Input::Left || in == Input::Up) cursor_at = (cursor_at + kItemCount - 1) % kItemCount;
                    if (in == Input::Right || in == Input::Down) cursor_at = (cursor_at + 1) % kItemCount;
                    if (in == Input::Back) cursor_at = kItemCount - 1;
                    if (in == Input::Confirm) {
                        if (cursor_at == 0) try_play();
                        else if (cursor_at == kItemCount - 1) running = false;
                        else open_page(static_cast<Page>(cursor_at));
                    }
                    break;
                case Page::Settings:
                    if (in == Input::Up) row = (row + kSettingRows - 1) % kSettingRows;
                    if (in == Input::Down) row = (row + 1) % kSettingRows;
                    if (in == Input::Left) change_setting(-1);
                    if (in == Input::Confirm && row == kSettingRows - 1) {
                        leave_page();
                        result = Result::ChangeFiles;
                        running = false;
                    } else if (in == Input::Confirm && row == kSettingRows - 2) {
                        m3setup::import_save(dir);
                    } else if (in == Input::Right || in == Input::Confirm) change_setting(1);
                    if (in == Input::Back) leave_page();
                    break;
                case Page::Controls:
                    if (in == Input::Up) row = (row + kControlRows - 1) % kControlRows;
                    if (in == Input::Down) row = (row + 1) % kControlRows;
                    if (in == Input::Back) leave_page();
                    else if (in == Input::Confirm && row == kControlRows - 1) {
                        st.keys = m3settings::defaults().keys;
                        pads.reset();
                    } else if (in == Input::Confirm) capturing = true;
                    break;
                case Page::Mods: {  // rows: each mod, then "Open mods folder"
                    const int rows = static_cast<int>(mods.size()) + 1;
                    if (in == Input::Up) row = (row + rows - 1) % rows;
                    if (in == Input::Down) row = (row + 1) % rows;
                    if (in == Input::Back) leave_page();
                    else if (in == Input::Confirm && row == rows - 1) {
                        const std::string url = "file:///" + m3mods::folder(dir);
                        SDL_OpenURL(url.c_str());
                    } else if (in == Input::Confirm && mods[row].valid) {
                        mods[row].enabled = !mods[row].enabled;
                    }
                    break;
                }
                case Page::Account: {
                    constexpr int kAccRows = 3;
                    if (in == Input::Up) row = (row + kAccRows - 1) % kAccRows;
                    if (in == Input::Down) row = (row + 1) % kAccRows;
                    if (in == Input::Back) {
                        m3discord::cancel_login();
                        leave_page();
                    } else if (in == Input::Confirm) {
                        if (row == 0) {
                            discord_cfg = m3discord::load_config(dir);
                            m3discord::begin_login(dir);
                        } else if (row == 1) {
                            discord_cfg = m3discord::load_config(dir);
                            m3discord::begin_refresh(dir);
                        } else if (row == 2) {
                            m3discord::cancel_login();
                            m3discord::clear_session(dir);
                            discord_session = m3discord::Session{};
                        }
                    }
                    break;
                }
                case Page::Updates: {
                    const int max_scroll = std::max(0, static_cast<int>(upd_state.patch_notes.size()) - 6);
                    if (in == Input::Up) {
                        if (row > 0) --row;
                        else notes_scroll = std::max(0, notes_scroll - 1);
                    }
                    if (in == Input::Down) {
                        if (row < 1) ++row;
                        else notes_scroll = std::min(max_scroll, notes_scroll + 1);
                    }
                    if (in == Input::Back) leave_page();
                    else if (in == Input::Confirm) {
                        if (row == 0) {
                            upd_cfg = m3updater::load_config(dir);
                            m3updater::begin_check(dir);
                        } else if (row == 1 && upd_state.update_available && !upd_state.downloading) {
                            m3updater::begin_download_and_apply(dir);
                        }
                    }
                    break;
                }
            }
        }
        m3discord::poll(dir, discord_session);
        m3updater::poll(upd_state);
        if (upd_state.ready_to_restart) { m3updater::launch_restart_script(dir); running = false; } if (built_flavor != st.flavor) build_windows();

        // The party walks 60 steps a second whatever the display's refresh rate:
        // vsync alone ran it 4x fast on a 240 Hz screen. A stall skips ahead at most 4.
        const Uint64 now = SDL_GetPerformanceCounter();
        ticks_due = std::min(ticks_due + double(now - last_tick) * 60.0 / double(SDL_GetPerformanceFrequency()), 4.0);
        last_tick = now;
        if (ticks_due < 1.0) SDL_Delay(1);  // in case vsync isn't on
        for (; ticks_due >= 1.0; ticks_due -= 1.0) {
            walker.tick();
            ++frame;
            for (const char* q = pad_script; q && *q;) {
                char* end;
                const long at = std::strtol(q, &end, 10), button = *end == ':' ? std::strtol(end + 1, &end, 10) : -1;
                if (at == frame && button >= 0) {
                    SDL_Event ev{};
                    ev.type = SDL_CONTROLLERBUTTONDOWN;
                    ev.cbutton.button = static_cast<Uint8>(button);
                    SDL_PushEvent(&ev);
                }
                q = *end == ',' ? end + 1 : nullptr;
            }
        }
        const int cam_x = std::clamp(kCamera.x, 0, scene.w - kW);
        const int cam_y = std::clamp(kCamera.y, 0, scene.h - kH);
        Canvas& cv = ui.cv;
        cv.blit_view(scene, cam_x, cam_y);

        // Back-to-front by feet position so nearer members overlap farther ones.
        std::vector<int> order(kPartySize);
        for (int k = 0; k < kPartySize; ++k) order[k] = kPartySize - 1 - k;
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            return walker.member(a).at.y < walker.member(b).at.y;
        });
        // One shared step clock keeps the party in step; each sprite's ROM walk cycle gives its frames.
        const int walk_step = frame / 11;
        for (int k : order) {
            const Step s = walker.member(k);
            const int f = kParty[k].stride ? s.dir + kParty[k].stride[walk_step % 4]
                                           : rom.walk_frame(kParty[k].sprite, s.dir, walk_step);
            cv.draw(rom.sprite_frame(kParty[k].sprite, f), s.at.x - cam_x - 32, s.at.y - cam_y - 48);
        }

        cv.draw(command_bar, 0, 0);
        for (int i = 0; i < kItemCount; ++i) {
            if (i == cursor_at && page == Page::Main && !hover_updates) ui.cursor(item_x[i] - 10, 11);
            ui.text(kItems[i], item_x[i], 7, i == cursor_at ? kInk : (page == Page::Main ? kInk : kDim));
        }

        constexpr int kPx = 8, kPy = 26, kLabelX = kPx + 18, kRight = kPx + 38 * 8 - 14, kHintY = kPy + 136;
        if (page == Page::Main) {
            const int by = kH - command_bar.h;
            cv.draw(command_bar, 0, by);
            const std::string upd_lbl = upd_state.update_available ? "Update!" : "Updates";
            if (hover_updates) ui.cursor(6, by + 11);
            ui.text(upd_lbl, 16, by + 7, upd_state.update_available ? kHot : kInk);
            const int ver_x = 16 + ui.width(upd_lbl) + 8;
            const std::string ver_str = std::string("v") + m3updater::kLauncherVersion + " (" + upd_cfg.build_version + ")";
            ui.text(ver_str, ver_x, by + 7, kDim);
            if (update_prompt) {
                const int ux = (kW - upd_box.w) / 2, uy = (kH - upd_box.h) / 2;
                cv.draw(upd_box, ux, uy);
                const std::string title = "Update available (v" + upd_state.latest_version + ")!";
                ui.text(title, ux + 12, uy + 8, kHot);
                const char* opts[2] = {"Update now", "Play without updating"};
                for (int i = 0; i < 2; ++i) {
                    const int ry = uy + 24 + i * 12;
                    if (i == prompt_row) ui.cursor(ux + 12, ry + 4);
                    ui.text((i == 0 ? "Update now" : "Play without updating"), ux + 22, ry, i == prompt_row ? kHot : kInk);
                }
            }
        } else if (page == Page::Settings) {
            cv.draw(page_win, kPx, kPy);
            const std::string values[kSettingRows] = {
                kFlavors[st.flavor], kDisplayModes[st.fullscreen], std::to_string(st.scale) + "x",
                st.sharp_filter ? "Sharp" : "Pixel", "", std::to_string(st.volume) + "%",
                kPromptStyles[st.prompt_style], st.widescreen ? "On (16:9)" : "Off",
                st.smooth == 2 ? "Smooth (always)" : !st.smooth ? "60"
                : display_hz(win) == 0 ? "Smooth"
                : display_hz(win) >= 100 ? std::to_string(display_hz(win)) + " (smooth)" : "Smooth (needs 100 Hz+)",
                st.hq_audio ? "On" : "Off", "", ""};
            const char* labels[kSettingRows] = {"Window flavor", "Display", "Window size", "Scaling",
                                                "Screen colors", "Volume", "Button prompts", "Widescreen",
                                                "Frame rate", "HQ audio", "Import a save file (.sav, .srm)",
                                                "Change game files (ROM, BIOS)"};
            for (int i = 0; i < kSettingRows; ++i) {
                const int y = kPy + 6 + i * 10;  // 12 rows: same pitch as Controls
                if (i == row) ui.cursor(kLabelX - 10, y + 4);
                ui.text(labels[i], kLabelX, y);
                std::string v = values[i];
                if (i == 4)
                    for (int s = 0; s < 5; ++s) if (st.screen == kScreens[s]) v = kScreenNames[s];
                if (!v.empty()) ui.choice(v, kRight, y, i == row);
            }
            ui.text(hint("Change"), kLabelX, kHintY, kDim);
        } else if (page == Page::Controls) {
            cv.draw(page_win, kPx, kPy);
            const int key_x = kPx + 100, pad_x = kPx + 200;
            ui.text("GBA", kLabelX, kPy + 4, kDim);
            ui.text("Keyboard", key_x, kPy + 4, kDim);
            ui.text(pads.available() ? "Controller" : "No controller", pad_x, kPy + 4, kDim);
            for (int i = 0; i < kControlRows; ++i) {
                const int y = kPy + 16 + i * 10;
                const std::uint32_t c = i == row ? kHot : kInk;
                if (i == row) ui.cursor(kLabelX - 10, y + 4);
                if (i == kControlRows - 1) { ui.text("Reset all to defaults", kLabelX, y, c); continue; }
                ui.text(m3settings::kButtonLabels[i], kLabelX, y, c);
                if (capturing && i == row) { ui.text("Press a key or button...", key_x, y, kHot); continue; }
                ui.text(st.keys[i], key_x, y, c);
                ui.text(pads.available() ? pads.label(i) : "-", pad_x, y, c);
            }
            // The runtime reads the left stick directly, so it isn't a bind.
            ui.text_right("Left stick always moves", kRight, kPy + 16 + (kControlRows - 1) * 10, kDim);
            ui.text(capturing ? std::string("Escape cancels") : hint("Rebind"), kLabelX, kHintY, kDim);
        } else if (page == Page::Updates) {
            cv.draw(page_win, kPx, kPy);
            ui.text("Launcher & Build Updates", kLabelX, kPy + 6, kHot);
            ui.text("Launcher: v" + std::string(m3updater::kLauncherVersion) + "   Build: " + upd_cfg.build_version,
                    kLabelX, kPy + 18, kInk);

            const char* act0 = upd_state.checking ? "Checking..." : "Check for updates";
            const std::string act1_str = upd_state.downloading ? "Downloading update..."
                : (upd_state.update_available ? ("Install update (v" + upd_state.latest_version + ")")
                                              : "Install update (none available)");
            if (row == 0) ui.cursor(kLabelX - 10, kPy + 39);
            ui.text(act0, kLabelX, kPy + 35, row == 0 ? kHot : kInk);
            if (row == 1) ui.cursor(kLabelX - 10, kPy + 51);
            ui.text(act1_str, kLabelX, kPy + 47,
                    row == 1 ? kHot : (upd_state.update_available ? kInk : kDim));

            ui.text("Patch Notes:", kLabelX, kPy + 62, kDim);
            constexpr int kNoteLines = 6;
            const int total_notes = static_cast<int>(upd_state.patch_notes.size());
            for (int i = 0; i < kNoteLines; ++i) {
                const int idx = notes_scroll + i;
                if (idx >= total_notes) break;
                ui.text(upd_state.patch_notes[idx], kLabelX, kPy + 73 + i * 9, kInk);
            }

            std::string st_msg = upd_state.status;
            while (ui.width(st_msg) > 265 && st_msg.size() > 4) st_msg.pop_back();
            ui.text(st_msg, kLabelX, kHintY - 11, kDim);
            ui.text(hint("Select"), kLabelX, kHintY, kDim);
        } else if (page == Page::Account) {
            cv.draw(page_win, kPx, kPy);
            ui.text("Discord Account & Playtest Access", kLabelX, kPy + 8, kHot);
            const std::string acct = discord_session.signed_in
                ? (discord_session.global_name + " (@" + discord_session.username + ")")
                : "Not linked";
            const std::string server_str = !discord_session.signed_in ? "-"
                : (discord_session.in_guild ? "Joined" : "Not in server");
            const std::string role_str = !discord_session.signed_in ? "Unverified"
                : (discord_session.is_playtester ? "Verified Playtester" : "Standard Member");
            ui.text("Account:", kLabelX, kPy + 24, kDim);
            ui.text(acct, kLabelX + 64, kPy + 24, discord_session.signed_in ? kInk : kDim);
            ui.text("Server:", kLabelX, kPy + 36, kDim);
            ui.text(server_str, kLabelX + 64, kPy + 36, discord_session.in_guild ? kInk : kDim);
            ui.text("Status:", kLabelX, kPy + 48, kDim);
            ui.text(role_str, kLabelX + 64, kPy + 48, discord_session.is_playtester ? kHot : kInk);
            std::vector<const char*> actions = {
                discord_session.signed_in ? "Re-link Discord account" : "Sign in with Discord",
                "Refresh playtester role",
                "Sign out / Unlink account"
            };
            for (int i = 0; i < 3; ++i) {
                const int y = kPy + 72 + i * 13;
                if (i == row) ui.cursor(kLabelX - 10, y + 4);
                ui.text(actions.at(i), kLabelX, y, i == row ? kHot : kInk);
            }
            std::string st_msg = m3discord::status_text();
            while (ui.width(st_msg) > 265 && st_msg.size() > 4) st_msg.pop_back();
            ui.text(st_msg, kLabelX, kHintY - 14, kDim);
            ui.text(hint("Select"), kLabelX, kHintY, kDim);
        } else {  // Mods
            cv.draw(page_win, kPx, kPy);
            constexpr int kVisible = 7;
            const int count = static_cast<int>(mods.size());
            if (row < count) mods_scroll = std::clamp(mods_scroll, row - kVisible + 1, row);
            if (count == 0) {
                ui.text("No mods yet. Put .ips, .bps or .ups", kLabelX, kPy + 12, kDim);
                ui.text("patches in the mods folder.", kLabelX, kPy + 24, kDim);
            }
            for (int v = 0; v < std::min(kVisible, count); ++v) {
                const int i = mods_scroll + v;
                if (i >= count) break;
                const int y = kPy + 10 + v * 13;
                const m3mods::Mod& m = mods[i];
                if (i == row) ui.cursor(kLabelX - 10, y + 4);
                cv.fill(kLabelX, y + 2, 8, 8, kInk);  // checkbox
                cv.fill(kLabelX + 1, y + 3, 6, 6, m.enabled ? kHot : 0xFFF4EEE8u);
                std::string name = m.file;
                while (ui.width(name) > 170 && name.size() > 4) name.pop_back();
                ui.text(name, kLabelX + 12, y, m.valid ? kInk : kDim);
                if (!m.valid) ui.text_right("doesn't fit this ROM", kRight, y, kDim);
                else if (m.changes_code) ui.text_right("slower", kRight, y, kDim);
            }
            const int y_open = kPy + 10 + kVisible * 13 + 4;
            if (row == count) ui.cursor(kLabelX - 10, y_open + 4);
            ui.text("Open mods folder", kLabelX, y_open);
            ui.text("Slower: changes game code, runs in compatibility mode.", kLabelX, kHintY - 12, kDim);
            ui.text(hint("Toggle"), kLabelX, kHintY, kDim);
        }

        SDL_UpdateTexture(tex, nullptr, cv.px.data(), kW * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, nullptr, nullptr);
        SDL_RenderPresent(ren);

        if (shot && frame >= shot_frame) {
            if (SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(cv.px.data(), kW, kH, 32, kW * 4,
                                                                    SDL_PIXELFORMAT_ARGB8888)) {
                SDL_SaveBMP(s, shot);
                SDL_FreeSurface(s);
            }
            result = Result::Quit;
            running = false;
        }
    }
    if (page != Page::Main && !shot) leave_page();  // closing the window keeps edits

    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
    return result;
}

}  // namespace m3launcher
