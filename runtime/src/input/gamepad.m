/* Game controllers (GameController framework) mapped onto GTA2's default
 * keyboard bindings, so they work everywhere the keyboard does: menus,
 * on foot and driving. Defaults from gta2.exe's built-in table: Up, Down,
 * Left, Right, LCtrl attack, Return enter/exit, Space jump/handbrake,
 * Z/X weapons, Tab, LAlt, RShift specials. Polled once per message pump. */
#import <GameController/GameController.h>

#include "input/gamepad.h"
#include "input/player.h"
#include "guest_memory.h"
#include "winapi/win32.h"
#include <math.h>
#include "winapi/imports.h"
#include "winapi/user32_api.h"

/* macOS key codes of the bound keys */
enum {
    K_UP, K_DOWN, K_LEFT, K_RIGHT, K_ATTACK, K_ENTER, K_JUMP, K_PREV, K_NEXT,
    K_SPECIAL1, K_SPECIAL2, K_SPECIAL3, K_PAUSE, K_COUNT
};
static const unsigned g_mac[K_COUNT] = {
    0x7E, 0x7D, 0x7B, 0x7C, 0x3B, 0x24, 0x31, 0x06, 0x07, 0x30, 0x3A, 0x3C, 0x35,
};
static int g_held[K_COUNT];
static int g_announced;


/* Stick-direction movement on foot: the player turns to face where the left
 * stick points and walks. Tank controls (left/right turn) stay on the d-pad,
 * and in cars. Registry screen/mac_tank_controls=1 turns it off. */
static int stick_mode(void)
{
    static int m = -1;
    if (m < 0) m = !reg_get_dword("screen", "mac_tank_controls", 0);
    return m;
}

void gamepad_poll(void)
{
    /* test runs (GTA2_HEADLESS) must not pick up the controller of a game being played alongside */
    static int headless = -1;
    if (headless < 0) headless = getenv("GTA2_HEADLESS") != NULL;
    if (headless) return;
    GCController *pad = GCController.current;
    if (!pad) pad = GCController.controllers.firstObject;
    GCExtendedGamepad *g = pad.extendedGamepad;
    if (!g) {
        g_announced = 0;
        return;
    }
    if (!g_announced) {
        rt_log("gamepad: %s", pad.vendorName.UTF8String ?: "controller");
        g_announced = 1;
    }
    /* Developer combos: hold View (Back/Select/Share) and press LB = switch
     * renderer, RB = snapshot, Y = instant replay. While View is held those
     * buttons don't reach the game. */
    static int combo_prev[3];
    int view = g.buttonOptions.pressed;
    int combo[3] = { view && g.leftShoulder.pressed, view && g.rightShoulder.pressed, view && g.buttonY.pressed };
    for (int i = 0; i < 3; i++) {
        if (combo[i] && !combo_prev[i]) {
            if (i == 0) user32_action_toggle_renderer();
            if (i == 1) user32_action_snapshot();
            if (i == 2) user32_action_replay();
        }
        combo_prev[i] = combo[i];
    }
    /* PlayStation touchpad click (unused by GTA2): instant replay, easy to hit mid-drive */
    static int pad_prev;
    int pad_click = 0;
    if ([g isKindOfClass:GCDualShockGamepad.class]) pad_click = ((GCDualShockGamepad *)g).touchpadButton.pressed;
    if ([g isKindOfClass:GCDualSenseGamepad.class]) pad_click = ((GCDualSenseGamepad *)g).touchpadButton.pressed;
    if (pad_click && !pad_prev) user32_action_replay();
    pad_prev = pad_click;

    const float t = 0.5f;
    int want[K_COUNT] = { 0 };
    float sx = g.leftThumbstick.xAxis.value, sy = g.leftThumbstick.yAxis.value;
    int16_t *facing = stick_mode() && player_on_foot() ? player_facing() : NULL;
    /* log stick-mode transitions, so replays can be matched to what it did */
    {
        static int prev_state = -1;
        int state = !stick_mode() ? 0 : !player_on_foot() ? 1 : !facing ? 2 : sx * sx + sy * sy > 0.35f * 0.35f ? 4 : 3;
        static const char *names[] = { "tank controls", "in car / not on foot", "on foot, no player found",
                                       "on foot, stick at rest", "on foot, steering by stick" };
        if (state != prev_state) {
            rt_log("gamepad: %s (stick %.2f, %.2f)", names[state], sx, sy);
            prev_state = state;
        }
    }
    if (facing && sx * sx + sy * sy > 0.35f * 0.35f) {
        /* bearing anticlockwise from north, in quarter degrees */
        float deg = atan2f(-sx, sy) * 57.29578f;
        int v = (int)lroundf(720.0f + 4.0f * deg) % 1440;
        *facing = (int16_t)(v < 0 ? v + 1440 : v);
        sx = 0, sy = 1; /* and walk forward */
    } else if (facing) {
        sx = sy = 0; /* stick at rest: stand still (no stick turning on foot) */
    }
    want[K_UP] = sy > t || g.dpad.up.pressed || g.rightTrigger.value > 0.3f;
    want[K_DOWN] = sy < -t || g.dpad.down.pressed || g.leftTrigger.value > 0.3f;
    want[K_LEFT] = sx < -t || g.dpad.left.pressed;
    want[K_RIGHT] = sx > t || g.dpad.right.pressed;
    want[K_JUMP] = g.buttonA.pressed;
    want[K_ENTER] = g.buttonB.pressed;
    want[K_ATTACK] = g.buttonX.pressed;
    want[K_SPECIAL1] = g.buttonY.pressed && !view;
    want[K_PREV] = g.leftShoulder.pressed && !view;
    want[K_NEXT] = g.rightShoulder.pressed && !view;
    want[K_SPECIAL3] = g.leftThumbstickButton.pressed;
    want[K_SPECIAL2] = g.rightThumbstickButton.pressed;
    want[K_PAUSE] = g.buttonMenu.pressed;
    for (int k = 0; k < K_COUNT; k++) {
        if (want[k] != g_held[k]) {
            g_held[k] = want[k];
            user32_inject_key(g_mac[k], want[k]);
        }
    }
}
