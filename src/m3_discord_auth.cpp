#include "m3_discord_auth.h"

#include <SDL.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shellapi.h>
#endif

namespace m3discord {
namespace {

using LockGuard = std::lock_guard<std::mutex>;

std::mutex g_mtx;
std::atomic<bool> g_busy{false};
std::atomic<bool> g_cancel{false};
std::atomic<bool> g_dirty{false};
std::string g_status = "Not signed in";
Session g_worker_session;

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s.at(i) == ' ' || s.at(i) == '\t')) ++i;
    return s.substr(i);
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::string base64url(const unsigned char* data, size_t len) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    size_t i = 0;
    while (i + 3 <= len) {
        unsigned int v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6) & 63]);
        out.push_back(tbl[v & 63]);
        i += 3;
    }
    if (i + 1 == len) {
        unsigned int v = data[i] << 16;
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
    } else if (i + 2 == len) {
        unsigned int v = (data[i] << 16) | (data[i + 1] << 8);
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6) & 63]);
    }
    return out;
}

std::string random_token(size_t bytes) {
    std::vector<unsigned char> buf(bytes);
    std::random_device rd;
    for (size_t i = 0; i < bytes; ++i) buf[i] = static_cast<unsigned char>(rd() & 0xFF);
    return base64url(buf.data(), buf.size());
}

std::string sha256_b64url(const std::string& input) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> digest(32, 0);
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
                           static_cast<ULONG>(input.size()), 0);
            BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return base64url(digest.data(), digest.size());
#else
    return input;
#endif
}

std::string json_get_string(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    ++pos;
    while (pos < json.size() && (json.at(pos) == ' ' || json.at(pos) == '\t' || json.at(pos) == '\n' || json.at(pos) == '\r'))
        ++pos;
    if (pos >= json.size() || json.at(pos) != '"') return {};
    ++pos;
    std::string out;
    while (pos < json.size() && json.at(pos) != '"') {
        if (json.at(pos) == '\\' && pos + 1 < json.size()) {
            ++pos;
            if (json.at(pos) == 'n') out.push_back('\n');
            else out.push_back(json.at(pos));
        } else {
            out.push_back(json.at(pos));
        }
        ++pos;
    }
    return out;
}

bool json_roles_contains(const std::string& json, const std::string& role_id) {
    if (role_id.empty()) return false;
    const size_t rpos = json.find("\"roles\"");
    if (rpos == std::string::npos) return false;
    const size_t open_b = json.find(char(91), rpos);
    const size_t close_b = json.find(char(93), open_b);
    if (open_b == std::string::npos || close_b == std::string::npos) return false;
    const std::string arr = json.substr(open_b, close_b - open_b + 1);
    return arr.find("\"" + role_id + "\"") != std::string::npos;
}

#ifdef _WIN32
struct HttpResp {
    int status = 0;
    std::string body;
};

HttpResp discord_http(const wchar_t* method, const std::wstring& path,
                      const std::string& body, const std::string& content_type,
                      const std::string& bearer_token) {
    HttpResp res;
    HINTERNET hSession = WinHttpOpen(L"Mother3RecompLauncher/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return res;
    HINTERNET hConnect = WinHttpConnect(hSession, L"discord.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return res;
    }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, method, path.c_str(),
                                            nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return res;
    }

    std::wstring headers;
    if (!content_type.empty()) {
        std::wstring wct(content_type.begin(), content_type.end());
        headers += L"Content-Type: " + wct + L"\r\n";
    }
    if (!bearer_token.empty()) {
        std::wstring wt(bearer_token.begin(), bearer_token.end());
        headers += L"Authorization: Bearer " + wt + L"\r\n";
    }
    if (!headers.empty()) {
        WinHttpAddRequestHeaders(hRequest, headers.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
    }

    BOOL ok = WinHttpSendRequest(
        hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()), 0);

    if (ok && WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD status_code = 0;
        DWORD sz = sizeof(status_code);
        WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &sz, WINHTTP_NO_HEADER_INDEX);
        res.status = static_cast<int>(status_code);

        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
            std::vector<char> chunk(avail);
            DWORD read = 0;
            if (WinHttpReadData(hRequest, chunk.data(), avail, &read) && read > 0) {
                res.body.append(chunk.data(), read);
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return res;
}
#endif

void set_status(const std::string& msg) {
    LockGuard lk(g_mtx);
    g_status = msg;
}

bool verify_user_and_role(const Config& cfg, Session& s) {
#ifdef _WIN32
    HttpResp me = discord_http(L"GET", L"/api/v10/users/@me", "", "", s.access_token);
    if (me.status != 200) {
        return false;
    }
    s.signed_in = true;
    s.user_id = json_get_string(me.body, "id");
    s.username = json_get_string(me.body, "username");
    s.global_name = json_get_string(me.body, "global_name");
    if (s.global_name.empty()) s.global_name = s.username;

    s.in_guild = false;
    s.is_playtester = false;
    if (!cfg.guild_id.empty()) {
        std::wstring gpath = L"/api/v10/users/@me/guilds/";
        gpath.append(cfg.guild_id.begin(), cfg.guild_id.end());
        gpath += L"/member";
        HttpResp gm = discord_http(L"GET", gpath, "", "", s.access_token);
        if (gm.status == 200) {
            s.in_guild = true;
            s.is_playtester = cfg.playtester_role_id.empty() ||
                              json_roles_contains(gm.body, cfg.playtester_role_id);
        }
    }
    return true;
#else
    (void)cfg;
    (void)s;
    return false;
#endif
}

struct WorkerArgs {
    std::string dir;
    Config cfg;
    std::string state;
    std::string verifier;
};

void refresh_worker(WorkerArgs args) {
    Config cfg = load_config(args.dir);
    Session s = load_session(args.dir);
    if (s.signed_in && !s.access_token.empty() && verify_user_and_role(cfg, s)) {
        set_status(s.is_playtester ? "Verified Playtester (@" + s.username + ")"
                                   : "Signed in (@" + s.username + ")");
        LockGuard lk(g_mtx);
        g_worker_session = s;
        g_dirty.store(true);
    } else {
        set_status("Session expired - please sign in again");
    }
    g_busy.store(false);
}

void login_worker(WorkerArgs args) {
#ifdef _WIN32
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) {
        set_status("Could not open local callback socket");
        g_busy.store(false);
        return;
    }

    int opt = 1;
setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
sockaddr_in addr{};
addr.sin_family = AF_INET;
addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
addr.sin_port = htons(48912);
if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(srv, 1) != 0) {
closesocket(srv);
set_status("Port 48912 busy - close other launcher");
g_busy.store(false);
return;
}
u_long nonblock = 1;
ioctlsocket(srv, FIONBIO, &nonblock);
std::string code;
std::string recv_state;
for (int tries = 0; tries < 1200 && !g_cancel.load(); ++tries) {
SOCKET cli = accept(srv, nullptr, nullptr);
if (cli == INVALID_SOCKET) {
Sleep(100);
continue;
}
char buf[4096] = {};
int n = recv(cli, buf, 4095, 0);
if (n > 0) {
std::string req(buf, n);
size_t q = req.find("GET /callback?");
if (q != std::string::npos) {
size_t end = req.find(' ', q + 14);
std::string query = req.substr(q + 14, end - (q + 14));
auto extract = [](const std::string& qry, const std::string& name) -> std::string {
std::string p = name + "=";
size_t pos = qry.find(p);
if (pos == std::string::npos) return {};
pos += p.size();
size_t amp = qry.find('&', pos);
return qry.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
};
code = extract(query, "code");
recv_state = extract(query, "state");
}
}
const char* html =
"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
""
"MOTHER 3 Recomp - Discord Linked!"
"You can close this browser tab and return to the launcher.";
send(cli, html, (int)std::strlen(html), 0);
closesocket(cli);
if (!code.empty()) break;
}
closesocket(srv);
if (code.empty() || recv_state != args.state) {
set_status(g_cancel.load() ? "Login cancelled" : "Discord sign-in timed out");
g_busy.store(false);
return;
}
set_status("Exchanging Discord token...");
const std::string form =
"client_id=" + url_encode(args.cfg.client_id) +
"&grant_type=authorization_code"
"&code=" + url_encode(code) +
"&redirect_uri=" + url_encode(args.cfg.redirect_uri) +
"&code_verifier=" + url_encode(args.verifier);
HttpResp tok = discord_http(L"POST", L"/api/v10/oauth2/token", form,
"application/x-www-form-urlencoded", "");
if (tok.status != 200) {
set_status("Discord token exchange failed (" + std::to_string(tok.status) + ")");
g_busy.store(false);
return;
}
Session s;
s.access_token = json_get_string(tok.body, "access_token");
s.refresh_token = json_get_string(tok.body, "refresh_token");
set_status("Checking Discord server & playtester role...");
if (!verify_user_and_role(args.cfg, s)) {
set_status("Failed to fetch Discord profile");
g_busy.store(false);
return;
}
set_status(s.is_playtester ? "Verified Playtester (@" + s.username + ")"
: "Signed in (@" + s.username + ")");
{
LockGuard lk(g_mtx);
g_worker_session = s;
g_dirty.store(true);
}
#else
(void)args;
set_status("Discord OAuth is currently Windows-only");
#endif
g_busy.store(false);
}
}  // namespace
Config load_config(const std::string& dir) {
Config cfg;
const std::string path = dir + "/discord_auth.ini";
std::ifstream in(path);
if (!in.is_open()) {
std::ofstream out(path);
out << "# Discord OAuth2 PKCE + Playtester Role Configuration\n"
    << "# Add http://127.0.0.1:48912/callback to your Discord App's OAuth2 Redirects.\n"
    << "client_id=1570322072373170177\n"
    << "guild_id=1570576347481251940\n"
    << "playtester_role_id=1570577186069422122\n"
    << "redirect_uri=http://127.0.0.1:48912/callback\n"
    << "require_playtester=0\n";
return cfg;
}
std::string line;
while (std::getline(in, line)) {
line = trim(line);
if (line.empty() || line.at(0) == '#' || line.at(0) == ';') continue;
const size_t eq = line.find('=');
if (eq == std::string::npos) continue;
const std::string k = trim(line.substr(0, eq));
const std::string v = trim(line.substr(eq + 1));
if (k == "client_id") cfg.client_id = v;
else if (k == "guild_id") cfg.guild_id = v;
else if (k == "playtester_role_id") cfg.playtester_role_id = v;
else if (k == "redirect_uri" && !v.empty()) cfg.redirect_uri = v;
else if (k == "require_playtester") cfg.require_playtester = (v == "1" || v == "true");
}
return cfg;
}
Session load_session(const std::string& dir) {
Session s;
std::ifstream in(dir + "/discord_session.ini");
if (!in.is_open()) return s;
std::string line;
while (std::getline(in, line)) {
line = trim(line);
const size_t eq = line.find('=');
if (eq == std::string::npos) continue;
const std::string k = trim(line.substr(0, eq));
const std::string v = trim(line.substr(eq + 1));
if (k == "signed_in") s.signed_in = (v == "1");
else if (k == "in_guild") s.in_guild = (v == "1");
else if (k == "is_playtester") s.is_playtester = (v == "1");
else if (k == "user_id") s.user_id = v;
else if (k == "username") s.username = v;
else if (k == "global_name") s.global_name = v;
else if (k == "access_token") s.access_token = v;
else if (k == "refresh_token") s.refresh_token = v;
}
if (s.signed_in && !s.username.empty()) {
set_status("Signed in as @" + s.username);
}
return s;
}
void save_session(const std::string& dir, const Session& s) {
std::ofstream out(dir + "/discord_session.ini");
out << "signed_in=" << (s.signed_in ? "1" : "0") << "\n"
<< "in_guild=" << (s.in_guild ? "1" : "0") << "\n"
<< "is_playtester=" << (s.is_playtester ? "1" : "0") << "\n"
<< "user_id=" << s.user_id << "\n"
<< "username=" << s.username << "\n"
<< "global_name=" << s.global_name << "\n"
<< "access_token=" << s.access_token << "\n"
<< "refresh_token=" << s.refresh_token << "\n";
}
void clear_session(const std::string& dir) {
std::remove((dir + "/discord_session.ini").c_str());
set_status("Not signed in");
}
void cancel_login() {
g_cancel.store(true);
}
bool is_busy() {
return g_busy.load();
}
std::string status_text() {
LockGuard lk(g_mtx);
return g_status;
}
bool poll(const std::string& dir, Session& out_session) {
if (!g_dirty.exchange(false)) return false;
{
LockGuard lk(g_mtx);
out_session = g_worker_session;
}
if (out_session.signed_in) save_session(dir, out_session);
return true;
}
void begin_refresh(const std::string& dir) {
if (g_busy.exchange(true)) return;
g_cancel.store(false);
set_status("Refreshing Discord role...");
WorkerArgs args{dir, Config{}, "", ""};
std::thread(refresh_worker, args).detach();
}
void begin_login(const std::string& dir) {
if (g_busy.exchange(true)) return;
g_cancel.store(false);
Config cfg = load_config(dir);
if (cfg.client_id.empty()) {
set_status("Set client_id in discord_auth.ini first!");
g_busy.store(false);
return;
}
const std::string state = random_token(16);
const std::string verifier = random_token(32);
const std::string challenge = sha256_b64url(verifier);
const std::string auth_url =
        std::string("https://") + "discord.com" + "/oauth2/authorize?response_type=code"
        "&client_id=" + url_encode(cfg.client_id) +
        "&scope=" + url_encode("identify guilds.members.read") +
        "&state=" + url_encode(state) +
        "&redirect_uri=" + url_encode(cfg.redirect_uri) +
        "&code_challenge=" + url_encode(challenge) +
        "&code_challenge_method=S256";

    WorkerArgs args{dir, cfg, state, verifier};
    std::thread(login_worker, args).detach();
    set_status("Opened browser (or Ctrl+V URL)...");
    SDL_SetClipboardText(auth_url.c_str());
#ifdef _WIN32
    std::wstring wurl(auth_url.begin(), auth_url.end());
    HINSTANCE res = ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(res) <= 32) {
        const wchar_t* browsers[] = {L"msedge.exe", L"chrome.exe", L"firefox.exe", L"brave.exe"};
        bool opened = false;
        for (const wchar_t* b : browsers) {
            std::wstring qurl = L"\"" + wurl + L"\"";
            HINSTANCE bres = ShellExecuteW(nullptr, L"open", b, wurl.c_str(), nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(bres) > 32) {
                opened = true;
                break;
            }
        }
        if (!opened) {
            SDL_OpenURL(auth_url.c_str());
        }
    }
#else
    SDL_OpenURL(auth_url.c_str());
#endif
}
}  // namespace m3discord
