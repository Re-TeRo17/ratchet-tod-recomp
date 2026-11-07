/*
 * tod_autopilot.cpp -- AUTOPILOT: Ratchet plays by himself (for agent tests). Wired in src/input/host_input.cpp
 * (tod_autopilot_apply, called after the keyboard / script / bot filled the pad). OFF unless TOD_AUTOPILOT is set.
 *
 *   TOD_AUTOPILOT=<level name | path file>   e.g. "pirate base" -> autopilot\pirate base.txt (searched in
 *                                            TOD_AP_DIR, then ..\tools_sweep\autopilot, then tools_sweep\autopilot,
 *                                            then the cwd), or a path to a file.
 *   TOD_AP_SURVEY=<file>                     names only (one volume per line): warp to each (hero_warp), wait, log the
 *                                            position "[autopilot] survey NAME x y z" -> build a path file from it.
 *   TOD_AP_DELAY=<s>   seconds after the hero is ready before it drives (default 6)
 *   TOD_AP_RADIUS=<f>  default arrival radius (default 4.0 world units)
 *   TOD_AP_STUCK=<s>   no progress for this long -> warp to the current step's volume (default 20)
 *   TOD_AP_STEP_MAX=<s> a step may take at most this long, then warp (default 120)
 *   TOD_AP_FIRE=<ms>   R1 period (default 350, held 200 ms); 0 = never fire. TOD_AP_MELEE=<ms> square tap period (default 900; 0 off)
 *   TOD_AP_WEAPON=<WPN_X>  hero_equip(Ratchet1,WPN_X) once at the start (default none; TOD_CHEAT=weapons first)
 *   TOD_AP_LOOP=1      restart the path at the end;  TOD_AP_NOWARP=1: never warp (log only)
 *   TOD_AP_PLANE=xy|xz ground plane (default xz: Y is up)
 *
 * Path file (one step per line, '#' comments):
 *   <volume name> <x> <y> <z> [radius] [opt...]     opt: w=<s> idle <s> seconds after arriving (cutscene), warp (warp straight
 *                                                   there, do not walk), nowarp (never warp on this step, only time out)
 *   <volume name>                                   no coordinates: warp-only step
 *   !weapon WPN_X        equip at start        !lua <chunk>    run this Lua at that point of the path
 *
 * Position: the live Ratchet entity (tod_cheat.cpp player_ent, ent+0x30). Camera: not read; the stick is relative to the
 * camera, so its yaw (and the world handedness) is estimated online from "commanded stick angle vs observed velocity".
 * The pad is only written; guest calls (hero_warp, hero_equip) go through the Lua queue of tod_cheat.cpp.
 * COMBAT (auto119): entities are read every 0.2 s (tod_cheat_ents). Enemy = vtable in TOD_AP_ENEMY_VT (hex list "8477E0,84A3F8") or,
 * without a list, a "walker": an entity alive > 1.5 s that has travelled > 3 units. The nearest one within TOD_AP_ENEMY_RANGE (28) is
 * the target: Ratchet turns to it (stick toward it, slow when close), R1 fires only when he faces it (facing = matrix row0 + learned
 * offset), square when it is close. When it vanishes = kill. TOD_AP_BLINDFIRE=<ms> = old blind R1 burst when there is no target (0 off).
 * TOD_AP_STRAFE=1: hold L1 and circle around the target. Every 5 s "[autopilot] census vt=.. n=.. near=.. movers=.." per vtable.
 * CAMERA: right stick: toward the target while fighting, else a 1.2 s swing every ~7 s (TOD_AP_CAM=0 off). JUMP: random every 5-12 s,
 * and while the speed is < 1.2 with the stick pushed. Per step a "STAGE" line: frames, seconds, fps (hitch log: RSX_HITCH_LOG=0.5).
 * auto118b: visited grid (6 u cells) + frontier heading in free roam; safe positions every 0.5 s; falling (y drops, vy<-4/s): steer back to the
 * safe point, at drop>12 "warp=x:y:z" back to it (TOD_AP_SAFEWARP=0 off) and mark the edge cells bad; FIGHT BUDGET (25 s in one place -> leave 15 s);
 * FROZEN watchdog escalates (side-step, reverse, explore + warp to an older safe point).
 * Log prefix: "[autopilot]".
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

#include "win32_compat.h"      /* GetTickCount64 */
#include "cellPad.h"           /* CellPadHostInput, CELL_PAD_CTRL_* */

extern "C" int  tod_cheat_player_pos(float* xyz);      /* tod_cheat.cpp: 1 = Ratchet entity found */
extern "C" void tod_cheat_lua(const char* code);       /* tod_cheat.cpp: queued, runs on the guest thread */
extern "C" void tod_cheat_arm(void);
extern "C" int  tod_cheat_lua_idle(void);              /* all queued chunks have run */
extern "C" int  tod_cheat_hero_ready(void);            /* hero valid 2 s + guest thread: the level runs */
extern "C" uint32_t g_rsx_engine_frame;
struct ApEnt { uint32_t h, vt; float x, y, z; };
extern "C" int  tod_cheat_ents(ApEnt* out, int max);    /* tod_cheat.cpp: live entities except Ratchet */
extern "C" int  tod_cheat_player_mat(float* o);         /* row0.x,row0.z,row2.x,row2.z,row0.y,row2.y */
extern "C" void tod_cheat_enqueue(const char* cmds);   /* tod_cheat.cpp: "warp=x:y:z" runs on the guest thread */
extern "C" int  tod_cheat_hero_dump(void* f, double t, uint32_t label);   /* tod_cheat.cpp: autofix8 binary dump */   /* tod_cheat.cpp: "warp=x:y:z" runs on the guest thread */

namespace {

struct Step {
    std::string name;
    float x = 0, y = 0, z = 0, r = 0;
    bool haspos = false, warp_only = false, nowarp = false;
    double wait = 0, roam = 0;
    std::string lua;
};

std::vector<Step> s_path, s_survey;
bool     s_on, s_xz = true, s_loop, s_nowarp, s_surveying;
double   s_delay = 6, s_defr = 4, s_stuck_s = 20, s_step_max = 120;
int      s_fire_ms = 300, s_melee_ms = 700;
std::string s_weapon;
uint64_t s_t0;

/* run state */
size_t   s_cur;
double   s_ready_t = -1, s_step_t0 = -1, s_prog_t = -1, s_idle_until = 0;
float    s_best = 1e9f;
bool     s_started, s_done, s_weapon_done;
int      s_reached_walk, s_reached_warp, s_stuck_events, s_warps, s_laps;
double   s_walk_dist;
float    s_lastx, s_lasty; bool s_have_last;

/* camera / handedness estimation */
float    s_theta;         /* world heading of "stick right" */
float    s_sgn = 1.f;     /* +1: world heading = theta + phi ; -1 mirrored */
int      s_calib;         /* 0 probe forward, 1 probe right, 2 done */
double   s_calib_t0 = -1;
float    s_pa, s_px, s_py;     /* probe start position / forward probe heading */
float    s_va;
float    s_lastphi; bool s_cmd_moving; float s_sx, s_sy; double s_st = -1;

/* stuck recovery */
double   s_mode_until; int s_mode; /* 0 normal, 1 jump, 2 sidestep, 3 reverse */
float    s_side = 1.f; double s_last_escape = -9;
double   s_survey_t0 = -1; size_t s_survey_i; bool s_survey_sent;

/* ---- combat / camera / jump state (auto119) ---- */
struct Trk { uint32_t vt; double t0, tl; float x, z, path; bool seen; int glued; };
std::unordered_map<uint32_t, Trk> s_trk;
std::unordered_map<uint32_t, double> s_ignore;
std::vector<uint32_t> s_enemy_vt;
uint32_t s_tgt; double s_tgt_t0, s_scan_t = -9, s_census_t = -9; float s_tgt_x, s_tgt_z, s_tgt_d;
bool s_brawl;   /* uni127 TOD_AP_BRAWL */
double s_enemy_range = 28, s_engage = 12, s_tgt_max = 8, s_fire_range = 24; int s_kills, s_targets, s_blind_ms; bool s_strafe, s_cam = true;
double s_fire_total, s_cam_next = 6, s_cam_until, s_jump_next = 4, s_jump_until, s_slow_t0 = -1, s_strafe_t = 0; float s_strafe_dir = 1.f;
float s_off_s, s_off_c; int s_off_n; bool s_roam_on, s_roam_have_c; double s_roam_end, s_roam_next; float s_roam_cx, s_roam_cy, s_roam_tx, s_roam_ty;
int s_falls, s_respawns; double s_last_warp_ms = 0;
double s_ammo_s = 4, s_ammo_t = 0, s_duration = 0, s_start_t = -1, s_frz_t0 = -1; float s_frz_x, s_frz_y; bool s_camtgt; int s_frozen;
float s_vspeed = 5.f; bool s_probe_right; double s_phi_t = 0; float s_phi_ref;
float s_camsgn = 1.f, s_cam_err0; double s_cam_t0 = -1; float s_cam_dir = 1.f; int s_camflips, s_jumps_rand;
uint32_t s_step_f0;

/* ---- auto118b: visited grid, safe positions, fall recovery ---- */
struct Cell { uint8_t v, bad; };
std::unordered_map<long long, Cell> s_grid;
struct SafePt { float x, y, z; double t; };
std::vector<SafePt> s_safe;
float  s_ground_y, s_drop, s_rec_x, s_rec_z, s_gdx = 1.f, s_gdz;
double s_away_until = -1; float s_away_psi; int s_fall_rep; float s_lf_x = 1e9f, s_lf_z; double s_lf_t = -99; bool s_prev_flat;   /* repeat-fall guard */
double s_reflex_t = -9, s_reflex_until = -1; int s_reflex_n; bool s_reflex_on = true;   /* TOD_AP_REFLEX=0 off */
bool   s_air, s_safewarp = true;
double s_air_t0 = -1, s_fall_warp_t = -99, s_safe_t = -9, s_fight_acc, s_frz_until, s_frz_last = -999, s_samp_t = -1;
int    s_safe_warps, s_frz_consec, s_descents; float s_wdrop = 30.f;   /* TOD_AP_SAFEWARP_DROP */
float  s_samp_x, s_samp_y, s_samp_z;
long long ckey(float x, float z) { const int cx = (int)floorf(x / 6.f), cz = (int)floorf(z / 6.f); return ((long long)(cx + 32768) << 32) | (unsigned)(cz + 32768); }
Cell* cell_find(float x, float z) { auto it = s_grid.find(ckey(x, z)); return it == s_grid.end() ? nullptr : &it->second; }
Cell& cell_get(float x, float z) { auto it = s_grid.find(ckey(x, z)); if (it == s_grid.end()) it = s_grid.emplace(ckey(x, z), Cell{0, 0}).first; return it->second; }
std::string s_edge_file; bool s_edge_loading;   /* TOD_AP_EDGEMEM=<dir>: fall edges persist across runs (<dir>/<TOD_AUTOPILOT>.txt) so the next run avoids them BEFORE falling */
void mark_bad(float x, float z, int amt) { Cell& c = cell_get(x, z); c.bad = (uint8_t)(std::min)(255, (int)c.bad + amt);
    if (amt >= 100 && !s_edge_loading && !s_edge_file.empty()) { if (FILE* f = fopen(s_edge_file.c_str(), "ab")) { fprintf(f, "%.1f %.1f %d\n", x, z, amt); fclose(f); } } }
void edge_load() { if (s_edge_file.empty()) return; FILE* f = fopen(s_edge_file.c_str(), "rb"); if (!f) return; float x, z; int a, n = 0; s_edge_loading = true;
    while (fscanf(f, "%f %f %d", &x, &z, &a) == 3) { mark_bad(x, z, a); n++; } s_edge_loading = false; fclose(f); fprintf(stderr, "[autopilot] edgemem: loaded %d marks from %s\n", n, s_edge_file.c_str()); }
void add_visit(float x, float z, int n) { Cell& c = cell_get(x, z); c.v = (uint8_t)(std::min)(255, (int)c.v + n); }
float wrap_(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }
/* frontier heading: 16 rays of 8 cells (6 u); cost = visits + bad cells (fall edge / wall) + turn penalty; the cheapest ray = the least explored */
float pick_heading(float gx, float gz, float cur)
{
    float bc = 1e9f, ba = cur;
    for (int i = 0; i < 16; i++) {
        const float a = i * 0.3926991f; float cost = 0; bool blocked = false;
        for (int k = 1; k <= 8; k++) {
            const Cell* c = cell_find(gx + 6.f * k * cosf(a), gz + 6.f * k * sinf(a));
            if (!c) continue;
            cost += (std::min)((int)c->v, 40) * 0.4f + (float)c->bad;
            if (c->bad >= 100 && !blocked) { blocked = true; cost += 200.f * (9 - k); }
        }
        cost += 4.0f * fabsf(wrap_(a - cur)) + 0.01f * i;
        if (cost < bc) { bc = cost; ba = a; }
    }
    return ba;
}
unsigned s_rng = 12345u;
float frand() { s_rng = s_rng * 1664525u + 1013904223u; return (s_rng >> 8) / 16777216.f; }

float wrap(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

bool file_exists(const std::string& p) { FILE* f = fopen(p.c_str(), "rb"); if (f) { fclose(f); return true; } return false; }

std::string find_path_file(const std::string& arg)
{
    if (file_exists(arg)) return arg;
    std::vector<std::string> dirs;
    if (const char* d = getenv("TOD_AP_DIR")) dirs.push_back(d);
    dirs.push_back("..\\tools_sweep\\autopilot"); dirs.push_back("tools_sweep\\autopilot");
    dirs.push_back(".");
    for (auto& d : dirs) {
        std::string p = d + "\\" + arg + ".txt";
        if (file_exists(p)) return p;
    }
    return "";
}

bool load(const std::string& path, std::vector<Step>& out, bool names_only)
{
    FILE* f = fopen(path.c_str(), "r");
    if (!f) { fprintf(stderr, "[autopilot] cannot open %s\n", path.c_str()); return false; }
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        std::string l = trim(line);
        if (l.empty() || l[0] == '#') continue;
        if (l[0] == '!') {
            if (!l.compare(0, 8, "!weapon ")) s_weapon = trim(l.substr(8));
            else if (!l.compare(0, 5, "!lua ")) { Step s; s.name = "lua"; s.lua = trim(l.substr(5)); s.warp_only = true; s.nowarp = true; s.wait = 0.5; out.push_back(s); }
            continue;
        }
        Step s; char nm[256]; int n = 0;
        if (sscanf(l.c_str(), "%255s%n", nm, &n) != 1) continue;
        s.name = nm;
        const char* rest = l.c_str() + n;
        float a[4] = {0, 0, 0, 0}; int k = 0, used = 0;
        if (!names_only) {
            const char* p = rest;
            while (k < 4) { char* e = nullptr; float v = strtof(p, &e); if (e == p) break; a[k++] = v; p = e; }
            used = (int)(p - rest);
            if (k >= 3) { s.haspos = true; s.x = a[0]; s.y = a[1]; s.z = a[2]; s.r = k >= 4 ? a[3] : 0.f; }
            rest += used;
        }
        std::string opts = rest;
        size_t pos = 0;
        while (pos < opts.size()) {
            while (pos < opts.size() && (opts[pos] == ' ' || opts[pos] == '\t')) pos++;
            size_t e = pos; while (e < opts.size() && opts[e] != ' ' && opts[e] != '\t') e++;
            std::string o = opts.substr(pos, e - pos); pos = e;
            if (o.empty()) continue;
            if (!o.compare(0, 2, "w=")) s.wait = atof(o.c_str() + 2);
            else if (!o.compare(0, 5, "roam=")) s.roam = atof(o.c_str() + 5);
            else if (o == "warp") s.warp_only = true;
            else if (o == "nowarp") s.nowarp = true;
        }
        if (!s.haspos) s.warp_only = true;
        out.push_back(s);
    }
    fclose(f);
    return true;
}

void lua_warp(const std::string& name)
{
    s_last_warp_ms = (double)GetTickCount64();
    std::string c = "hero_warp(Ratchet1," + name + ")";
    tod_cheat_lua(c.c_str());
}

void summary(double t, const char* why)
{
    fprintf(stderr, "[autopilot] summary(%s) t=%.1f safew=%d steps=%zu cur=%zu walked=%d warped=%d stuck_events=%d warps=%d laps=%d walk_dist=%.0f targets=%d kills=%d fire_s=%.1f jumps_rand=%d cam_flips=%d face_n=%d falls=%d respawns=%d frozen=%d descents=%d\n",
            why, t, s_safe_warps, s_path.size(), s_cur, s_reached_walk, s_reached_warp, s_stuck_events, s_warps, s_laps, s_walk_dist, s_targets, s_kills, s_fire_total, s_jumps_rand, s_camflips, s_off_n, s_falls, s_respawns, s_frozen, s_descents);
}

void next_step(double t, bool by_walk)
{
    const Step& st = s_path[s_cur];
    fprintf(stderr, "[autopilot] t=%.1f step %zu/%zu '%s' reached by %s after %.1f s\n", t, s_cur + 1, s_path.size(), st.name.c_str(),
            by_walk ? "WALKING" : "warp", s_step_t0 >= 0 ? t - s_step_t0 : 0.0);
    if (by_walk) s_reached_walk++; else s_reached_warp++;
    fprintf(stderr, "[autopilot] STAGE %zu '%s' by=%s t0=%.1f t1=%.1f f0=%u f1=%u fps=%.1f kills=%d\n", s_cur + 1, st.name.c_str(), by_walk ? "walk" : "warp",
            s_step_t0 >= 0 ? s_step_t0 : t, t, s_step_f0, g_rsx_engine_frame, (s_step_t0 >= 0 && t > s_step_t0) ? (g_rsx_engine_frame - s_step_f0) / (t - s_step_t0) : 0.0, s_kills);
    if (st.wait > 0) s_idle_until = t + st.wait;
    s_cur++; s_step_t0 = -1; s_prog_t = -1; s_best = 1e9f; s_mode = 0; s_have_last = false;
    if (s_cur >= s_path.size()) {
        if (s_loop) { s_cur = 0; s_laps++; fprintf(stderr, "[autopilot] t=%.1f path done, loop %d\n", t, s_laps); }
        else { s_done = true; fprintf(stderr, "[autopilot] t=%.1f PATH DONE\n", t); summary(t, "done"); }
    }
}

void do_warp(double t, const char* why)
{
    const Step& st = s_path[s_cur];
    s_warps++;
    if (st.roam > 0 && !s_roam_on) {   /* warp, then wander around the landing point for roam seconds (the step ends when the time is up) */
        if (!s_nowarp && st.name != "-") { fprintf(stderr, "[autopilot] t=%.1f %s -> warp to '%s' + roam %.0f s\n", t, why, st.name.c_str(), st.roam); lua_warp(st.name); }
        s_roam_on = true; s_roam_have_c = false; s_roam_end = t + 2.5 + st.roam; s_roam_next = 0; s_idle_until = t + 2.5;
        return;
    }
    if (s_nowarp) { fprintf(stderr, "[autopilot] t=%.1f %s -> warp to '%s' SKIPPED (TOD_AP_NOWARP)\n", t, why, st.name.c_str()); s_prog_t = t; s_step_t0 = t; return; }
    fprintf(stderr, "[autopilot] t=%.1f %s -> warp to '%s'\n", t, why, st.name.c_str());
    lua_warp(st.name);
    next_step(t, false);
    s_idle_until = t + 2.5;   /* let the warp land */
}

double s_explore_until, s_explore_t0 = -1, s_explore_every = 25, s_explore_len = 12, s_pk_t0 = -1; float s_pk_x, s_pk_y; int s_stuck_events_free; bool s_free; float s_roam_ang; double s_roam_rmax = 16;
bool is_enemy_vt(uint32_t vt) { for (uint32_t v : s_enemy_vt) if (v == vt) return true; return false; }

/* nearest enemy candidate within range (for the free roam: walk toward the fight) */
bool nearest_enemy(float gx, float gz, float range, float* ox, float* oz)
{
    float bd = 1e9f; bool f = false;
    for (auto& kv : s_trk) {
        const Trk& k = kv.second; const float d = hypotf(k.x - gx, k.z - gz);
        if (d > range || d < 4.0f || k.glued > 25) continue;
        { const Cell* c = cell_find(k.x, k.z); if (c && (c->v >= 40 || c->bad >= 100)) continue; }   /* area already fought over / dangerous */
        const bool cand = s_enemy_vt.empty() ? ((k.path > 3.f)) : is_enemy_vt(k.vt);
        if (!cand) continue;
        if (d < bd) { bd = d; *ox = k.x; *oz = k.z; f = true; }
    }
    return f;
}

/* every 0.2 s: update the tracker, pick the target (nearest candidate), count kills, census log */
void scan_ents(double t, float gx, float gz)
{
    static ApEnt buf[4096];
    const int n = tod_cheat_ents(buf, 4096);
    for (auto& kv : s_trk) kv.second.seen = false;
    for (int i = 0; i < n; i++) {
        const ApEnt& e = buf[i];
        if (!isfinite(e.x) || !isfinite(e.z) || !isfinite(e.y) || fabsf(e.x) > 1e5f || fabsf(e.z) > 1e5f) continue;
        auto it = s_trk.find(e.h);
        if (it == s_trk.end() || it->second.vt != e.vt) { Trk k; k.vt = e.vt; k.t0 = k.tl = t; k.x = e.x; k.z = e.z; k.path = 0; k.seen = true; k.glued = 0; s_trk[e.h] = k; continue; }
        Trk& k = it->second;
        const float m = hypotf(e.x - k.x, e.z - k.z);
        if (m > 0.05f && m < 6.f) { k.path += m; k.tl = t; }
        k.x = e.x; k.z = e.z; k.seen = true;
        if (hypotf(e.x - gx, e.z - gz) < 2.5f) k.glued++;
    }
    for (auto it = s_trk.begin(); it != s_trk.end();) {
        if (!it->second.seen) {
            if (it->first == s_tgt) { s_kills++; fprintf(stderr, "[autopilot] t=%.1f KILL target h=%08X vt=%08X after %.1f s (kills=%d)\n", t, it->first, it->second.vt, t - s_tgt_t0, s_kills); s_tgt = 0; }
            it = s_trk.erase(it);
        } else ++it;
    }
    /* target: keep the current one while valid, else the nearest candidate */
    if (s_tgt) {
        auto it = s_trk.find(s_tgt);
        if (it == s_trk.end()) s_tgt = 0;
        else if (t - s_tgt_t0 > s_tgt_max) { fprintf(stderr, "[autopilot] t=%.1f target h=%08X timeout (%.1f s), ignore 30 s\n", t, s_tgt, s_tgt_max); s_ignore[s_tgt] = t + 30; s_tgt = 0; }
        else if (hypotf(it->second.x - gx, it->second.z - gz) > s_enemy_range * 1.4f) s_tgt = 0;
    }
    uint32_t best = 0; float bd = 1e9f;
    if (t < s_explore_until || t < s_frz_until) { s_tgt = 0; }
    else for (auto& kv : s_trk) {
        const Trk& k = kv.second;
        const float d = hypotf(k.x - gx, k.z - gz);
        if (d > s_enemy_range || d < 1.0f || k.glued > 25) continue;
        auto ig = s_ignore.find(kv.first); if (ig != s_ignore.end() && t < ig->second) continue;
        const bool cand = s_enemy_vt.empty() ? ((t - k.t0) > 1.5 && k.path > 3.f) : is_enemy_vt(k.vt);
        if (!cand) continue;
        if (d < bd) { bd = d; best = kv.first; }
    }
    if (!s_tgt && best) {
        s_tgt = best; s_tgt_t0 = t; s_targets++;
        fprintf(stderr, "[autopilot] t=%.1f TARGET h=%08X vt=%08X dist=%.1f at (%.1f,%.1f) targets=%d\n", t, best, s_trk[best].vt, bd, s_trk[best].x, s_trk[best].z, s_targets);
    }
    if (s_tgt) { const Trk& k = s_trk[s_tgt]; s_tgt_x = k.x; s_tgt_z = k.z; s_tgt_d = hypotf(k.x - gx, k.z - gz); }
    /* census every 5 s: per vtable within 45 units */
    if (t - s_census_t >= 5.0) {
        s_census_t = t;
        struct C { uint32_t vt; int n, mv; float near_; };
        std::vector<C> cs;
        for (auto& kv : s_trk) {
            const Trk& k = kv.second; const float d = hypotf(k.x - gx, k.z - gz);
            if (d > 45.f) continue;
            C* c = nullptr; for (auto& x : cs) if (x.vt == k.vt) { c = &x; break; }
            if (!c) { cs.push_back({k.vt, 0, 0, 1e9f}); c = &cs.back(); }
            c->n++; if (k.path > 3.f && (t - k.tl) < 3.0) c->mv++; if (d < c->near_) c->near_ = d;
        }
        std::sort(cs.begin(), cs.end(), [](const C& a, const C& b) { return a.near_ < b.near_; });
        char line[1500]; int o = snprintf(line, sizeof line, "[autopilot] t=%.1f census(%d ents tracked) kills=%d targets=%d:", t, (int)s_trk.size(), s_kills, s_targets);
        for (size_t i = 0; i < cs.size() && i < 14 && o < 1400; i++) o += snprintf(line + o, sizeof line - o, " vt=%X n=%d near=%.0f mv=%d;", cs[i].vt, cs[i].n, cs[i].near_, cs[i].mv);
        fprintf(stderr, "%s\n", line);
    }
}

}  // namespace

extern "C" void tod_autopilot_init(void)
{
    const char* e = getenv("TOD_AUTOPILOT");
    const char* sv = getenv("TOD_AP_SURVEY");
    if (!e && !sv) return;
    if (const char* p = getenv("TOD_AP_DELAY")) s_delay = atof(p);
    if (const char* p = getenv("TOD_AP_RADIUS")) s_defr = atof(p);
    if (const char* p = getenv("TOD_AP_STUCK")) s_stuck_s = atof(p);
    if (const char* p = getenv("TOD_AP_STEP_MAX")) s_step_max = atof(p);
    if (const char* p = getenv("TOD_AP_FIRE")) s_fire_ms = atoi(p);
    if (const char* p = getenv("TOD_AP_MELEE")) s_melee_ms = atoi(p);
    if (const char* p = getenv("TOD_AP_WEAPON")) s_weapon = p;
    if (const char* p = getenv("TOD_AP_PLANE")) s_xz = !strcmp(p, "xz");   /* default xz: the game is Y-up (survey 05/11) */
    if (const char* p = getenv("TOD_AP_ENEMY_RANGE")) s_enemy_range = atof(p);
    if (const char* p = getenv("TOD_AP_ENGAGE")) s_engage = atof(p);
    if (const char* p = getenv("TOD_AP_FIRE_RANGE")) s_fire_range = atof(p);
    if (const char* p = getenv("TOD_AP_TARGET_MAX")) s_tgt_max = atof(p);
    if (const char* p = getenv("TOD_AP_BLINDFIRE")) s_blind_ms = atoi(p);
    if (const char* p = getenv("TOD_AP_CAM")) s_cam = atoi(p) != 0;
    s_strafe = getenv("TOD_AP_STRAFE") != nullptr;
    s_brawl = getenv("TOD_AP_BRAWL") != nullptr;   /* uni127: brawl mode, see the combat block */
    s_camtgt = getenv("TOD_AP_CAMTGT") != nullptr;
    if (const char* p = getenv("TOD_AP_AMMO")) s_ammo_s = atof(p);
    if (const char* p = getenv("TOD_AP_ROAM_R")) s_roam_rmax = atof(p);
    if (const char* p = getenv("TOD_AP_EXPLORE")) s_explore_every = atof(p);
    if (const char* p = getenv("TOD_AP_EXPLORE_LEN")) s_explore_len = atof(p);
    if (const char* p = getenv("TOD_AP_DURATION")) s_duration = atof(p);
    if (const char* p = getenv("TOD_AP_REFLEX")) s_reflex_on = atoi(p) != 0;
    if (const char* p = getenv("TOD_AP_SAFEWARP_DROP")) s_wdrop = (float)atof(p);   /* warp back only when the fall is deeper than this (default 30: shorter drops land on real floor) */
    if (const char* p = getenv("TOD_AP_SAFEWARP")) s_safewarp = atoi(p) != 0;   /* 0: after a fall do not warp back (the game respawns) */
    if (const char* p = getenv("TOD_AP_SGN")) s_sgn = atof(p) < 0 ? -1.f : 1.f; else s_sgn = -1.f;
    s_probe_right = getenv("TOD_AP_PROBE_RIGHT") != nullptr;
    if (const char* p = getenv("TOD_AP_ENEMY_VT")) { const char* q = p; while (*q) { char* e = nullptr; unsigned long v = strtoul(q, &e, 16); if (e == q) { q++; continue; } s_enemy_vt.push_back((uint32_t)v); q = e; } }
    if (const char* em = getenv("TOD_AP_EDGEMEM")) { s_edge_file = std::string(em) + "\\" + (e ? e : "x") + ".txt"; edge_load(); }
    s_loop = getenv("TOD_AP_LOOP") != nullptr;
    s_nowarp = getenv("TOD_AP_NOWARP") != nullptr;
    if (sv) {
        s_surveying = true;
        if (!load(sv, s_survey, true)) return;
        fprintf(stderr, "[autopilot] SURVEY mode: %zu volumes from %s\n", s_survey.size(), sv);
    } else {
        if (!strcmp(e, "roam")) {   /* free level mode: no path file, wander toward enemies for the whole run */
            Step st; st.name = "-"; st.warp_only = true; st.nowarp = true; st.roam = 1e9; s_path.push_back(st); s_free = true; s_loop = true;
            fprintf(stderr, "[autopilot] ON: FREE roam mode (no path file)\n");
            s_on = true; tod_cheat_arm(); return;
        }
        const std::string f = find_path_file(e);
        if (f.empty()) { fprintf(stderr, "[autopilot] no path file for '%s'\n", e); return; }
        if (!load(f, s_path, false)) return;
        fprintf(stderr, "[autopilot] ON: %zu steps from %s (delay %.0f s, stuck %.0f s, radius %.1f, fire %d ms, melee %d ms%s)\n",
                s_path.size(), f.c_str(), s_delay, s_stuck_s, s_defr, s_fire_ms, s_melee_ms, s_nowarp ? ", NOWARP" : "");
    }
    s_on = true;
    tod_cheat_arm();
}

/* Called from host_input() AFTER it filled *out. Returns nonzero when it drives. */
extern "C" int tod_autopilot_apply(CellPadHostInput* out)
{
    if (!s_on) return 0;
    if (!s_t0) s_t0 = GetTickCount64();
    const double t = (GetTickCount64() - s_t0) / 1000.0;

    float p3[3];
    if (!tod_cheat_hero_ready() || !tod_cheat_player_pos(p3)) {
        static double lw = -9;
        if (t - lw > 5) { lw = t; fprintf(stderr, "[autopilot] t=%.1f waiting for the Ratchet entity\n", t); }
        s_ready_t = -1;
        return 0;
    }
    if (!isfinite(p3[0]) || !isfinite(p3[1]) || !isfinite(p3[2]) || fabsf(p3[0]) > 1e6f || fabsf(p3[1]) > 1e6f) return 0;
    const float gx = p3[0], gy = s_xz ? p3[2] : p3[1];
    if (s_ready_t < 0) { s_ready_t = t; fprintf(stderr, "[autopilot] t=%.1f Ratchet entity found at (%.1f,%.1f,%.1f)\n", t, p3[0], p3[1], p3[2]); }
    if (t - s_ready_t < s_delay) return 0;

    /* fall / respawn detection */
    { static float zavg = 0, px = 0, pz = 0, py = 0; static bool zi = false; static double lastfall = -9;
      if (!zi) { zi = true; zavg = p3[1]; px = p3[0]; pz = p3[2]; py = p3[1]; }
      const bool ours = (double)GetTickCount64() - s_last_warp_ms < 6000.0;
      if (hypotf(p3[0] - px, p3[2] - pz) > 40.f || fabsf(p3[1] - py) > 40.f) {
          if (!ours) { s_respawns++; fprintf(stderr, "[autopilot] t=%.1f RESPAWN/teleport (%.1f,%.1f,%.1f) -> (%.1f,%.1f,%.1f) #%d\n", t, px, py, pz, p3[0], p3[1], p3[2], s_respawns); }
          s_roam_have_c = false; s_roam_next = 0; s_have_last = false; zavg = p3[1];
      } else if (p3[1] < zavg - 8.f && t - lastfall > 5.0) { lastfall = t; s_falls++; fprintf(stderr, "[autopilot] t=%.1f FALL y=%.1f (avg %.1f) #%d\n", t, p3[1], zavg, s_falls); }
      zavg = zavg * 0.994f + p3[1] * 0.006f; px = p3[0]; pz = p3[2]; py = p3[1]; }

    /* autofix8: TOD_AP_HERODUMP=<file> [TOD_AP_HERODUMP_S=0.2]: binary dump of ent/obj memory for offline RE of a grounded/floor field */
    { static FILE* hf = nullptr; static bool hinit = false; static double hnext = 0, hper = 0.2;
      if (!hinit) { hinit = true; if (const char* hp = getenv("TOD_AP_HERODUMP")) { hf = fopen(hp, "wb"); if (const char* hs = getenv("TOD_AP_HERODUMP_S")) hper = atof(hs); fprintf(stderr, "[autopilot] hero dump -> %s (%s)\n", hp, hf ? "ok" : "OPEN FAILED"); } }
      if (hf && t >= hnext) { hnext = t + hper; const uint32_t lab = (s_air ? 1u : 0u) | (s_drop > 0.6f ? 2u : 0u) | (s_cmd_moving ? 4u : 0u) | (s_mode ? 8u : 0u);
          tod_cheat_hero_dump(hf, t, lab); static int hn = 0; if (++hn % 25 == 0) fflush(hf); } }

    /* ---- grounded / airborne tracker (every 0.25 s): safe positions, visited grid, fall recovery by warping back ---- */
    if (!s_surveying) {
        const double nowt = t;
        if (s_samp_t < 0) { s_samp_t = nowt; s_samp_x = p3[0]; s_samp_y = p3[1]; s_samp_z = p3[2]; s_ground_y = p3[1]; }
        else if (nowt - s_samp_t >= 0.25) {
            const float dtm = (float)(nowt - s_samp_t), vy = (p3[1] - s_samp_y) / dtm, hd = hypotf(p3[0] - s_samp_x, p3[2] - s_samp_z);
            if (hd > 40.f || fabsf(p3[1] - s_samp_y) > 40.f) { s_safe.clear(); s_air = false; s_air_t0 = -1; s_ground_y = p3[1]; }
            else if (vy > -4.f) {
                if (s_air && !s_safe.empty() && s_ground_y - p3[1] >= 4.f) {   /* autofix8: LANDED after a descent (no warp back: the game's floor below is real): edge marked, new floor becomes the reference */
                    const SafePt& l = s_safe.back(); s_descents++;
                    if (hypotf(l.x - s_lf_x, l.z - s_lf_z) < 20.f && nowt - s_lf_t < 45.0) s_fall_rep++; else s_fall_rep = 0;
                    s_lf_x = l.x; s_lf_z = l.z; s_lf_t = nowt;
                    for (int j = 0; j <= 3; j++) mark_bad(l.x + s_gdx * (3.f + 5.f * j), l.z + s_gdz * (3.f + 5.f * j), 200); mark_bad(l.x, l.z, 100);
                    for (int k = -2; k <= 2; k++) if (k) for (int j = 0; j <= 1; j++) mark_bad(l.x + s_gdx * (3.f + 5.f * j) - s_gdz * 5.f * k, l.z + s_gdz * (3.f + 5.f * j) + s_gdx * 5.f * k, 150);
                    fprintf(stderr, "[autopilot] t=%.1f DESCENT landed drop=%.1f from edge (%.1f,%.1f,%.1f) to (%.1f,%.1f,%.1f) #%d rep=%d (no warp back, edge cells marked)\n", nowt, s_ground_y - p3[1], l.x, l.y, l.z, p3[0], p3[1], p3[2], s_descents, s_fall_rep);
                }
                s_ground_y = p3[1]; s_air = false; s_air_t0 = -1;
                if (hd > 0.3f) { s_gdx = (p3[0] - s_samp_x) / hd; s_gdz = (p3[2] - s_samp_z) / hd; }
                const bool flat = vy > -2.5f && vy < 2.5f; const bool okpt = flat && s_prev_flat; s_prev_flat = flat;   /* never record jump arcs / slopes as safe */
                if (okpt && nowt - s_safe_t >= 0.5) {
                    s_safe_t = nowt;
                    if (s_safe.empty() || hypotf(s_safe.back().x - p3[0], s_safe.back().z - p3[2]) > 1.0f) {
                        s_safe.push_back({p3[0], p3[1], p3[2], nowt});
                        if (s_safe.size() > 80) s_safe.erase(s_safe.begin(), s_safe.begin() + 20);
                    }
                }
                add_visit(p3[0], p3[2], 1);
            } else { s_prev_flat = false; if (!s_air) { s_air = true; s_air_t0 = s_samp_t; } }
            s_samp_t = nowt; s_samp_x = p3[0]; s_samp_y = p3[1]; s_samp_z = p3[2];
        }
        s_drop = s_air ? s_ground_y - p3[1] : 0.f;
        if (s_air && !s_safe.empty()) {   /* the recovery point: ~1 s of walking before the edge */
            const SafePt* sp = &s_safe.front();
            { const SafePt& ed = s_safe.back();   /* the last grounded spot = the edge */
              const float need = 8.f + 8.f * (float)s_fall_rep; bool got = false;
              for (int pass = 0; pass < 3 && !got; pass++) {
                  const float dmin = pass == 0 ? need : pass == 1 ? need * 0.5f : 0.f; const double tmin = pass < 2 ? 2.5 : 1.5;
                  for (auto& q : s_safe) { const Cell* c = cell_find(q.x, q.z);
                      if (q.t <= s_air_t0 - tmin && (pass == 2 || (c && c->v >= 3 && c->bad < 100)) && hypotf(q.x - ed.x, q.z - ed.z) >= dmin) { sp = &q; got = true; } }
              }
              if (s_fall_rep >= 3) sp = &s_safe.front(); }
            s_rec_x = sp->x; s_rec_z = sp->z;
            if (s_drop > s_wdrop && s_safewarp && nowt - s_fall_warp_t > 3.0) {
                char b[96]; snprintf(b, sizeof b, "warp=%.3f:%.3f:%.3f", sp->x, sp->y + 0.8f, sp->z);
                tod_cheat_enqueue(b);
                s_last_warp_ms = (double)GetTickCount64(); s_fall_warp_t = nowt; s_safe_warps++; s_falls++;
                fprintf(stderr, "[autopilot] t=%.1f FALL drop=%.1f at (%.1f,%.1f,%.1f): SAFE-WARP back to (%.1f,%.1f,%.1f) #%d, edge cells marked\n", nowt, s_drop, p3[0], p3[1], p3[2], sp->x, sp->y, sp->z, s_safe_warps);
                { const SafePt& l = s_safe.back();
                  if (hypotf(l.x - s_lf_x, l.z - s_lf_z) < 20.f && nowt - s_lf_t < 45.0) s_fall_rep++; else s_fall_rep = 0;
                  s_lf_x = l.x; s_lf_z = l.z; s_lf_t = nowt;
                  if (s_fall_rep >= 2 && s_roam_on && !s_free) { s_roam_end = nowt; fprintf(stderr, "[autopilot] t=%.1f repeated falls at the same edge x%d: ending this roam step, next warp\n", nowt, s_fall_rep + 1); }   /* a void-bordered island: leave via the next checkpoint warp */
                  s_away_psi = atan2f(sp->z - l.z, sp->x - l.x); s_away_until = nowt + 5.0 + 3.0 * s_fall_rep;
                  for (int j = 0; j <= 3; j++) mark_bad(l.x + s_gdx * (3.f + 5.f * j), l.z + s_gdz * (3.f + 5.f * j), 200); mark_bad(l.x, l.z, 100);
                  for (int k = -2; k <= 2; k++) if (k) for (int j = 0; j <= 1; j++) mark_bad(l.x + s_gdx * (3.f + 5.f * j) - s_gdz * 5.f * k, l.z + s_gdz * (3.f + 5.f * j) + s_gdx * 5.f * k, 150);   /* the edge is wider than one cell */
          mark_bad(s_samp_x, s_samp_z, 200); }
                s_air = false; s_air_t0 = -1; s_ground_y = sp->y; s_drop = 0; s_samp_t = -1;
                s_idle_until = (std::max)(s_idle_until, nowt + 2.0); s_have_last = false; s_roam_next = 0; s_mode = 0; s_frz_t0 = -1;
            }
        }
    }

    /* ---- edge reflex (option 3): react to the FIRST frames of an unplanned descent (vy < -4 over ~0.15 s, well below the last
       grounded height), not after the 0.25 s sampler: stop forward, steer to the newest safe point, mark the edge bad now ---- */
    if (!s_surveying && s_reflex_on) {
        static float ry[8], rx[8], rz[8]; static double rt[8]; static int rn = 0;
        const int ri = rn++ & 7; rt[ri] = t; ry[ri] = p3[1]; rx[ri] = p3[0]; rz[ri] = p3[2];
        if (rn >= 8) {
            int bj = -1; for (int j = 0; j < 8; j++) { if (j == ri) continue; const double dtj = t - rt[j]; if (dtj >= 0.12 && dtj <= 0.5 && (bj < 0 || rt[j] > rt[bj])) bj = j; }
            if (bj >= 0) {
                const float v = (p3[1] - ry[bj]) / (float)(t - rt[bj]);
                const bool same = hypotf(p3[0] - rx[bj], p3[2] - rz[bj]) < 12.f && fabsf(p3[1] - ry[bj]) < 30.f;
                if (same && v < -4.f && p3[1] < s_ground_y - 0.6f && t - s_reflex_t > 1.5 && !s_safe.empty()) {
                    s_reflex_t = t; s_reflex_until = t + 0.9; s_reflex_n++;
                    const SafePt& l = s_safe.back(); s_rec_x = l.x; s_rec_z = l.z;
                    mark_bad(p3[0], p3[2], 60); for (int j = 1; j <= 2; j++) mark_bad(p3[0] + s_gdx * 4.f * j, p3[2] + s_gdz * 4.f * j, 60);   /* soft: a real fall (drop>12) upgrades to 200 */
                    fprintf(stderr, "[autopilot] t=%.1f EDGE-REFLEX vy=%.1f drop=%.1f at (%.1f,%.1f,%.1f): back toward (%.1f,%.1f,%.1f) #%d\n", t, v, s_ground_y - p3[1], p3[0], p3[1], p3[2], l.x, l.y, l.z, s_reflex_n);
                }
            }
        }
    }
    /* periodic position log + summary */
    { static double lp = -9, ls = 0;
      if (t - lp >= 2.0) { lp = t; fprintf(stderr, "[autopilot] t=%.1f pos=(%.1f,%.1f,%.1f) step=%zu/%zu frame=%u theta=%.2f sgn=%.0f calib=%d mode=%d\n",
                                           t, p3[0], p3[1], p3[2], s_cur, s_path.size(), g_rsx_engine_frame, s_theta, s_sgn, s_calib, s_mode); }
      if (!s_surveying && s_started && t - ls >= 30.0) { ls = t; summary(t, "periodic"); } }

    /* ---------- survey mode ---------- */
    if (s_surveying) {
        if (s_survey_i >= s_survey.size()) { static bool d = false; if (!d) { d = true; fprintf(stderr, "[autopilot] SURVEY done\n"); } return 0; }
        if (s_survey_t0 < 0) { s_survey_t0 = t; s_survey_sent = false; }
        if (!s_survey_sent) { lua_warp(s_survey[s_survey_i].name); s_survey_sent = true; s_survey_t0 = t; return 0; }
        if (!tod_cheat_lua_idle()) { s_survey_t0 = t; return 0; }   /* the warp has not run yet */
        if (t - s_survey_t0 >= 3.0) {
            fprintf(stderr, "[autopilot] survey %s %.2f %.2f %.2f\n", s_survey[s_survey_i].name.c_str(), p3[0], p3[1], p3[2]);
            s_survey_i++; s_survey_t0 = -1;
        }
        return 0;
    }
    if (s_path.empty() || s_done) return 0;

    if (!s_started) { s_started = true; s_start_t = t; fprintf(stderr, "[autopilot] t=%.1f START\n", t); }
    if (s_duration > 0 && t - s_start_t > s_duration && !s_done) { s_done = true; fprintf(stderr, "[autopilot] t=%.1f DURATION %.0f s over\n", t, s_duration); summary(t, "duration"); return 0; }
    if (s_ammo_s > 0 && t - s_ammo_t >= s_ammo_s) { s_ammo_t = t; tod_cheat_lua("hero_give_all_ammo(Ratchet1)"); }
    /* uni127 TOD_AP_WEAPON_CYCLE=WPN_A,WPN_B,..[@seconds] (default off, 20 s): equip the next weapon in turn, for heavier fights
     * (more kinds of shots and explosions on screen) in load tests. A name the game does not know just does nothing. */
    { static std::vector<std::string> wl; static double wsec = 20, wnext = -1; static size_t wi = 0; static int winit = 0;
      if (!winit) { winit = 1; if (const char* c = getenv("TOD_AP_WEAPON_CYCLE")) { std::string v = c; size_t at = v.find('@');
          if (at != std::string::npos) { wsec = atof(v.c_str() + at + 1); v = v.substr(0, at); } if (wsec < 3) wsec = 3;
          for (size_t a = 0; a <= v.size();) { size_t b = v.find(',', a); if (b == std::string::npos) b = v.size(); if (b > a) wl.push_back(v.substr(a, b - a)); a = b + 1; } } }
      /* rissa127: never in the 4 s after a warp. The first equip used to go out in the same frame as the START warp; in ~1 run of 3
       * Ratchet then stayed motionless for 60-100 s (input given, position frozen: fit1, tlA2, tlB2, ring2d, poolph, lua1). */
      if (!wl.empty() && t >= wnext && tod_cheat_lua_idle() && (double)GetTickCount64() - s_last_warp_ms > 4000.0 && t >= s_idle_until) {
          wnext = t + wsec; const std::string& w = wl[wi++ % wl.size()];
          std::string c = "hero_equip(Ratchet1," + w + ")"; tod_cheat_lua(c.c_str());
          fprintf(stderr, "[autopilot] t=%.1f weapon cycle: equip %s\n", t, w.c_str());
      } }
    if (!s_weapon_done && !s_weapon.empty()) {
        s_weapon_done = true;
        std::string c = "hero_equip(Ratchet1," + s_weapon + ")"; tod_cheat_lua(c.c_str());
        fprintf(stderr, "[autopilot] equip %s\n", s_weapon.c_str());
    }
    /* uni127: only a queued WARP stops the input (it moves Ratchet); ammo refills (every 4 s) and equips used to stop it 2.5 s each:
     * Ratchet stood still ~60 % of the time in every run */
    if (!tod_cheat_lua_idle() && (double)GetTickCount64() - s_last_warp_ms < 5000.0) { s_have_last = false; s_frz_t0 = -1; if (s_idle_until < t + 2.5) s_idle_until = t + 2.5; return 0; }   /* a warp / Lua chunk is queued: wait for it */
    if (t < s_idle_until) { s_have_last = false; s_frz_t0 = -1; return 0; }   /* cutscene / warp landing: no input */

    Step& st = s_path[s_cur];
    if (s_step_t0 < 0) { s_step_t0 = t; s_prog_t = t; s_best = 1e9f; s_step_f0 = g_rsx_engine_frame; s_rng = 12345u + (unsigned)s_cur * 7919u; }
    if (!st.lua.empty()) { tod_cheat_lua(st.lua.c_str()); fprintf(stderr, "[autopilot] t=%.1f lua step: %s\n", t, st.lua.c_str()); next_step(t, true); return 0; }
    if (s_roam_on && t >= s_roam_end && !s_brawl) {   /* uni127 brawl: stay in this area, no warp to the next step */ s_roam_on = false; next_step(t, false); return 0; }
    if (st.warp_only && !s_roam_on) { do_warp(t, "warp-only step"); if (!s_roam_on) return 0; return 0; }

    /* frozen watchdog: the position has not changed for 25 s while we are driving (cutscene / script lock / wedged) */
    if (s_frz_t0 < 0 || hypotf(gx - s_frz_x, gy - s_frz_y) > 0.6f) { s_frz_t0 = t; s_frz_x = gx; s_frz_y = gy; }
    { static double skt = -99; if (t - s_frz_t0 > 5.0 && t - skt > 4.0) { skt = t;   /* motionless 5 s: dialogue/scene lock? try to release it from Lua (each chunk alone: an unknown native only fails itself) */
        tod_cheat_lua("hero_lockout_control(Ratchet1,false)"); tod_cheat_lua("stop_dialog()"); tod_cheat_lua("hero_end_scene(Ratchet1)");
        fprintf(stderr, "[autopilot] t=%.1f motionless %.1f s: scene/dialog release attempt (lockout off, stop_dialog, end_scene)\n", t, t - s_frz_t0); } }
    if (s_pk_t0 < 0 || hypotf(gx - s_pk_x, gy - s_pk_y) > 5.f) { s_pk_t0 = t; s_pk_x = gx; s_pk_y = gy; }
    if ((t - s_frz_t0 > 25.0 && !(s_brawl && s_tgt)) || (!s_brawl && t - s_pk_t0 > 10.0)) {   /* uni127 brawl: circling an enemy is the point, not a pocket */   /* wedged still (25 s) or circling inside a 5 u pocket (14 s) */
        s_frz_t0 = t; s_pk_t0 = t; s_pk_x = gx; s_pk_y = gy; s_frozen++;
        s_frz_consec = (t - s_frz_last < 70.0) ? s_frz_consec + 1 : 1; s_frz_last = t; { static float lx = 1e9f, ly; static double lt = -99; if (hypotf(gx - lx, gy - ly) < 15.f && t - lt < 70.0 && s_frz_consec < 3) s_frz_consec = 3; lx = gx; ly = gy; lt = t; }   /* same place again: skip straight to explore+warp */
        fprintf(stderr, "[autopilot] t=%.1f FROZEN/POCKET at (%.1f,%.1f) (#%d, consecutive %d): escalating unstick\n", t, gx, gy, s_frozen, s_frz_consec);
        s_frz_until = t + 2.5; s_roam_next = 0; s_prog_t = t - 2.0; s_side = -s_side;
        mark_bad(gx, gy, 60);
        if (s_frz_consec == 1) { s_mode = 2; s_mode_until = t + 2.0; }
        else if (s_frz_consec == 2) { s_mode = 3; s_mode_until = t + 2.0; }
        else {
            s_mode = 2; s_mode_until = t + 2.5; s_explore_t0 = t; s_explore_until = t + 12; s_roam_ang = frand() * 6.2831853f;
            if (s_safewarp && s_safe.size() > 4 && !s_brawl) {   /* wedged for good: warp to an older safe spot (up to ~20 s back) */
                const SafePt& q = s_safe[s_safe.size() / 3]; char b[96]; snprintf(b, sizeof b, "warp=%.3f:%.3f:%.3f", q.x, q.y + 0.8f, q.z);
                tod_cheat_enqueue(b); s_last_warp_ms = (double)GetTickCount64(); s_idle_until = t + 2.0; s_safe_warps++;
                fprintf(stderr, "[autopilot] t=%.1f FROZEN x%d: warp to older safe point (%.1f,%.1f,%.1f)\n", t, s_frz_consec, q.x, q.y, q.z);
            }
        }
    }
    float wx = st.x, wy = s_xz ? st.z : st.y;
    float rad = st.r > 0 ? st.r : (float)s_defr;
    if (s_free) {
        if (s_explore_t0 < 0) s_explore_t0 = t;
        { static double lt = -1; const double dtt = lt < 0 ? 0.0 : (std::min)(t - lt, 0.5); lt = t;   /* fight budget: >25 s of fighting in one place -> leave */
          if (s_tgt && s_tgt_d < s_enemy_range) s_fight_acc += dtt; else s_fight_acc = (std::max)(0.0, s_fight_acc - dtt * 0.5);
          if (!s_brawl && s_fight_acc > 45.0 && t >= s_explore_until) {
              s_fight_acc = 0; s_explore_t0 = t; s_explore_until = t + 10; s_roam_next = 0; s_tgt = 0;
              for (int i = -1; i <= 1; i++) for (int j = -1; j <= 1; j++) add_visit(gx + 6.f * i, gy + 6.f * j, 40);
              fprintf(stderr, "[autopilot] t=%.1f FIGHT BUDGET: leaving the area at (%.1f,%.1f) for 10 s (grid marked)\n", t, gx, gy);
          } }
        if (!s_brawl && t - s_explore_t0 > s_explore_every && t >= s_explore_until) {   /* leave the fight: walk on in a straight-ish line for a while */
            s_explore_t0 = t; s_explore_until = t + s_explore_len; s_roam_next = 0; s_tgt = 0; s_roam_ang = pick_heading(gx, gy, s_roam_ang);
            fprintf(stderr, "[autopilot] t=%.1f EXPLORE %.0f s (heading %.2f)\n", t, s_explore_len, s_roam_ang);
        }
    }
    if (s_roam_on) {   /* wander: a new random point 10-25 units from the landing point every 6 s or when reached */
        if (!s_roam_have_c) { s_roam_cx = gx; s_roam_cy = gy; s_roam_have_c = true; s_roam_next = 0; }
        if (t >= s_roam_next || hypotf(s_roam_tx - gx, s_roam_ty - gy) < 3.f) {
            float ex = 0, ez = 0;
            if (s_free) {   /* free level mode: go toward the nearest enemy (70 units), else keep a heading with random turns */
                s_roam_cx = gx; s_roam_cy = gy;
                const bool expl = t < s_explore_until;
                if (s_stuck_events_free) { mark_bad(gx + 5.f * cosf(s_roam_ang), gy + 5.f * sinf(s_roam_ang), 20); s_stuck_events_free = 0; }   /* a wall ahead */
                s_roam_ang = pick_heading(gx, gy, s_roam_ang);
                if (!expl && nearest_enemy(gx, gy, 70.f, &ex, &ez)) { s_roam_tx = ex; s_roam_ty = ez; }
                else { const float r = (expl ? 40.f : 30.f); s_roam_tx = gx + r * cosf(s_roam_ang); s_roam_ty = gy + r * sinf(s_roam_ang); }
                s_roam_next = t + 5.0; s_best = 1e9f; s_prog_t = t;
            } else {
                /* path roam (warp + roam): same frontier heading as the free mode, 22 u ahead, escapes corridors instead of circling */
                if (s_stuck_events_free) { mark_bad(gx + 5.f * cosf(s_roam_ang), gy + 5.f * sinf(s_roam_ang), 20); s_stuck_events_free = 0; }
                s_roam_ang = pick_heading(gx, gy, s_roam_ang);
                s_roam_tx = gx + 22.f * cosf(s_roam_ang); s_roam_ty = gy + 22.f * sinf(s_roam_ang); s_roam_next = t + 5.0; s_best = 1e9f; s_prog_t = t;
            }
        }
        wx = s_roam_tx; wy = s_roam_ty; rad = 3.f;
    }
    const float dx = wx - gx, dy = wy - gy, dist = sqrtf(dx * dx + dy * dy);
    const float dz = s_xz ? fabsf(st.y - p3[1]) : fabsf(st.z - p3[2]);   /* vertical axis */
    if (!s_roam_on && dist < rad && dz < 15.f) { next_step(t, true); return 1; }

    /* progress / stuck bookkeeping */
    if (dist < s_best - 1.0f) { s_best = dist; s_prog_t = t; }
    if (s_have_last) { float mx = gx - s_lastx, my = gy - s_lasty, m = sqrtf(mx * mx + my * my); if (m < 6.f) s_walk_dist += m; }
    s_lastx = gx; s_lasty = gy; s_have_last = true;
    const double since = t - s_prog_t;
    if (since > s_stuck_s && !st.nowarp && !s_roam_on) { s_stuck_events++; do_warp(t, "stuck"); return 0; }
    if (t - s_step_t0 > s_step_max && !st.nowarp && !s_roam_on) { s_stuck_events++; do_warp(t, "step timeout"); return 0; }

    /* ---------- estimate camera yaw / handedness from velocity ---------- */
    if (s_st < 0) { s_sx = gx; s_sy = gy; s_st = t; }
    float vspeed = 0, vang = 0; bool vvalid = false;
    if (t - s_st >= 0.2) {
        const float vx = gx - s_sx, vy = gy - s_sy, d = sqrtf(vx * vx + vy * vy), dt = (float)(t - s_st);
        vspeed = d / dt; s_vspeed = vspeed;
        if (d > 8.f) { s_cmd_moving = false; }   /* teleport */
        else if (vspeed > 2.0f && s_cmd_moving) {
            vvalid = true; vang = atan2f(vy, vx);
            float m[6];
            if (s_xz && tod_cheat_player_mat(m) && fabsf(m[4]) < 0.3f && isfinite(m[0]) && isfinite(m[1])) {   /* facing offset = heading of motion minus row0 angle */
                const float o = wrap(vang - atan2f(m[1], m[0]));
                s_off_s = 0.85f * s_off_s + sinf(o); s_off_c = 0.85f * s_off_c + cosf(o); s_off_n++;
            }
        }
        s_sx = gx; s_sy = gy; s_st = t;
    }

    float phi = 0, mag = 1.f; bool jump = false;
    if (s_calib < 2) {
        /* probe 0: stick forward (phi = +pi/2) ; probe 1: stick right (phi = 0). Handedness and yaw from the two velocity directions. */
        if (s_calib_t0 < 0) { s_calib_t0 = t; s_st = -1; s_cmd_moving = false; }
        phi = s_calib == 0 ? 1.5707963f : 0.f;
        s_cmd_moving = true; s_lastphi = phi;
        static float acc_x, acc_y; static double acc_t0 = -1; static float ox, oy, oyh;
        if (acc_t0 < 0) { acc_t0 = t; ox = gx; oy = gy; oyh = p3[1]; }
        if (t - acc_t0 >= 1.4) {
            const float vx = gx - ox, vy = gy - oy, d = sqrtf(vx * vx + vy * vy);
            if (d > 3.f && d < 40.f && fabsf(p3[1] - oyh) < 1.0f) {   /* flat ground only (a fall spoils the heading) */
                const float ang = atan2f(vy, vx);
                if (s_calib == 0) {
                    s_va = ang; s_calib = 1; fprintf(stderr, "[autopilot] calib forward: moved %.1f heading %.2f\n", d, ang);
                    if (!s_probe_right) { s_theta = wrap(ang - s_sgn * 1.5707963f); s_calib = 2; fprintf(stderr, "[autopilot] calib: sgn=%.0f fixed, theta=%.2f (forward probe only)\n", s_sgn, s_theta); }
                }
                else {
                    const float dd = wrap(ang - s_va);   /* -pi/2 right-handed, +pi/2 mirrored */
                    s_sgn = dd < 0 ? 1.f : -1.f; s_theta = ang; s_calib = 2;
                    fprintf(stderr, "[autopilot] calib right: moved %.1f heading %.2f (delta %.2f) -> sgn=%.0f theta=%.2f\n", d, ang, dd, s_sgn, s_theta);
                }
            } else fprintf(stderr, "[autopilot] calib probe %d moved only %.1f: retry\n", s_calib, d);
            acc_t0 = -1; (void)acc_x; (void)acc_y;
            if (s_calib < 2) { s_calib_t0 = -1; }
        }
        out->lx = (uint8_t)(128.f + 127.f * cosf(phi));
        out->ly = (uint8_t)(128.f - 127.f * sinf(phi));
        return 1;
    }
    if (fabsf(wrap(s_lastphi - s_phi_ref)) > 0.25f) { s_phi_ref = s_lastphi; s_phi_t = t; }
    if (vvalid && vspeed > 2.0f && t - s_phi_t > 0.7) {   /* only while the stick has been steady: Ratchet turns slowly and the camera follows */
        const float meas = wrap(vang - s_sgn * s_lastphi);
        s_theta = wrap(s_theta + 0.12f * wrap(meas - s_theta));
    }

    /* ---------- combat: nearest enemy -> face it, fire only when facing it ---------- */
    if (t - s_scan_t >= 0.2) { s_scan_t = t; scan_ents(t, gx, gy); }
    float facing = 0; bool have_facing = false;
    if (s_xz && s_off_n >= 4) { float m[6]; if (tod_cheat_player_mat(m) && fabsf(m[4]) < 0.5f) { facing = atan2f(m[1], m[0]) + atan2f(s_off_s, s_off_c); have_facing = true; } }
    const bool recov = ((s_air && s_drop > 3.f) || t < s_reflex_until) && !s_safe.empty();   /* falling over an edge: steer back to the safe point */
    bool combat = s_tgt != 0 && s_tgt_d < s_enemy_range && (s_brawl || t >= s_explore_until) && t >= s_frz_until && !recov && t >= s_away_until;
    float epsi = 0, edist = 0;
    if (combat) { epsi = atan2f(s_tgt_z - gy, s_tgt_x - gx); edist = hypotf(s_tgt_x - gx, s_tgt_z - gy); }

    /* stuck recovery ladder (no progress while the stick is pushed) */
    float psi = atan2f(dy, dx);
    if (combat) {
        s_prog_t = t;   /* the fight does not count as "stuck" for the waypoint */
        s_mode = 0;
        psi = epsi;
    } else if (s_mode != 0 && t < s_mode_until) {
        if (s_mode == 1) jump = true;
        else if (s_mode == 2) { psi = wrap(psi + s_side * 1.5707963f); jump = true; }
        else if (s_mode == 3) { psi = wrap(psi + 3.1415926f + s_side * 0.6f); }
    } else {
        s_mode = 0;
        if (since > 1.2 && t - s_last_escape > 0.1) {
            const int k = (int)((since - 1.2) / 1.6) % 4;
            s_last_escape = t; if (s_free || s_roam_on) s_stuck_events_free = 1;
            s_mode = k == 0 ? 1 : k == 1 ? 2 : k == 2 ? 1 : 3;
            s_side = (k == 1) ? s_side : -s_side;
            s_mode_until = t + (s_mode == 3 ? 1.0 : 0.9);
            if (s_mode != 1) fprintf(stderr, "[autopilot] t=%.1f no progress %.1f s at d=%.1f: %s\n", t, since, dist, s_mode == 2 ? "sidestep+jump" : "reverse");
        }
    }
    if (recov) { psi = atan2f(s_rec_z - gy, s_rec_x - gx); s_mode = 0; s_prog_t = t; jump = false; }
    else if (t < s_away_until) { psi = s_away_psi; s_mode = 0; s_prog_t = t; jump = false; }   /* after a recovery warp: walk away from the edge for 5+ s */
    else {   /* learned fall edge ahead (bad >= 100): turn away before walking off it */
        const Cell* c0 = cell_find(gx + 5.f * cosf(psi), gy + 5.f * sinf(psi)); const Cell* c1 = cell_find(gx + 9.f * cosf(psi), gy + 9.f * sinf(psi));
        if ((c0 && c0->bad >= 100) || (c1 && c1->bad >= 100)) {
            static const float off[4] = {1.0472f, -1.0472f, 2.0944f, -2.0944f};
            for (int i = 0; i < 4; i++) { const float a = wrap(psi + off[i]); const Cell* d0 = cell_find(gx + 5.f * cosf(a), gy + 5.f * sinf(a)); const Cell* d1 = cell_find(gx + 9.f * cosf(a), gy + 9.f * sinf(a));
                if (!(d0 && d0->bad >= 100) && !(d1 && d1->bad >= 100)) { psi = a; break; } }
        }
    }
    mag = (dist > 3 * rad || s_mode != 0 || recov) ? 1.0f : 0.7f + 0.3f * dist / (3 * rad);
    bool l1 = false;
    if (combat && s_brawl) {
        /* uni127 TOD_AP_BRAWL: fight like a player -- run at the enemy, then circle it sideways (L1) at full speed, switching side
         * every 1-2.5 s, with dodge jumps; fire is continuous (below). For load tests with lots of shots and explosions. */
        static double side_t = 0, side_len = 1.5, jump_t = 0;
        if (edist > 7.f) { psi = epsi; mag = 1.f; }
        else {
            if (t - side_t > side_len) { side_t = t; side_len = 1.0 + 1.5 * frand(); s_strafe_dir = -s_strafe_dir; }
            psi = wrap(epsi + s_strafe_dir * 1.5707963f); mag = 1.f; l1 = true;   /* l1 = the strafe button: L2 in ToD (below) */
        }
        if (t - jump_t > 1.2 && frand() < 0.03f) { jump_t = t; jump = true; }
    }
    else if (combat) {
        if (edist > s_engage) mag = 1.f;
        else if (s_strafe) {   /* circle the target: stick perpendicular to it, L1 held (strafe) */
            if (t - s_strafe_t > 2.0) { s_strafe_t = t; s_strafe_dir = -s_strafe_dir; }
            psi = wrap(epsi + s_strafe_dir * 1.5707963f); mag = 0.8f; l1 = true;
        }
        else mag = 0.6f;      /* close: creep toward it while turning */
    }
    phi = wrap(s_sgn * (psi - s_theta));
    s_lastphi = phi; s_cmd_moving = mag > 0.05f;
    out->lx = (uint8_t)(128.f + 127.f * cosf(phi) * mag);
    out->ly = (uint8_t)(128.f - 127.f * sinf(phi) * mag);
    if (l1) out->buttons |= CELL_PAD_CTRL_L2;   /* uni127: ToD strafes with L2 (user 06/11), was L1 */

    /* ---------- jump: when blocked (slow with the stick pushed) and now and then at random ---------- */
    if (!combat && mag > 0.5f && s_vspeed < 1.2f && s_step_t0 >= 0 && t - s_step_t0 > 1.5) {
        if (s_slow_t0 < 0) s_slow_t0 = t;
        if (t - s_slow_t0 > 0.5) { const uint64_t ms2 = (uint64_t)(t * 1000.0); if (ms2 % 500 < 200) jump = true; }
    } else s_slow_t0 = -1;
    if (recov) { s_jump_until = 0; jump = false; }
    else if (t >= s_jump_next && t >= s_away_until) { s_jump_until = t + 0.3; s_jump_next = t + 3.0 + 4.0 * frand(); s_jumps_rand++; }
    if (t < s_jump_until) jump = true;
    if (jump) out->buttons |= CELL_PAD_CTRL_CROSS;
    if (s_frz_t0 >= 0 && t - s_frz_t0 > 10.0 && fmod(t, 1.0) < 0.15) out->buttons |= (CELL_PAD_CTRL_CROSS | 0x2000);   /* motionless 10 s: maybe a dialogue/story scene: pulse X+O to skip it */

    /* ---------- camera: right stick ---------- */
    if (s_cam) {
        const float camfwd = s_theta + s_sgn * 1.5707963f;   /* world heading the camera looks along */
        float rxv = 0;
        if (combat && edist > 2.f && s_camtgt) {
            const float err = wrap(epsi - camfwd);
            if (fabsf(err) > 0.45f) {
                if (s_cam_t0 < 0) { s_cam_t0 = t; s_cam_err0 = fabsf(err); }
                else if (t - s_cam_t0 > 1.6) {
                    if (fabsf(err) > s_cam_err0 + 0.2f) { s_camsgn = -s_camsgn; s_camflips++; fprintf(stderr, "[autopilot] t=%.1f camera sign flipped -> %.0f (err %.2f -> %.2f)\n", t, s_camsgn, s_cam_err0, fabsf(err)); }
                    s_cam_t0 = t; s_cam_err0 = fabsf(err);
                }
                rxv = s_camsgn * (err > 0 ? 1.f : -1.f) * (fabsf(err) > 1.0f ? 1.f : 0.6f);
            } else s_cam_t0 = -1;
        } else {
            s_cam_t0 = -1;
            if (t >= s_cam_next) { s_cam_until = t + 1.0 + 0.6 * frand(); s_cam_next = s_cam_until + 5.0 + 4.0 * frand(); s_cam_dir = frand() < 0.5f ? -1.f : 1.f; }
            if (t < s_cam_until) rxv = 0.7f * s_cam_dir;
        }
        if (rxv != 0.f) out->rx = (uint8_t)(128.f + 120.f * rxv);
    }

    /* ---------- fire: at the target when facing it; the old blind burst only with TOD_AP_BLINDFIRE ---------- */
    const uint64_t ms = (uint64_t)(t * 1000.0);
    if (s_fire_ms > 0) {
        bool fire = false;
        if (combat && edist < s_fire_range) {
            const bool aligned = !have_facing || fabsf(wrap(epsi - facing)) < 0.4f || edist < 4.f;
            fire = (aligned || s_brawl) && ms % (uint64_t)s_fire_ms < 200;
        } else if (!combat && s_blind_ms > 0) fire = ms % (uint64_t)s_blind_ms < 200;
        if (fire) { out->buttons |= CELL_PAD_CTRL_R1; s_fire_total += 0.016; }
    }
    if (s_melee_ms > 0 && combat && edist < 3.5f && ms % (uint64_t)s_melee_ms < 110) out->buttons |= CELL_PAD_CTRL_SQUARE;
    return 1;
}
