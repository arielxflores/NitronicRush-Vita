/* The race controls: each action on one of the Vita's buttons (changeable in Controls), the
 * steering and trick sticks swappable. The menus always use the d-pad / left stick, Cross and
 * Circle (and the touch screen). Defaults: R gas, L brake, Cross jump, Circle boost, Square wings,
 * Triangle view, Select reset, Start pause. */
#ifndef NR_CONTROLS_H
#define NR_CONTROLS_H
#include "hud.h"

/* ACT_TRICK: the PC's Advanced Controls (ControlScheme_TriggerPedals_RollMode): held, the steering stick does the
 * tricks (LB on the PC's pad); it may share its button with the brake (then it rolls in the air, brakes on the ground) */
enum { ACT_GAS, ACT_BRAKE, ACT_BOOST, ACT_JUMP, ACT_WINGS, ACT_VIEW, ACT_RESET, ACT_PAUSE, ACT_TRICK, ACT_N };
enum { BTN_CROSS, BTN_CIRCLE, BTN_SQUARE, BTN_TRIANGLE, BTN_L, BTN_R, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
       BTN_START, BTN_SELECT, BTN_N };

static const char *const ACT_NAME[ACT_N] = { "Gas", "Brake / Reverse", "Boost", "Jump", "Wings (in the air)",
                                            "Change View", "Reset to Checkpoint", "Pause", "Roll (Advanced Controls)" };
static const int ACT_DEFAULT[ACT_N] = { BTN_R, BTN_L, BTN_CIRCLE, BTN_CROSS, BTN_SQUARE, BTN_TRIANGLE, BTN_SELECT, BTN_START, BTN_L };
/* the PC pad's buttons by place (RT, LT, A boost, X jump, B wings, Y view): the tutorial's spoken lines name them */
static const int ACT_PADPOS[ACT_N] = { BTN_R, BTN_L, BTN_CROSS, BTN_SQUARE, BTN_CIRCLE, BTN_TRIANGLE, BTN_SELECT, BTN_START, BTN_L };
#define BIND_VERSION 2                       /* saves with an older one and the old defaults get the new defaults */
/* two actions that may be on the same button */
static inline int act_shareable(int a, int b) { return (a == ACT_TRICK && b == ACT_BRAKE) || (a == ACT_BRAKE && b == ACT_TRICK); }
static const char *const BTN_NAME[BTN_N] = { "Cross", "Circle", "Square", "Triangle", "L", "R", "Up", "Down", "Left", "Right",
                                            "Start", "Select" };
static inline const char *btn_name(int b) { return b >= 0 && b < BTN_N ? BTN_NAME[b] : "?"; }
static inline char btn_icon(int b) { return (char)(b >= 0 && b < BTN_N ? HUD_ICON_CROSS + b : '?'); }
#endif
