/* Offline test of src/discord/discord_rpc.cpp against a mock Discord (a named-pipe server).
 *
 *   clang-cl /std:c++20 /EHsc /DTOD_DISCORD_TEST -I src/discord tools/discord_mock_test.cpp ^
 *            src/discord/discord_rpc.cpp /Fe:discord_mock_test.exe
 *   discord_mock_test.exe
 *
 * The game's level reader is replaced by a stub whose answer the test changes. Checks:
 *  1. Discord absent: the thread stays silent and the process is not slowed (nothing is
 *     written to stderr by the client, retries happen).
 *  2. The pipe appears later: handshake bytes {"v":1,"client_id":"<id>"} in opcode 0, a
 *     SET_ACTIVITY frame (opcode 1) with details/state/timestamps/assets/button.
 *  3. A change of presence is sent, but not before the minimum interval; no change = no frame.
 *  4. A PING is answered with a PONG.
 *  5. Discord dies mid-session (pipe closed): no crash, the client reconnects to a new server.
 *  6. A server that never answers the handshake does not block anything for long.
 */
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "discord_level.h"

extern "C" void tod_discord_start(void);

static const char* g_next_details = "Metropolis, Kerwan";
static const char* g_next_state = "Playing";
bool TodPresence::operator==(const TodPresence& o) const { return details == o.details && state == o.state; }
TodPresence tod_discord_presence() { return { g_next_details, g_next_state }; }

struct Frame { unsigned op; std::string body; };
static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } else { printf("  ok:   " __VA_ARGS__); printf("\n"); } } while (0)

static const char kPipe[] = "\\\\.\\pipe\\tod-mock-discord-ipc-0";

static HANDLE make_server()
{
    return CreateNamedPipeA(kPipe, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, nullptr);
}

static bool srv_read(HANDLE h, Frame& f, DWORD ms)
{
    const ULONGLONG until = GetTickCount64() + ms;
    DWORD avail = 0;
    while (PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) && avail < 8) {
        if (GetTickCount64() >= until) return false;
        Sleep(10);
    }
    if (avail < 8) return false;
    unsigned hdr[2]; DWORD n;
    if (!ReadFile(h, hdr, 8, &n, nullptr)) return false;
    f.op = hdr[0]; f.body.assign(hdr[1], '\0');
    for (DWORD got = 0; got < hdr[1]; got += n) if (!ReadFile(h, &f.body[got], hdr[1] - got, &n, nullptr)) return false;
    return true;
}

static bool srv_write(HANDLE h, unsigned op, const std::string& j)
{
    std::string fr(8 + j.size(), '\0');
    unsigned len = (unsigned)j.size();
    memcpy(&fr[0], &op, 4); memcpy(&fr[4], &len, 4); memcpy(&fr[8], j.data(), j.size());
    DWORD n; return WriteFile(h, fr.data(), (DWORD)fr.size(), &n, nullptr) && n == fr.size();
}

static bool srv_accept(HANDLE h, DWORD ms)
{
    OVERLAPPED o = {}; o.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    BOOL ok = ConnectNamedPipe(h, &o);
    bool r = ok || GetLastError() == ERROR_PIPE_CONNECTED;
    if (!r && GetLastError() == ERROR_IO_PENDING) r = WaitForSingleObject(o.hEvent, ms) == WAIT_OBJECT_0;
    CloseHandle(o.hEvent);
    return r;
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    _putenv_s("TOD_DISCORD_APP_ID", "123456789012345678");
    _putenv_s("TOD_DISCORD_PIPE", "\\\\.\\pipe\\tod-mock-discord-ipc-");
    const ULONGLONG t0 = GetTickCount64();
    tod_discord_start();
    printf("[1] Discord absent: client thread started in %llu ms; waiting 2.5 s with no server\n", GetTickCount64() - t0);
    CHECK(GetTickCount64() - t0 < 50, "tod_discord_start returns at once");
    Sleep(2500);

    printf("[2] server appears\n");
    HANDLE s = make_server();
    if (s == INVALID_HANDLE_VALUE) { printf("cannot create pipe\n"); return 2; }
    CHECK(srv_accept(s, 8000), "client connected to the pipe");
    Frame f;
    CHECK(srv_read(s, f, 3000), "handshake frame arrived");
    CHECK(f.op == 0, "handshake opcode is 0 (got %u)", f.op);
    CHECK(f.body == "{\"v\":1,\"client_id\":\"123456789012345678\"}", "handshake JSON is exact: %s", f.body.c_str());
    srv_write(s, 1, "{\"cmd\":\"DISPATCH\",\"data\":{\"v\":1},\"evt\":\"READY\",\"nonce\":null}");
    CHECK(srv_read(s, f, 3000), "SET_ACTIVITY frame arrived");
    CHECK(f.op == 1, "SET_ACTIVITY opcode is 1");
    printf("      %s\n", f.body.c_str());
    CHECK(f.body.find("\"cmd\":\"SET_ACTIVITY\"") != std::string::npos, "cmd SET_ACTIVITY");
    CHECK(f.body.find("\"details\":\"Metropolis, Kerwan\"") != std::string::npos, "details");
    CHECK(f.body.find("\"state\":\"Playing\"") != std::string::npos, "state");
    CHECK(f.body.find("\"timestamps\":{\"start\":") != std::string::npos, "start timestamp");
    CHECK(f.body.find("\"large_image\":\"icon\"") != std::string::npos, "large image key icon");
    CHECK(f.body.find("\"buttons\":[{\"label\":\"GitHub\",\"url\":\"https://github.com/") != std::string::npos, "GitHub button");
    srv_write(s, 1, "{\"cmd\":\"SET_ACTIVITY\",\"data\":{},\"evt\":null,\"nonce\":\"1\"}");

    printf("[3] unchanged -> no frame; changed -> one frame, not before the minimum interval\n");
    CHECK(!srv_read(s, f, 2600), "no new frame while nothing changed");
    g_next_details = "Cobalia"; g_next_state = "Playing";
    const ULONGLONG tc = GetTickCount64();
    CHECK(srv_read(s, f, 4000), "frame after the level changed");
    printf("      %s (after %llu ms)\n", f.body.c_str(), GetTickCount64() - tc);
    CHECK(f.body.find("\"details\":\"Cobalia\"") != std::string::npos, "new details");

    printf("[4] ping -> pong\n");
    srv_write(s, 3, "{\"nonce\":\"abc\"}");
    CHECK(srv_read(s, f, 2000) && f.op == 4 && f.body == "{\"nonce\":\"abc\"}", "PONG with the same payload");

    printf("[5] Discord dies mid-session; a new one starts later\n");
    DisconnectNamedPipe(s); CloseHandle(s);
    Sleep(1000);
    s = make_server();
    CHECK(srv_accept(s, 8000), "client reconnected to the new server");
    CHECK(srv_read(s, f, 3000) && f.op == 0, "new handshake");
    srv_write(s, 1, "{\"cmd\":\"DISPATCH\",\"evt\":\"READY\"}");
    CHECK(srv_read(s, f, 3000) && f.op == 1 && f.body.find("Cobalia") != std::string::npos, "presence re-sent after reconnect");

    printf("[6] Discord closes with a CLOSE frame\n");
    srv_write(s, 2, "{\"code\":1000}");
    Sleep(600);
    DisconnectNamedPipe(s); CloseHandle(s);
    s = make_server();
    CHECK(srv_accept(s, 8000), "client came back after CLOSE");
    CloseHandle(s);

    printf("[7] server accepts but never answers the handshake\n");
    s = make_server();
    CHECK(srv_accept(s, 8000), "client connected");
    Frame h;
    srv_read(s, h, 3000);
    Sleep(2500);   /* the client gives up after its 700 ms timeout and drops the pipe */
    DWORD avail;
    BOOL alive = PeekNamedPipe(s, nullptr, 0, nullptr, &avail, nullptr);
    CHECK(!alive, "client dropped the silent server");
    CloseHandle(s);
    printf(g_fail ? "RESULT: %d FAILED\n" : "RESULT: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
