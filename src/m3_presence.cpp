#include "m3_presence.h"

#define SDL_MAIN_HANDLED
#include "SDL.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include "windows.h"
#endif

#include "atomic"
#include "cstdio"
#include "cstdint"
#include "cstring"
#include "ctime"
#include "fstream"
#include "string"

#include "gba_bus.h"
#include "host_overlay.h"
#include "runtime_bus_bridge.h"

namespace m3presence {
namespace {

constexpr std::uint32_t kMapIdAddr = 0x020281E4u;
constexpr std::uint32_t kGameAddr  = 0x02005C00u;

constexpr const char* kDefaultDiscordClientId = "1305948271645098014";

std::atomic_bool g_enabled{true};
int g_last_map = -1;
int g_last_chapter = -1;
bool g_last_enabled = false;
std::int64_t g_start_epoch = 0;

std::string load_discord_client_id() {
    std::string cfg_path = "config.ini";
    if (char* base = SDL_GetBasePath()) {
        cfg_path = std::string(base) + "config.ini";
        SDL_free(base);
    }
    std::ifstream f(cfg_path);
    if (!f.is_open()) f.open("config.ini");

    std::string line;
    while (std::getline(f, line)) {
        const auto pos = line.find("discord_client_id");
        if (pos == std::string::npos) continue;
        const auto eq = line.find('=', pos);
        if (eq == std::string::npos) continue;
        std::string val = line.substr(eq + 1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t' || val.front() == '"')) val.erase(val.begin());
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\r' || val.back() == '\n' || val.back() == '"')) val.pop_back();
        if (!val.empty()) return val;
    }
    return kDefaultDiscordClientId;
}

#if defined(_WIN32)
HANDLE g_pipe = INVALID_HANDLE_VALUE;
bool g_discord_handshaken = false;
bool g_discord_ready = false;
bool g_sent_initial_activity = false;
Uint32 g_next_connect_ms = 0;
int g_nonce = 1;

void discord_close() {
    if (g_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
    g_discord_handshaken = false;
    g_discord_ready = false;
    g_sent_initial_activity = false;
}

bool discord_write_frame(std::uint32_t opcode, const char* json, std::uint32_t len) {
    if (g_pipe == INVALID_HANDLE_VALUE) return false;
    std::uint32_t hdr_op = opcode;
    std::uint32_t hdr_len = len;
    DWORD written = 0;
    if (!WriteFile(g_pipe, &hdr_op, 4, &written, nullptr) || written != 4) {
        discord_close();
        return false;
    }
    if (!WriteFile(g_pipe, &hdr_len, 4, &written, nullptr) || written != 4) {
        discord_close();
        return false;
    }
    if (len > 0 && (!WriteFile(g_pipe, json, len, &written, nullptr) || written != len)) {
        discord_close();
        return false;
    }
    FlushFileBuffers(g_pipe);
    return true;
}

void discord_drain_incoming() {
    if (g_pipe == INVALID_HANDLE_VALUE) return;
    DWORD avail = 0;
    while (PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &avail, nullptr) && avail >= 8) {
        std::uint32_t op = 0, len = 0;
        DWORD read_bytes = 0;
        if (!ReadFile(g_pipe, &op, 4, &read_bytes, nullptr) || read_bytes != 4) {
            discord_close();
            return;
        }
        if (!ReadFile(g_pipe, &len, 4, &read_bytes, nullptr) || read_bytes != 4) {
            discord_close();
            return;
        }
        if (len > 65536u) {
            discord_close();
            return;
        }
        std::string msg(len, '\0');
        if (len > 0 && (!ReadFile(g_pipe, &msg.front(), len, &read_bytes, nullptr) || read_bytes != len)) {
            discord_close();
            return;
        }
        if (op == 2u) {
            std::fprintf(stderr, "m3-discord: closed by Discord (%s)\n", msg.c_str());
            discord_close();
            return;
        }
        if (msg.find("\"READY\"") != std::string::npos) {
            g_discord_ready = true;
            std::fprintf(stderr, "m3-discord: connected and READY!\n");
        } else if (msg.find("\"ERROR\"") != std::string::npos) {
            std::fprintf(stderr, "m3-discord: error response: %s\n", msg.c_str());
        }
    }
}

bool discord_ensure_connected() {
    discord_drain_incoming();
    if (g_pipe != INVALID_HANDLE_VALUE && g_discord_handshaken) return g_discord_ready;

    const Uint32 now = SDL_GetTicks();
    if (now < g_next_connect_ms) return false;
    g_next_connect_ms = now + 4000u;

    const std::string client_id = load_discord_client_id();
    if (client_id.empty()) return false;

    for (int i = 0; i < 10; ++i) {
        std::string pipe_name(64, '\0');
        std::snprintf(&pipe_name.front(), pipe_name.size(), "\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileA(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            g_pipe = h;
            break;
        }
    }
    if (g_pipe == INVALID_HANDLE_VALUE) return false;

    std::string handshake(256, '\0');
    const int n = std::snprintf(&handshake.front(), handshake.size(),
                                "{\"v\":1,\"client_id\":\"%s\"}", client_id.c_str());
    if (n <= 0 || !discord_write_frame(0u, handshake.c_str(), std::uint32_t(n))) {
        discord_close();
        return false;
    }
    g_discord_handshaken = true;
    g_discord_ready = false;
    g_sent_initial_activity = false;
    std::fprintf(stderr, "m3-discord: sent handshake for client_id=%s\n", client_id.c_str());
    return false;
}

std::string json_escape(const std::string& src) {
    std::string out;
    out.reserve(src.size() + 8);
    for (unsigned char c : src) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(char(c));
        } else if (c >= 0x20u) {
            out.push_back(char(c));
        }
    }
    return out;
}

void discord_update_activity(const std::string& details, const std::string& state) {
    if (!discord_ensure_connected()) return;

    const std::string esc_details = json_escape(details);
    const std::string esc_state = json_escape(state);

    const DWORD pid = GetCurrentProcessId();
    std::string payload(1024, '\0');
    const int n = std::snprintf(
        &payload.front(), payload.size(),
        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":{"
        "\"details\":\"%s\","
        "\"state\":\"%s\","
        "\"timestamps\":{\"start\":%lld}"
        "}},\"nonce\":\"m3-%d\"}",
        static_cast<unsigned long>(pid),
        esc_details.c_str(),
        esc_state.c_str(),
        static_cast<long long>(g_start_epoch),
        g_nonce++);

    if (n > 0 && discord_write_frame(1u, payload.c_str(), std::uint32_t(n))) {
        g_sent_initial_activity = true;
    }
}
#else
void discord_close() {}
bool discord_ensure_connected() { return false; }
void discord_update_activity(const std::string&, const std::string&) {}
#endif

std::uint8_t ew8(const std::uint8_t* ew, std::uint32_t addr) {
    return *(ew + ((addr - 0x02000000u) & 0x3FFFFu));
}

std::uint16_t ew16(const std::uint8_t* ew, std::uint32_t addr) {
    const std::uint32_t o = (addr - 0x02000000u) & 0x3FFFFu;
    return std::uint16_t(*(ew + o) | (*(ew + o + 1) << 8));
}

const char* chapter_name(int ch) {
    switch (ch) {
        case 0: return "Prologue";
        case 1: return "Chapter 1";
        case 2: return "Chapter 2";
        case 3: return "Chapter 3";
        case 4: return "Chapter 4";
        case 5: return "Chapter 5";
        case 6: return "Chapter 6";
        case 7: return "Chapter 7";
        case 8: return "Chapter 8";
        default: return nullptr;
    }
}

const char* room_area_name(int map_id) {
    if (map_id == 0) return "Title Screen";
    if (map_id >= 1 && map_id <= 12) return "Alec's House & Mt. Oriander";
    if (map_id >= 13 && map_id <= 28) return "Sunshine Forest";
    if (map_id == 36) return "Tazmily Crossroads";
    if (map_id >= 29 && map_id <= 68) return "Tazmily Village";
    if (map_id >= 69 && map_id <= 85) return "Yado Inn & Town Square";
    if (map_id >= 86 && map_id <= 115) return "Dragos' Plateau & Forest";
    if (map_id >= 116 && map_id <= 145) return "Tazmily Graveyard & Tunnel";
    if (map_id >= 146 && map_id <= 215) return "Osohe Castle";
    if (map_id >= 216 && map_id <= 245) return "Death Desert";
    if (map_id >= 246 && map_id <= 290) return "Railroad & Factory";
    if (map_id >= 291 && map_id <= 345) return "Club Titiboo & Attic";
    if (map_id >= 346 && map_id <= 395) return "Murky Highway";
    if (map_id >= 396 && map_id <= 445) return "Thunder Tower";
    if (map_id >= 446 && map_id <= 485) return "Sunflower Field";
    if (map_id >= 486 && map_id <= 545) return "Chimera Laboratory";
    if (map_id >= 546 && map_id <= 610) return "Snowcap Mountain & Saturn Valley";
    if (map_id >= 611 && map_id <= 675) return "Fire Mountain & Tanetane Island";
    if (map_id >= 676 && map_id <= 740) return "Chupichupyoi Temple & Sea Floor";
    if (map_id >= 741 && map_id <= 840) return "New Pork City";
    if (map_id >= 841 && map_id <= 940) return "Empire Porky Building";
    if (map_id >= 941 && map_id <= 999) return "Underground & Final Needle";
    return nullptr;
}

SDL_Window* find_game_window() {
    if (SDL_Window* w = SDL_GetKeyboardFocus()) return w;
    if (SDL_Window* w = SDL_GetMouseFocus()) return w;
    for (Uint32 id = 1; id <= 128; ++id) {
        if (SDL_Window* w = SDL_GetWindowFromID(id)) return w;
    }
    return nullptr;
}

}  // namespace

bool enabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

void set_enabled(bool on) {
    g_enabled.store(on, std::memory_order_relaxed);
    if (!on) {
        if (SDL_Window* win = find_game_window()) {
            SDL_SetWindowTitle(win, "MOTHER 3");
        }
        discord_close();
        g_last_map = -1;
        g_last_chapter = -1;
        g_last_enabled = false;
    }
}

void tick() {
const bool on = g_enabled.load(std::memory_order_relaxed);
if (!on) {
g_last_enabled = false;
return;
}
if (g_start_epoch == 0) {
g_start_epoch = std::int64_t(std::time(nullptr));
}
gba::GbaBus* bus = gbarecomp::active_bus();
if (!bus) return;
const std::uint8_t* ew = bus->ewram_ptr();
if (!ew) return;
const int map_id = int(ew16(ew, kMapIdAddr));
const int ch_raw = int(ew8(ew, kGameAddr + 0x0Cu));
const int chapter = (ch_raw >= 0 && ch_raw <= 8) ? ch_raw : -1;
SDL_Window* win = find_game_window();
if (!win) return;
const bool title_dirty = (on != g_last_enabled || map_id != g_last_map || chapter != g_last_chapter);
#if defined(_WIN32)
const bool discord_ready_now = discord_ensure_connected();
const bool discord_dirty = discord_ready_now && (title_dirty || !g_sent_initial_activity);
#else
const bool discord_dirty = false;
#endif
if (!title_dirty && !discord_dirty) return;
g_last_enabled = on;
g_last_map = map_id;
g_last_chapter = chapter;
std::string title(256, '\0');
std::string discord_details(128, '\0');
std::string discord_state(128, '\0');
const char* ch_str = chapter_name(chapter);
const char* area_str = room_area_name(map_id);
if (map_id <= 0) {
std::snprintf(&title.front(), title.size(), "MOTHER 3 — Title Screen");
std::snprintf(&discord_details.front(), discord_details.size(), "Title Screen");
std::snprintf(&discord_state.front(), discord_state.size(), "Selecting a Save File");
} else if (ch_str && area_str) {
std::snprintf(&title.front(), title.size(), "MOTHER 3 — %s | %s (Room %d)", ch_str, area_str, map_id);
std::snprintf(&discord_details.front(), discord_details.size(), "%s", area_str);
std::snprintf(&discord_state.front(), discord_state.size(), "%s (Room %d)", ch_str, map_id);
} else if (area_str) {
std::snprintf(&title.front(), title.size(), "MOTHER 3 — %s (Room %d)", area_str, map_id);
std::snprintf(&discord_details.front(), discord_details.size(), "%s", area_str);
std::snprintf(&discord_state.front(), discord_state.size(), "Room %d", map_id);
} else {
std::snprintf(&title.front(), title.size(), "MOTHER 3 — Room %d", map_id);
std::snprintf(&discord_details.front(), discord_details.size(), "Exploring Room %d", map_id);
std::snprintf(&discord_state.front(), discord_state.size(), "MOTHER 3");
}
if (title_dirty) {
SDL_SetWindowTitle(win, title.c_str());
}
if (discord_dirty) {
discord_update_activity(discord_details.c_str(), discord_state.c_str());
}
}
void render_overlay(gbarecomp::HostOverlay*) {
}
}  // namespace m3presence
