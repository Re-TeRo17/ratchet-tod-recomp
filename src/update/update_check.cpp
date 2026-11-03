/* The update check: see update_check.h. */
#include "update/update_check.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

#include <atomic>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

namespace {

std::atomic<int> s_state{ 0 };
char s_latest[32];
std::atomic<bool> s_started{ false };

/* "v1.02" / "1.02" / "1.0.3" -> {1, 2} / {1, 0, 3}; the numbers only. */
std::vector<long> version_parts(const char* s)
{
    std::vector<long> v;
    while (*s && !isdigit((unsigned char)*s)) s++;
    while (*s) {
        if (!isdigit((unsigned char)*s)) break;
        char* end = NULL;
        v.push_back(strtol(s, &end, 10));
        s = end;
        if (*s != '.') break;
        s++;
    }
    return v;
}

/* >0 when a is newer than b. Missing parts count as 0 (1.0 == 1.0.0). */
int version_cmp(const char* a, const char* b)
{
    const std::vector<long> x = version_parts(a), y = version_parts(b);
    for (size_t i = 0; i < x.size() || i < y.size(); i++) {
        const long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p > q ? 1 : -1;
    }
    return 0;
}

bool https_get(const wchar_t* host, const wchar_t* path, std::string* body)
{
    bool ok = false;
    HINTERNET ses = WinHttpOpen(L"tod-recomp/" TEXT(TOD_RELEASE_VERSION), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    WinHttpSetTimeouts(ses, 5000, 5000, 5000, 5000);
    HINTERNET con = WinHttpConnect(ses, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE)
                        : NULL;
    if (req && WinHttpSendRequest(req, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, NULL)) {
        DWORD code = 0, len = sizeof code;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code,
                            &len, WINHTTP_NO_HEADER_INDEX);
        if (code == 200) {
            ok = true;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail)) { ok = false; break; }
                if (!avail) break;
                if (body->size() + avail > (1u << 20)) { ok = false; break; }   /* a release page is a few KB */
                const size_t at = body->size();
                body->resize(at + avail);
                DWORD got = 0;
                if (!WinHttpReadData(req, &(*body)[at], avail, &got)) { ok = false; break; }
                body->resize(at + got);
            }
        } else {
            fprintf(stderr, "[update] the release check answered HTTP %lu\n", (unsigned long)code);
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

/* The value of "tag_name" in GitHub's JSON (a plain search: the field is a short string). */
bool tag_name(const std::string& json, char* out, size_t n)
{
    size_t p = json.find("\"tag_name\"");
    if (p == std::string::npos) return false;
    p = json.find(':', p);
    if (p == std::string::npos) return false;
    p = json.find('"', p);
    if (p == std::string::npos) return false;
    const size_t e = json.find('"', p + 1);
    if (e == std::string::npos || e - p - 1 == 0 || e - p - 1 >= n) return false;
    memcpy(out, json.data() + p + 1, e - p - 1);
    out[e - p - 1] = 0;
    return true;
}

DWORD WINAPI check_thread(LPVOID)
{
    std::string body;
    char tag[32] = {};
    if (const char* test = getenv("TOD_UPDATE_TEST_TAG")) {    /* test: pretend GitHub answered this tag, no network */
        snprintf(tag, sizeof tag, "%s", test);
        body = "{\"tag_name\": \"" + std::string(tag) + "\"}";
    } else if (!https_get(L"api.github.com", L"/repos/Re-TeRo17/ratchet-tod-recomp/releases/latest", &body) ||
        !tag_name(body, tag, sizeof tag) || version_parts(tag).empty()) {
        fprintf(stderr, "[update] could not check for a newer release (offline?)\n");
        s_state = -1;
        return 0;
    }
    const char* t = tag;
    while (*t && !isdigit((unsigned char)*t)) t++;
    if (version_cmp(t, TOD_RELEASE_VERSION) > 0) {
        snprintf(s_latest, sizeof s_latest, "%s", t);
        fprintf(stderr, "[update] version %s is available (this is %s): %s\n", s_latest, TOD_RELEASE_VERSION, TOD_RELEASES_URL);
        s_state = 2;
    } else {
        fprintf(stderr, "[update] up to date (%s; latest release %s)\n", TOD_RELEASE_VERSION, t);
        s_state = 1;
    }
    return 0;
}

}  // namespace

extern "C" void tod_update_check_start(void)
{
    const char* e = getenv("TOD_UPDATE_CHECK");
    if (e && e[0] == '0') { fprintf(stderr, "[update] check off (TOD_UPDATE_CHECK=0)\n"); return; }
    if (s_started.exchange(true)) return;
    HANDLE h = CreateThread(NULL, 0, check_thread, NULL, 0, NULL);
    if (h) {
        SetThreadPriority(h, THREAD_PRIORITY_LOWEST);
        CloseHandle(h);
    }
}

extern "C" int tod_update_state(void) { return s_state.load(); }

extern "C" const char* tod_update_latest(void) { return s_state.load() == 2 ? s_latest : ""; }

extern "C" void tod_update_open_page(void)
{
    ShellExecuteA(NULL, "open", TOD_RELEASES_URL, NULL, NULL, SW_SHOWNORMAL);
}
