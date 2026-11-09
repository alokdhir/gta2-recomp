/* Game controllers (GameController framework) mapped onto GTA2's default
 * keyboard bindings, so they work everywhere the keyboard does: menus,
 * on foot and driving. Defaults from gta2.exe's built-in table: Up, Down,
 * Left, Right, LCtrl attack, Return enter/exit, Space jump/handbrake,
 * Z/X weapons, Tab, LAlt, RShift specials. Polled once per message pump. */
#import <GameController/GameController.h>

#include "input/gamepad.h"
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

void gamepad_poll(void)
{
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

    const float t = 0.5f;
    int want[K_COUNT] = { 0 };
    want[K_UP] = g.leftThumbstick.yAxis.value > t || g.dpad.up.pressed || g.rightTrigger.value > 0.3f;
    want[K_DOWN] = g.leftThumbstick.yAxis.value < -t || g.dpad.down.pressed || g.leftTrigger.value > 0.3f;
    want[K_LEFT] = g.leftThumbstick.xAxis.value < -t || g.dpad.left.pressed;
    want[K_RIGHT] = g.leftThumbstick.xAxis.value > t || g.dpad.right.pressed;
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
