#include "m3_updater.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace m3updater {
namespace {

using MutexLock = std::lock_guard<std::mutex>;
using AtomicBool = std::atomic<bool>;
using IntVec = std::vector<int>;

std::mutex g_mtx;
State g_shared;
AtomicBool g_busy{false};

std::string trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s.at(a) == ' ' || s.at(a) == '\t' || s.at(a) == '\r' || s.at(a) == '\n')) ++a;
    size_t b = s.size();
    while (b > a && (s.at(b - 1) == ' ' || s.at(b - 1) == '\t' || s.at(b - 1) == '\r' || s.at(b - 1) == '\n')) --b;
    return s.substr(a, b - a);
}

StrVec wrap_lines(const std::string& text, size_t max_chars = 44) {
    StrVec out;
    std::istringstream iss(text);
    std::string raw;
    while (std::getline(iss, raw)) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        if (raw.empty()) {
            out.push_back("");
            continue;
        }
        std::string cur;
        std::istringstream words(raw);
        std::string w;
        while (words >> w) {
            if (cur.empty()) {
                cur = w;
            } else if (cur.size() + 1 + w.size() <= max_chars) {
                cur += " " + w;
            } else {
                out.push_back(cur);
                cur = w;
            }
        }
        if (!cur.empty()) out.push_back(cur);
    }
    return out;
}

bool is_newer_version(const std::string& remote, const std::string& local) {
    auto parse = [](const std::string& v) {
        IntVec nums;
        std::string cur;
        for (char c : v) {
            if (c >= '0' && c <= '9') cur.push_back(c);
            else if (!cur.empty()) { nums.push_back(std::atoi(cur.c_str())); cur.clear(); }
        }
        if (!cur.empty()) nums.push_back(std::atoi(cur.c_str()));
        while (nums.size() < 3) nums.push_back(0);
        return nums;
    };
    const IntVec r = parse(remote);
    const IntVec l = parse(local);
    for (size_t i = 0; i < 3; ++i) {
        if (r.at(i) != l.at(i)) return r.at(i) > l.at(i);
    }
    return false;
}

#ifdef _WIN32
bool http_get_bytes(const std::string& url, std::string& body_out, std::string& err_out) {
    std::wstring wurl(url.begin(), url.end());
    std::wstring host_buf(256, L'\0');
    std::wstring path_buf(2048, L'\0');
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host_buf.data();
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path_buf.data();
    uc.dwUrlPathLength = 2048;

    if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc)) {
        err_out = "Invalid manifest URL";
        return false;
    }

    HINTERNET hSession = WinHttpOpen(L"Mother3Recomp-Updater/2.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { err_out = "WinHttpOpen failed"; return false; }

    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy, sizeof(redirect_policy));

    HINTERNET hConnect = WinHttpConnect(hSession, host_buf.c_str(), uc.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        err_out = "Connection failed";
        return false;
    }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path_buf.c_str(), nullptr,
                                            WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        err_out = "OpenRequest failed";
        return false;
    }

    bool ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(hRequest, nullptr);
    if (!ok) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        err_out = "Could not reach update server";
        return false;
    }

    DWORD status_code = 0, sc_size = sizeof(status_code);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &sc_size, WINHTTP_NO_HEADER_INDEX);
    if (status_code < 200 || status_code >= 300) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        err_out = "HTTP " + std::to_string(status_code);
        return false;
    }

    body_out.clear();
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (WinHttpReadData(hRequest, chunk.data(), avail, &read) && read > 0) {
            body_out.append(chunk.data(), read);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return true;
}
#endif

}  // namespace

Config load_config(const std::string& dir) {
    Config cfg;
    const std::string path = dir + "/update.ini";
    std::ifstream in(path);
    if (!in.is_open()) {
        std::ofstream out(path);
        out << "# MOTHER 3 Recomp Launcher Update & Build Config\n"
            << "# Set manifest_url to a raw INI file containing:\n"
            << "#   latest_version=2.0.1\n"
            << "#   download_url=https://.../Mother3Recomp.exe\n"
            << "#   notes=Line 1|Line 2|Line 3\n"
            << "manifest_url=\n"
            << "channel=stable\n"
            << "build_version=" << kDefaultBuildVersion << "\n";
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
        if (k == "manifest_url") cfg.manifest_url = v;
        else if (k == "channel" && !v.empty()) cfg.channel = v;
        else if (k == "build_version" && !v.empty()) cfg.build_version = v;
    }
    return cfg;
}

StrVec load_local_notes(const std::string& dir) {
    std::ifstream in(dir + "/patch_notes.txt");
    if (in.is_open()) {
        std::stringstream ss;
        ss << in.rdbuf();
        StrVec lines = wrap_lines(ss.str());
        if (!lines.empty()) return lines;
    }
    return {
        "v2.0.0 - Current Release",
        "- Added Discord OAuth2 Playtester Auth",
        "- Added full Mouse support in Launcher",
        "- Added HQ Audio Mixer & Widescreen",
        "- Added Smooth Frame Rate & Mods menu",
        "",
        "Tip: Add patch_notes.txt or set",
        "manifest_url in update.ini for live",
        "over-the-air launcher updates."
    };
}

void begin_check(const std::string& dir) {
    if (g_busy.exchange(true)) return;
    Config cfg = load_config(dir);
    {
        MutexLock lk(g_mtx);
        g_shared.checking = true;
        g_shared.status = "Checking for updates...";
    }
    std::thread worker([dir, cfg]() {
        State next;
        next.patch_notes = load_local_notes(dir);
        if (cfg.manifest_url.empty()) {
            next.status = "Set manifest_url in update.ini";
        } else {
#ifdef _WIN32
            std::string body, err;
            if (!http_get_bytes(cfg.manifest_url, body, err)) {
                next.status = "Check failed: " + err;
            } else {
                std::istringstream iss(body);
                std::string line, raw_notes;
                while (std::getline(iss, line)) {
                    line = trim(line);
                    const size_t eq = line.find('=');
                    if (eq == std::string::npos) continue;
                    const std::string k = trim(line.substr(0, eq));
                    const std::string v = trim(line.substr(eq + 1));
                    const bool beta = (cfg.channel == "beta");
                    if (!beta && k == "latest_version") next.latest_version = v;
                    else if (!beta && k == "download_url") next.download_url = v;
                    else if (!beta && k == "notes") raw_notes = v;
                    else if (beta && k == "beta_version") next.latest_version = v;
                    else if (beta && k == "beta_download_url") next.download_url = v;
                    else if (beta && k == "beta_notes") raw_notes = v;
                }
                if (!raw_notes.empty()) {
                    for (char& ch : raw_notes) if (ch == '|') ch = '\n';
                    next.patch_notes = wrap_lines(raw_notes);
                }
if (!next.latest_version.empty() && is_newer_version(next.latest_version, kLauncherVersion)) {
next.update_available = true;
next.status = "Update available: v" + next.latest_version;
} else {
next.status = "Up to date (v" + std::string(kLauncherVersion) + ")";
}
}
#else
next.status = "Updater is Windows-only";
#endif
}
{
MutexLock lk(g_mtx);
g_shared = next;
}
g_busy.store(false);
});
worker.detach();
}
void download_worker_fn(std::string dir, std::string url) {
#ifdef _WIN32
    std::string bin, err;
    const bool fetched = http_get_bytes(url, bin, err);
    const bool valid_exe = (fetched && bin.size() >= 500000 && bin.at(0) == 0x4D && bin.at(1) == 0x5A);
    if (!valid_exe) {
        MutexLock lk(g_mtx);
        g_shared.downloading = false;
        g_shared.status = "Download failed: " + (err.empty() ? "no valid .exe at URL" : err);
        g_busy.store(false);
        return;
    }
    const std::string new_exe = dir + "/Mother3Recomp.new.exe";
    std::ofstream out(new_exe, std::ios::binary);
    out.write(bin.data(), static_cast<std::streamsize>(bin.size()));
    out.close();

    MutexLock lk(g_mtx);
    g_shared.downloading = false;
    g_shared.ready_to_restart = true;
    g_shared.status = "Update downloaded! Restarting...";
#else
    (void)dir; (void)url;
#endif
    g_busy.store(false);
}

void begin_download_and_apply(const std::string& dir) {
    if (g_busy.exchange(true)) return;
    std::string url;
    {
        MutexLock lk(g_mtx);
        url = g_shared.download_url;
        if (url.empty()) {
            g_shared.status = "No download URL in manifest";
            g_busy.store(false);
            return;
        }
        g_shared.downloading = true;
        g_shared.status = "Downloading update...";
    }
    std::thread worker(download_worker_fn, dir, url);
    worker.detach();
}

void poll(State& st) {
MutexLock lk(g_mtx);
if (g_shared.patch_notes.empty() && !st.patch_notes.empty()) {
g_shared.patch_notes = st.patch_notes;
}
st = g_shared;
}
void launch_restart_script(const std::string& dir) {
#ifdef _WIN32
    const std::string q(1, char(34));
    const std::string bs(1, char(92));
    const std::string crlf = std::string(1, char(13)) + std::string(1, char(10));
    const std::string bat_path = dir + "/_apply_update.bat";
    std::ofstream bat(bat_path, std::ios::binary);
    bat << "@echo off" << crlf
        << "timeout /t 1 /nobreak >nul" << crlf
        << "move /y " << q << dir << bs << "Mother3Recomp.new.exe" << q << " " << q << dir << bs << "Mother3Recomp.exe" << q << " >nul" << crlf
        << "start " << q << q << " " << q << dir << bs << "Mother3Recomp.exe" << q << crlf
        << "del " << q << "%~f0" << q << crlf;
    bat.close();

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION proc_info{};
    std::string cmd = "cmd.exe /c " + q + bat_path + q;
    if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &proc_info)) {
        CloseHandle(proc_info.hProcess);
        CloseHandle(proc_info.hThread);
    }
#else
    (void)dir;
#endif
}

}  // namespace m3updater
