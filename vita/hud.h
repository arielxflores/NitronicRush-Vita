/* 2D drawing: the game's bitmap fonts, its menu/HUD textures (ui/<name>.nrt), the HUD bar meshes
 * (ui/hud.bin) and the menu-button / heat-meter looks (ported from menu_button.fx and
 * heatmeter.fx). Screen space is 960x544, origin top left. */
#ifndef NR_HUD_H
#define NR_HUD_H
#include "../src/nr_pak.h"

enum { HUD_FONT_MAIN = 0, HUD_FONT_TITLE = 1 };      /* hemihead, ethon */
enum { HUD_LEFT = 0, HUD_CENTER = 1, HUD_RIGHT = 2 };
enum { HUD_MESH_MAIN = 0, HUD_MESH_HEAT, HUD_MESH_HEATLINES, HUD_MESH_FLAMES, HUD_NMESH };
/* the Vita's buttons as bytes inside text (drawn as icons) */
enum { HUD_ICON_CROSS = 0x11, HUD_ICON_CIRCLE, HUD_ICON_SQUARE, HUD_ICON_TRIANGLE, HUD_ICON_L, HUD_ICON_R,
       HUD_ICON_UP, HUD_ICON_DOWN, HUD_ICON_LEFT, HUD_ICON_RIGHT, HUD_ICON_START, HUD_ICON_SELECT,
       HUD_ICON_LSTICK, HUD_ICON_RSTICK };

int   hud_init(nr_pak *pak);                         /* 0 = ok */
void  hud_begin(void);                               /* 2D state for the rest of the frame */
void  hud_end(void);                                 /* restores 3D state */
/* size = text height in pixels; colour as 0xRRGGBBAA */
float hud_text(int font, float x, float y, float size, unsigned rgba, int align, const char *s);
float hud_text_width(int font, float size, const char *s);
/* text with '|' line breaks, centred on x; returns the height used */
float hud_text_lines(int font, float x, float y, float size, float gap, unsigned rgba, int align, const char *s);
void  hud_rect(float x, float y, float w, float h, unsigned rgba);
/* the countdown's 3 / 2 / 1 / RUSH (countdown.fx): size x size round (cx, cy), F its Float1 (0 -> 2 over its life) */
void  hud_countdown(unsigned tex, unsigned height, float cx, float cy, float size, float F);
void  hud_additive(int on);                          /* additive blending for the next draws */
void  hud_opaque(int on);                            /* no blending (the image's alpha ignored) */
/* the PC's loading screen (screenload.fx): the picture with its light sweep, t seconds into the load */
void  hud_loading_screen(unsigned tex, float t);

/* textures from ui/<name>.nrt (cached); 0 if missing */
unsigned hud_tex(const char *name);
void  hud_tex_size(unsigned tex, int *w, int *h);
void  hud_image(unsigned tex, float x, float y, float w, float h, unsigned rgba);
void  hud_image_uv(unsigned tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1, unsigned rgba);
/* image centred on (cx, cy), scaled and rotated (radians) */
void  hud_image_rot(unsigned tex, float cx, float cy, float w, float h, float angle, unsigned rgba);
/* menu_button.fx: col/col2 are the effect's Color and Color2 (r, g, b, a) */
void  hud_button(unsigned tex, float x, float y, float w, float h, const float col[4], const float col2[4]);
/* HUD bar meshes (normalised screen coordinates); heat 0..1 drives heatmeter.fx */
void  hud_mesh(int which, unsigned tex, unsigned rgba);
void  hud_heat_meter(unsigned tex, float heat, float hue);
#endif
