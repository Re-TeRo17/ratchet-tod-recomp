/*
 * Keyboard / mouse bindings of port 0 (keys136): which key or mouse button is which PS3 pad input.
 *
 * Every action has a tod.ini key ("KEY_CROSS = Space"). The defaults are the layout the port always had
 * (arrows = D-pad and left stick, Z X A S = Cross Circle Square Triangle, Q W = L1 R1, 1 2 = L2 R2,
 * Enter = Start, Tab = Select), so nothing changes for anyone who never touches them. L3, R3 and the
 * camera (right stick) keys are unbound by default. The mouse camera (TOD_MOUSE) has its own bindings:
 * TOD_MOUSE_LEFT / _RIGHT / _MIDDLE / _WHEEL_UP / _WHEEL_DOWN = a pad button (R1 L1 Triangle Left Right).
 *
 * Used by src/input/host_input.cpp (the pad poll) and src/ui/tod_overlay.cpp (the "Controls" tab).
 * Values are Windows virtual-key codes (layout independent, like the old fixed table); 0 = unbound.
 */
#pragma once
#include <stdint.h>
#include <string>

namespace kb {

enum Action {
    CROSS, CIRCLE, SQUARE, TRIANGLE, L1, R1, L2, R2, L3, R3, START, SELECT,
    DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT,
    LS_UP, LS_DOWN, LS_LEFT, LS_RIGHT,       /* left stick */
    CAM_UP, CAM_DOWN, CAM_LEFT, CAM_RIGHT,   /* right stick (camera) */
    NACTIONS
};
const int kNumButtonTargets = DPAD_RIGHT + 1;   /* what a mouse button / the wheel can be: Cross .. D-pad Right */

enum MouseSlot { M_LEFT, M_RIGHT, M_MIDDLE, M_WHEEL_UP, M_WHEEL_DOWN, NMOUSE };

/* Mouse camera on when TOD_MOUSE is not set. (Kept off: it grabs the cursor, so it is opt-in until it is
 * checked on long play with a real mouse.) */
constexpr bool kMouseDefaultOn = false;

void init();                      /* once, from the main thread: reads the environment and TOD_KEY_FAKE */
const char* ini_key(int a);       /* "KEY_CROSS" */
const char* label(int a);         /* "Cross" */
int default_vk(int a);
int get(int a);                   /* the bound virtual key, 0 = none */
void set(int a, int vk);

std::string key_name(int vk);                 /* "Space", "A", "Num4", "Mouse4", "None" for 0 */
int parse_key(const char* s, bool* ok);       /* the inverse; case-insensitive; "None"/"" = 0 */

const char* mouse_ini_key(int slot);          /* "TOD_MOUSE_LEFT" */
const char* mouse_label(int slot);            /* "Left button" */
int mouse_default(int slot);                  /* an Action (< kNumButtonTargets) or -1 = none; follows TOD_MOUSE_WHEEL=off */
int mouse_get(int slot);
void mouse_set(int slot, int action);
const char* target_token(int action);         /* "Cross", "DpadLeft" ...; "None" for -1 */
int parse_target(const char* s, bool* ok);    /* -1 = None */

void reload_from_env();                       /* every KEY_* / TOD_MOUSE_* now in the environment (unset = default) */

/* One pad poll's worth: the buttons (CELL_PAD_CTRL_*) the held keys give and the stick directions
 * (bit 1 left, 2 right, 4 up, 8 down). Real keys only when the window has the focus; TOD_KEY_FAKE ones always. */
uint16_t poll(bool focused, uint8_t* ls, uint8_t* rs);
/* Adds one action's effect (the mouse bindings use it). */
void apply(int action, uint16_t* btn, uint8_t* ls, uint8_t* rs);
uint16_t button_bit(int action);              /* the CELL_PAD_CTRL_ bit of Cross .. D-pad Right, else 0 */

}  // namespace kb
