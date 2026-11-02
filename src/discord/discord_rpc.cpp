/*
 * Discord Rich Presence (Windows) for Tools of Destruction Recompiled.
 *
 * If the Discord app is running and signed in, the player's profile shows
 * "Playing Tools of Destruction Recompiled", the current level, a short state and
 * the time played, with a "GitHub" button. It is always on: players who do not want it
 * switch off "share my activity" in Discord itself. Nothing is configured in the game.
 *
 * How it talks to Discord: the local IPC named pipe of the Discord client
 * (\\.\pipe\discord-ipc-0..9), the same channel Discord's own SDKs use: a handshake
 * frame (opcode 0, {"v":1,"client_id":...}) and then SET_ACTIVITY frames (opcode 1,
 * JSON). No SDK DLL; nothing leaves this PC except through the Discord app.
 *
 * It must never hurt the game: everything below runs on one low-priority background
 * thread that never touches a game, render or audio lock. All pipe I/O is overlapped
 * with a short timeout. When Discord is not running (or closes mid-session) the thread
 * retries every ~20 s without a word in the log; the activity is sent at most once every
 * 15 s and only when something changed.
 *
 * The connection part (pipe framing, handshake, SET_ACTIVITY layout) is adapted from
 * RACiT's game/discord_rpc.cpp (https://github.com/HighDrexler/RACiT), MIT licence:
 *
 *   Copyright (c) 2026 HighDrexler
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 *
 * Debug-only environment (not for players): TOD_DISCORD=0 turns the feature off,
 * TOD_DISCORD_APP_ID=<digits> uses another application, TOD_DISCORD_LOG=1 prints Discord's replies, TOD_DISCORD_PIPE=<prefix> uses
 * another pipe name prefix (default \\.\pipe\discord-ipc-), for a mock server.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include "discord_config.h"
#include "discord_level.h"

#ifdef _WIN32
#include <windows.h>

namespace {

const char* g_app_id = TOD_DISCORD_APP_ID;
const char* g_pipe_prefix = "\\\\.\\pipe\\discord-ipc-";
HANDLE g_pipe = INVALID_HANDLE_VALUE;
bool g_verbose;                      /* TOD_DISCORD_LOG=1: print every reply (debug) */
HANDLE g_ev = nullptr;               /* manual-reset event for the overlapped pipe I/O */
unsigned g_nonce;

#ifndef TOD_DISCORD_TEST   /* tools/discord_mock_test.cpp shortens the timings */
constexpr DWORD kIoTimeoutMs = 1500;       /* any single pipe read / write / handshake step */
constexpr ULONGLONG kRetryMs = 20000;      /* Discord not running: look again this often (silently) */
constexpr ULONGLONG kMinUpdateMs = 15000;  /* Discord rate-limits presence; never send more often */
constexpr DWORD kLoopMs = 1000;           /* how often the thread looks at the pipe and the game */
constexpr ULONGLONG kFirstTryMs = 5000;    /* let the game get going first */
#else
constexpr DWORD kIoTimeoutMs = 700;
constexpr ULONGLONG kRetryMs = 1500;
constexpr ULONGLONG kMinUpdateMs = 2000;
constexpr DWORD kLoopMs = 200;
constexpr ULONGLONG kFirstTryMs = 300;
#endif

void json_str(std::string& out, const char* s)
{
    out += '"';
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c >= 0x20) out += (char)c;
    }
    out += '"';
}

/* Wait for the overlapped operation `o` (started on g_pipe); false on timeout / failure. */
bool io_wait(OVERLAPPED& o, BOOL started, DWORD want, DWORD* got)
{
    if (!started) {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        if (WaitForSingleObject(g_ev, kIoTimeoutMs) != WAIT_OBJECT_0) {
            CancelIoEx(g_pipe, &o);
            DWORD n; GetOverlappedResult(g_pipe, &o, &n, TRUE);   /* the cancel completes at once */
            return false;
        }
    }
    DWORD n = 0;
    if (!GetOverlappedResult(g_pipe, &o, &n, FALSE)) return false;
    if (got) *got = n;
    return want == 0 || n == want;
}

bool pipe_write_all(const void* data, DWORD len)
{
    OVERLAPPED o = {}; o.hEvent = g_ev; ResetEvent(g_ev);
    DWORD n = 0;
    const BOOL ok = WriteFile(g_pipe, data, len, &n, &o);
    return io_wait(o, ok, len, &n);
}

bool pipe_read_all(void* data, DWORD len)
{
    OVERLAPPED o = {}; o.hEvent = g_ev; ResetEvent(g_ev);
    DWORD n = 0;
    const BOOL ok = ReadFile(g_pipe, data, len, &n, &o);
    return io_wait(o, ok, len, &n);
}

bool pipe_write(uint32_t op, const std::string& json)
{
    std::string frame(8 + json.size(), '\0');
    const uint32_t len = (uint32_t)json.size();
    memcpy(&frame[0], &op, 4);          /* little-endian header: opcode, length */
    memcpy(&frame[4], &len, 4);
    memcpy(&frame[8], json.data(), json.size());
    return pipe_write_all(frame.data(), (DWORD)frame.size());
}

/* One whole frame; the caller knows a header is already waiting (or accepts the timeout). */
bool pipe_read(uint32_t* op, std::string& out)
{
    uint32_t hdr[2];
    if (!pipe_read_all(hdr, 8) || hdr[1] > (1u << 20)) return false;
    out.assign(hdr[1], '\0');
    if (hdr[1] && !pipe_read_all(&out[0], hdr[1])) return false;
    if (op) *op = hdr[0];
    return true;
}

void disconnect()
{
    if (g_pipe != INVALID_HANDLE_VALUE) CloseHandle(g_pipe);
    g_pipe = INVALID_HANDLE_VALUE;
}

/* Read what Discord sent without waiting for more. false once the pipe is gone (Discord
 * closed, or it asked us to close). Pings are answered. */
bool pipe_service()
{
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &avail, nullptr)) return false;
        if (avail < 8) return true;
        uint32_t op = 0; std::string body;
        if (!pipe_read(&op, body)) return false;
        if (op == 2) return false;                       /* CLOSE */
        if (op == 1) {                                   /* a reply: only an error is worth a line */
            static int told;
            const bool err = body.find("\"evt\":\"ERROR\"") != std::string::npos;
            if ((g_verbose || err) && told < (g_verbose ? 100 : 3)) {   /* an error is logged at most 3 times, whatever Discord says */
                told++;
                fprintf(stderr, "[discord] reply: %.700s\n", body.c_str());
            }
        }
        if (op == 3 && !pipe_write(4, body)) return false;   /* PING -> PONG */
    }
}

bool connect()
{
    for (int i = 0; i < 10; i++) {
        char name[96];
        snprintf(name, sizeof name, "%s%d", g_pipe_prefix, i);
        HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;         /* no such pipe / busy: try the next */
        g_pipe = h;
        std::string hello = "{\"v\":1,\"client_id\":";
        json_str(hello, g_app_id);
        hello += '}';
        uint32_t op = 0; std::string reply;
        if (pipe_write(0, hello) && pipe_read(&op, reply) && op == 1 &&
            reply.find("\"READY\"") != std::string::npos)
            return true;
        disconnect();
    }
    return false;
}

bool repo_url_ok()
{
    return !strstr(TOD_REPO_URL, "{{") && !strncmp(TOD_REPO_URL, "https://", 8);
}

bool set_activity(const TodPresence& p, long long start)
{
    std::string j = "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"" + std::to_string(++g_nonce) +
                    "\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId()) + ",\"activity\":{";
    if (p.details) { j += "\"details\":"; json_str(j, p.details); j += ','; }
    if (p.state)   { j += "\"state\":";   json_str(j, p.state);   j += ','; }
    j += "\"timestamps\":{\"start\":" + std::to_string(start) + "},";
    j += "\"assets\":{\"large_image\":\"icon\",\"large_text\":\"Tools of Destruction Recompiled\"}";
    if (repo_url_ok()) {
        j += ",\"buttons\":[{\"label\":\"GitHub\",\"url\":"; json_str(j, TOD_REPO_URL); j += "}]";
    }
    j += "}}}";
    if (!pipe_write(1, j)) return false;
    /* Discord answers every command; drain it (and see an error) without waiting long. */
    return pipe_service();
}

/* Unix time the process started, so "elapsed" counts from launch. */
long long process_start_unix()
{
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        const ULONGLONG t = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;   /* 100 ns since 1601 */
        if (t > 116444736000000000ull) return (long long)((t - 116444736000000000ull) / 10000000ull);
    }
    return (long long)time(nullptr);
}

DWORD WINAPI presence_thread(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const long long start = process_start_unix();
    ULONGLONG next_try = GetTickCount64() + kFirstTryMs;
    ULONGLONG last_sent = 0;
    bool had_link = false;
    ULONGLONG connected_at = 0;
    TodPresence shown = { "?", "?" }, want = shown;
    for (;;) {
        Sleep(kLoopMs);
        const ULONGLONG now = GetTickCount64();

        if (g_pipe == INVALID_HANDLE_VALUE) {
            if (now < next_try) continue;
            next_try = now + kRetryMs;
            if (!connect()) continue;                    /* silent: Discord is simply not running */
            fprintf(stderr, "[discord] connected\n");
            had_link = true;
            connected_at = now;
            shown = { "?", "?" };                        /* a new connection: show it afresh */
            last_sent = 0;
        }
        if (!pipe_service()) {                           /* Discord closed mid-session */
            disconnect();
            if (had_link) { fprintf(stderr, "[discord] lost the connection; will retry quietly\n"); had_link = false; }
            next_try = now + kRetryMs;
            continue;
        }
        want = tod_discord_presence();
        if (want == shown) continue;
        /* Just connected and the game has not said anything yet: give it a few seconds before
         * showing a bare "Playing ..." with no level (the next update would wait 15 s). */
        if (!last_sent && !want.details && !want.state && now - connected_at < 8000) continue;
        if (last_sent && now - last_sent < kMinUpdateMs) continue;
        if (!set_activity(want, start)) {
            disconnect();
            if (had_link) { fprintf(stderr, "[discord] lost the connection; will retry quietly\n"); had_link = false; }
            next_try = now + kRetryMs;
            continue;
        }
        shown = want;
        last_sent = now;
    }
    return 0;
}

}  // namespace

extern "C" void tod_discord_start(void)
{
    const char* off = getenv("TOD_DISCORD");
    if (off && off[0] == '0') return;
    const char* id = getenv("TOD_DISCORD_APP_ID");
    if (id && *id) g_app_id = id;
    /* "0" / empty / non-numeric: the placeholder of a build with no Application ID -> inactive, silently. */
    if (!g_app_id[0] || !strcmp(g_app_id, "0")) return;
    for (const char* p = g_app_id; *p; p++) if (*p < '0' || *p > '9') return;
    g_verbose = getenv("TOD_DISCORD_LOG") != nullptr;
    const char* pre = getenv("TOD_DISCORD_PIPE");
    if (pre && *pre) g_pipe_prefix = pre;
    g_ev = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!g_ev) return;
    HANDLE t = CreateThread(nullptr, 0, presence_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

#else

extern "C" void tod_discord_start(void) {}

#endif
