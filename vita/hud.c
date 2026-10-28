#include "hud.h"
#include "../src/nr_data.h"
#ifdef NR_PC
#include "pc_gl.h"
#define sceGxmTransferFinish() ((void)0)
#else
#include <vitaGL.h>
#include <psp2/gxm.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { short x, y, w, h, xo, yo, xa; } glyph;
typedef struct { GLuint tex; int tw, th; float line, base; glyph g[256]; int ok; } font;
static font F[2];
static GLuint white;
static nr_pak *g_pak;

/* ------------------------------------------------------------------ programs */
static const char *VS =
    "attribute vec2 aPos;\n"
    "attribute vec2 aUV;\n"
    "attribute vec4 aCol;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vCol;\n"
    "void main() {\n"
    "  vUV = aUV; vCol = aCol;\n"
    "  gl_Position = vec4(aPos.x / 480.0 - 1.0, 1.0 - aPos.y / 272.0, 0.0, 1.0);\n"
    "}\n";
static const char *FS_TEX =
    "uniform sampler2D uTex;\n"
    "uniform vec4 uColor;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vCol;\n"
    "void main() { gl_FragColor = texture2D(uTex, vUV) * uColor * vCol; }\n";
/* menu_button.fx: red, blue and green channels are masks; Color2.z/.w switch the blue and
 * green parts on, Color tints and sets the opacity */
static const char *FS_BTN =
    "uniform sampler2D uTex;\n"
    "uniform vec4 uC0;\n"
    "uniform vec4 uC1;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vCol;\n"
    "void main() {\n"
    "  vec4 t = texture2D(uTex, vUV);\n"
    "  float i = t.x + t.z * uC1.z + t.y * uC1.w;\n"
    "  float a = t.w + t.z * uC1.z + t.y * uC1.w;\n"
    "  vec3 rgb = i * uC0.rgb + uC1.z * (vec3(0.8, 0.9, 1.0) - i * uC0.rgb);\n"
    "  gl_FragColor = vec4(rgb, clamp(a, 0.0, 1.0) * uC0.w);\n"
    "}\n";
/* heatmeter.fx: the gradient texture's red channel is the fill position; the colour runs from
 * blue to orange along it, shifted towards red by uHue */
static const char *FS_HEAT =
    "uniform sampler2D uTex;\n"
    "uniform vec4 uColor;\n"
    "uniform vec2 uHeat;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vCol;\n"
    "void main() {\n"
    "  vec4 t = texture2D(uTex, vUV);\n"
    "  float k = t.x * 0.35 + uHeat.y;\n"
    "  vec3 rgb = (k * vec3(1.1, -0.3, -1.2) + vec3(0.1, 0.6, 1.2)) * uColor.rgb;\n"
    "  float a = clamp((uHeat.x - t.x) * 50.0, 0.0, 1.0) * t.w * uColor.a;\n"
    "  gl_FragColor = vec4(rgb, a);\n"
    "}\n";

/* screenload.fx (the loading screen, 0x610640): loading2.tga; where its alpha (x2 - 1) is not
 * negative, a band that moves with the seconds since loading began brightens the picture up to 3x:
 * y = 1 - |(|a - t|) - 0.5| x 3.33, c = c x (1 + 2 max(y, 0)) */
static const char *FS_LOAD =
    "uniform sampler2D uTex;\n"
    "uniform vec4 uColor;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vCol;\n"
    "void main() {\n"
    "  vec4 pic = texture2D(uTex, vUV);\n"
    "  float a = pic.a * 2.0 - 1.0;\n"
    "  float band = 1.0 - abs(abs(a - uColor.x) - 0.5) * 3.33333;\n"
    "  float k = 1.0 + 2.0 * clamp(band, 0.0, 1.0) * step(0.0, a);\n"
    "  gl_FragColor = vec4(pic.rgb * k, 1.0) * vCol;\n"
    "}\n";

/* countdown.fx (the countdown's 3, 2, 1, RUSH; LevelStartTextLogic's Float1 F = 0 -> 2 in uColor.x): the picture
 * where its height map (Diffuse2) is above 0.1 (the grid it is drawn in), showing where h + F > 0.9 and gone where
 * h + F > 2.1, the two edges reddened */
static const char *FS_COUNT =
    "uniform sampler2D uTex;\n"
    "uniform sampler2D uHgt;\n"
    "uniform vec4 uColor;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vCol;\n"
    "void main() {\n"
    "  float h = texture2D(uHgt, vUV).r, F = uColor.x;\n"
    "  float x1 = h + F - 0.9, x2 = 2.1 - F - h;\n"
    "  float a = min(clamp(10.0 * (h - 0.1), 0.0, 1.0), min(clamp(10.0 * x1, 0.0, 1.0), clamp(10.0 * x2, 0.0, 1.0)));\n"
    "  float e1 = clamp(2.0 - 10.0 * x1, 0.0, 1.0), e2 = clamp(2.0 - 10.0 * x2, 0.0, 1.0);\n"
    "  vec4 c = texture2D(uTex, vUV);\n"
    "  vec3 r = vec3(c.r + 0.5 * e1, c.g * (1.0 - e1), c.b * (1.0 - e1));\n"
    "  r = vec3(r.r + 0.5 * e2, r.g * (1.0 - e2), r.b * (1.0 - e2));\n"
    "  gl_FragColor = vec4(r, a * c.a) * vCol;\n"
    "}\n";

typedef struct { GLuint p; GLint uColor, uC0, uC1, uHeat; } prog_t;
static prog_t P_TEX, P_BTN, P_HEAT, P_LOAD, P_COUNT;
static prog_t *cur;

static GLuint shader(GLenum t, const char *src)
{
    GLuint s = glCreateShader(t);
#ifdef NR_PC
    const char *srcs[3] = { t == GL_FRAGMENT_SHADER ? "precision mediump float;\n" : "", NRPC_VGL_MACROS, src };
    glShaderSource(s, 3, srcs, NULL); glCompileShader(s);
#else
    glShaderSource(s, 1, &src, NULL); glCompileShader(s);
#endif
    return s;
}
static int make(prog_t *o, const char *fs)
{
    o->p = glCreateProgram();
    glAttachShader(o->p, shader(GL_VERTEX_SHADER, VS));
    glAttachShader(o->p, shader(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(o->p, 0, "aPos");
    glBindAttribLocation(o->p, 1, "aUV");
    glBindAttribLocation(o->p, 2, "aCol");
    glLinkProgram(o->p);
    GLint ok = 0; glGetProgramiv(o->p, GL_LINK_STATUS, &ok);
    if (!ok) return -1;
    glUseProgram(o->p);
    o->uColor = glGetUniformLocation(o->p, "uColor");
    o->uC0 = glGetUniformLocation(o->p, "uC0");
    o->uC1 = glGetUniformLocation(o->p, "uC1");
    o->uHeat = glGetUniformLocation(o->p, "uHeat");
    GLint ut = glGetUniformLocation(o->p, "uTex"); if (ut >= 0) glUniform1i(ut, 0);
    GLint uh = glGetUniformLocation(o->p, "uHgt"); if (uh >= 0) glUniform1i(uh, 1);
    return 0;
}
static void use(prog_t *p) { if (cur != p) { glUseProgram(p->p); cur = p; } }

/* ------------------------------------------------------------------ textures */
typedef struct { char name[48]; GLuint id; int w, h; } uitex;
static uitex T[160]; static int nT;

unsigned hud_tex(const char *name)
{
    for (int i = 0; i < nT; i++) if (!strcmp(T[i].name, name)) return T[i].id;
    if (nT >= 160 || !g_pak || !name || !name[0]) return 0;
    char p[96]; snprintf(p, sizeof p, "ui/%s.nrt", name);
    uint32_t sz; uint8_t *b = nr_pak_read(g_pak, p, &sz);
    nr_texdata t;
    GLuint id = 0;
    if (b && !nr_tex_parse(b, sz, &t)) {
        glGenTextures(1, &id); glBindTexture(GL_TEXTURE_2D, id);
        if (t.fmt == 8) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t.w, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, t.data);
        else {
            int w, h; uint32_t s = nr_tex_mip_size(&t, 0, &w, &h);
            glCompressedTexImage2D(GL_TEXTURE_2D, 0, t.fmt == 1 ? GL_COMPRESSED_RGB_S3TC_DXT1_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, w, h, 0, s, t.data);
            sceGxmTransferFinish();     /* see load_texture in gfx.c */
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        snprintf(T[nT].name, sizeof T[nT].name, "%s", name); T[nT].id = id; T[nT].w = t.w; T[nT].h = t.h; nT++;
    } else {
        snprintf(T[nT].name, sizeof T[nT].name, "%s", name); T[nT].id = 0; nT++;   /* remember the miss */
    }
    free(b);
    return id;
}
void hud_tex_size(unsigned tex, int *w, int *h)
{
    for (int i = 0; i < nT; i++) if (T[i].id == tex) { *w = T[i].w; *h = T[i].h; return; }
    *w = *h = 1;
}

/* ------------------------------------------------------------------ button icons
 * The Vita's buttons, drawn once into a 256x256 atlas at start-up: the face buttons in their
 * colours on a dark disc, the d-pad arrows, the sticks, and a pill for L / R / START / SELECT
 * (their label is drawn on top with the font). Text strings carry them as HUD_ICON_* bytes. */
static GLuint icon_tex;
static uint8_t *ICO;
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static void ico_put(int x, int y, const float c[3], float a)
{
    if (a <= 0) return;
    uint8_t *p = ICO + (y * 256 + x) * 4;
    float da = p[3] / 255.0f, oa = a + da * (1 - a);
    for (int k = 0; k < 3; k++) {
        float dc = p[k] / 255.0f, oc = oa > 0 ? (c[k] * a + dc * da * (1 - a)) / oa : 0;
        p[k] = (uint8_t)(clampf(oc, 0, 1) * 255 + 0.5f);
    }
    p[3] = (uint8_t)(clampf(oa, 0, 1) * 255 + 0.5f);
}
static float sd_seg(float px, float py, float ax, float ay, float bx, float by)
{
    float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
    float h = clampf((pax * bax + pay * bay) / (bax * bax + bay * bay), 0, 1);
    float dx = pax - bax * h, dy = pay - bay * h;
    return sqrtf(dx * dx + dy * dy);
}
static float cov(float d) { return clampf(0.5f - d, 0, 1); }       /* d: signed distance in pixels */
/* signed distance to a triangle's edges (negative inside) */
static float sd_tri(float px, float py, const float v[3][2])
{
    float d = 1e9f; int inside = 1;
    for (int i = 0; i < 3; i++) {
        const float *a = v[i], *b = v[(i + 1) % 3];
        d = fminf(d, sd_seg(px, py, a[0], a[1], b[0], b[1]));
        float cr = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
        if (cr < 0) inside = 0;
    }
    return inside ? -d : d;
}
static void make_icons(void)
{
    ICO = calloc(256 * 256 * 4, 1);
    if (!ICO) return;
    static const float BG[3] = { 0.09f, 0.09f, 0.11f }, RIM[3] = { 0.80f, 0.80f, 0.84f }, WH[3] = { 0.95f, 0.95f, 0.97f };
    static const float COL[4][3] = { { 0.49f, 0.70f, 0.95f }, { 0.96f, 0.40f, 0.40f }, { 0.93f, 0.58f, 0.85f }, { 0.25f, 0.85f, 0.69f } };
    for (int cell = 0; cell < 10; cell++) {
        int ox = (cell % 4) * 64, oy = (cell / 4) * 64;
        for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
            float px = x + 0.5f - 32, py = y + 0.5f - 32, r = sqrtf(px * px + py * py);
            ico_put(ox + x, oy + y, BG, 0.92f * cov(r - 28.5f));
            ico_put(ox + x, oy + y, RIM, cov(fabsf(r - 27.5f) - 1.6f));
            float a = 0; const float *c = WH;
            if (cell == 0) { a = cov(fminf(sd_seg(px, py, -10, -10, 10, 10), sd_seg(px, py, 10, -10, -10, 10)) - 3.0f); c = COL[0]; }
            else if (cell == 1) { a = cov(fabsf(r - 12.0f) - 3.0f); c = COL[1]; }
            else if (cell == 2) { float d = fmaxf(fabsf(px), fabsf(py)); a = cov(fabsf(d - 11.0f) - 3.0f); c = COL[2]; }
            else if (cell == 3) { const float v[3][2] = { { 0, -13 }, { 12.5f, 9 }, { -12.5f, 9 } }; a = cov(fabsf(sd_tri(px, py, v)) - 3.0f); c = COL[3]; }
            else if (cell >= 4 && cell <= 7) {     /* the d-pad: an arrow */
                float v[3][2] = { { 0, -14 }, { 12, 8 }, { -12, 8 } };
                for (int k = 0; k < 3; k++) {
                    float vx = v[k][0], vy = v[k][1];
                    if (cell == 5) { v[k][0] = -vx; v[k][1] = -vy; }
                    else if (cell == 6) { v[k][0] = vy; v[k][1] = -vx; }
                    else if (cell == 7) { v[k][0] = -vy; v[k][1] = vx; }
                }
                a = cov(sd_tri(px, py, v) - 0.5f);
            } else {                                /* a stick: a ring inside (the label goes on top) */
                a = 0.55f * cov(fabsf(r - 19.0f) - 1.5f);
            }
            ico_put(ox + x, oy + y, c, a);
        }
    }
    /* the pill (128x64 at 0,192) */
    for (int y = 0; y < 64; y++) for (int x = 0; x < 128; x++) {
        float px = fabsf(x + 0.5f - 64), py = fabsf(y + 0.5f - 32);
        float qx = fmaxf(px - (60 - 26), 0), qy = fmaxf(py - (28 - 26), 0);
        float d = sqrtf(qx * qx + qy * qy) - 26;
        ico_put(x, 192 + y, BG, 0.92f * cov(d));
        ico_put(x, 192 + y, RIM, cov(fabsf(d + 1.0f) - 1.6f));
    }
    glGenTextures(1, &icon_tex); glBindTexture(GL_TEXTURE_2D, icon_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, ICO);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(ICO); ICO = NULL;
}
/* an icon byte: its atlas cell (-1 = the pill), its label, its width in text heights */
static int icon_info(int c, float uv[4], const char **label, float *wf)
{
    static const char *LB[] = { "", "", "", "", "L", "R", "", "", "", "", "START", "SELECT", "L", "R" };
    static const int CELL[] = { 0, 1, 2, 3, -1, -1, 4, 5, 6, 7, -1, -1, 8, 9 };
    static const float WF[] = { 1, 1, 1, 1, 1.45f, 1.45f, 1, 1, 1, 1, 2.6f, 2.9f, 1, 1 };
    int i = c - HUD_ICON_CROSS;
    if (i < 0 || i >= (int)(sizeof CELL / sizeof CELL[0])) return 0;
    if (CELL[i] < 0) { uv[0] = 0; uv[1] = 192 / 256.0f; uv[2] = 0.5f; uv[3] = 1; }
    else { uv[0] = (CELL[i] % 4) * 0.25f; uv[1] = (CELL[i] / 4) * 0.25f; uv[2] = uv[0] + 0.25f; uv[3] = uv[1] + 0.25f; }
    *label = LB[i]; *wf = WF[i];
    return 1;
}

/* ------------------------------------------------------------------ fonts */
static int load_font(font *f, nr_pak *pak, const char *name)
{
    char p[96]; uint32_t sz;
    snprintf(p, sizeof p, "fonts/%s.fnt", name);
    char *t = nr_pak_read(pak, p, &sz);
    snprintf(p, sizeof p, "textures/font_%s.nrt", name);
    uint8_t *tb = nr_pak_read(pak, p, &sz);
    nr_texdata td;
    if (!t || !tb || nr_tex_parse(tb, sz, &td) || td.fmt != 8) { free(t); free(tb); return -1; }
    memset(f, 0, sizeof *f);
    int line = 0, base = 0;
    const char *q = strstr(t, "lineHeight="); if (q) line = atoi(q + 11);
    q = strstr(t, "base="); if (q) base = atoi(q + 5);
    f->line = line; f->base = base;
    for (q = strstr(t, "char id="); q; q = strstr(q + 1, "char id=")) {
        int id, x, y, w, h, xo, yo, xa;
        if (sscanf(q, "char id=%d x=%d y=%d width=%d height=%d xoffset=%d yoffset=%d xadvance=%d", &id, &x, &y, &w, &h, &xo, &yo, &xa) == 8 && id >= 0 && id < 256) {
            glyph *g = &f->g[id]; g->x = x; g->y = y; g->w = w; g->h = h; g->xo = xo; g->yo = yo; g->xa = xa;
        }
    }
    /* fonts without lower case: use the capitals */
    for (int c = 'a'; c <= 'z'; c++) if (!f->g[c].xa && f->g[c - 32].xa) f->g[c] = f->g[c - 32];
    f->tw = td.w; f->th = td.h;
    glGenTextures(1, &f->tex); glBindTexture(GL_TEXTURE_2D, f->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, td.w, td.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, td.data);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(t); free(tb);
    f->ok = 1;
    return 0;
}

/* ------------------------------------------------------------------ HUD meshes */
typedef struct { float x, y, u, v; float r, g, b, a; } mvert;
static struct { int nv, ni; mvert *v; uint16_t *idx; } M[HUD_NMESH];

static void load_meshes(nr_pak *pak)
{
    uint32_t sz; uint8_t *b = nr_pak_read(pak, "ui/hud.bin", &sz);
    if (!b || sz < 8 || memcmp(b, "NRH1", 4)) { free(b); return; }
    uint32_t n; memcpy(&n, b + 4, 4);
    uint8_t *p = b + 8;
    for (uint32_t k = 0; k < n && k < HUD_NMESH; k++) {
        uint32_t nv, ni; memcpy(&nv, p + 16, 4); memcpy(&ni, p + 20, 4); p += 24;
        M[k].nv = nv; M[k].ni = ni;
        M[k].v = malloc(sizeof(mvert) * nv); M[k].idx = malloc(2 * ni);
        for (uint32_t i = 0; i < nv; i++) {
            float f[4]; memcpy(f, p, 16);
            M[k].v[i].x = f[0] * 960.0f; M[k].v[i].y = (1.0f - f[1]) * 544.0f;   /* the game measures up from the bottom */ M[k].v[i].u = f[2]; M[k].v[i].v = f[3];
            M[k].v[i].r = p[16] / 255.0f; M[k].v[i].g = p[17] / 255.0f; M[k].v[i].b = p[18] / 255.0f; M[k].v[i].a = p[19] / 255.0f;
            p += 20;
        }
        memcpy(M[k].idx, p, 2 * ni); p += 2 * ni + ((ni & 1) ? 2 : 0);
    }
    free(b);
}

int hud_init(nr_pak *pak)
{
    g_pak = pak;
    if (make(&P_TEX, FS_TEX) || make(&P_BTN, FS_BTN) || make(&P_HEAT, FS_HEAT)) return -1;
    if (make(&P_LOAD, FS_LOAD)) P_LOAD.p = 0;
    if (make(&P_COUNT, FS_COUNT)) P_COUNT.p = 0;
    uint8_t px[16 * 4]; memset(px, 255, sizeof px);
    glGenTextures(1, &white); glBindTexture(GL_TEXTURE_2D, white);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    int a = load_font(&F[0], pak, "hemihead"), b = load_font(&F[1], pak, "ethon");
    if (b) F[1] = F[0];
    load_meshes(pak);
    make_icons();
    return a;
}

/* ------------------------------------------------------------------ batching */
#define MAXQ 256
static float vbuf[MAXQ * 6 * 8];      /* x y u v r g b a */
static unsigned cur_col = 0xFFFFFFFF;

static void set_color(prog_t *p, unsigned rgba)
{
    if (p->uColor >= 0) glUniform4f(p->uColor, (rgba >> 24 & 255) / 255.0f, (rgba >> 16 & 255) / 255.0f, (rgba >> 8 & 255) / 255.0f, (rgba & 255) / 255.0f);
}
static void flush(int nq)
{
    if (!nq) return;
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 32, vbuf);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 32, vbuf + 2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 32, vbuf + 4);
    glDrawArrays(GL_TRIANGLES, 0, nq * 6);
}
static void quad4(int i, const float xy[4][2], float u0, float v0, float u1, float v1)
{
    float *v = vbuf + i * 48;
    const float uv[4][2] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
    static const int ord[6] = { 0, 1, 2, 0, 2, 3 };
    for (int k = 0; k < 6; k++) {
        int c = ord[k];
        v[k * 8 + 0] = xy[c][0]; v[k * 8 + 1] = xy[c][1]; v[k * 8 + 2] = uv[c][0]; v[k * 8 + 3] = uv[c][1];
        v[k * 8 + 4] = v[k * 8 + 5] = v[k * 8 + 6] = v[k * 8 + 7] = 1.0f;
    }
}
static void quad(int i, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1)
{
    const float xy[4][2] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
    quad4(i, xy, u0, v0, u1, v1);
}

void hud_begin(void)
{
    cur = NULL; use(&P_TEX);
    glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1); glEnableVertexAttribArray(2);
    glDisableVertexAttribArray(3);
    glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    (void)cur_col;
}
void hud_end(void)
{
    glDisable(GL_BLEND); glDepthMask(GL_TRUE); glEnable(GL_DEPTH_TEST);
    glEnableVertexAttribArray(2); glEnableVertexAttribArray(3);
    cur = NULL;
}
void hud_additive(int on)
{
    if (on) glBlendFunc(GL_SRC_ALPHA, GL_ONE); else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void hud_opaque(int on) { if (on) glDisable(GL_BLEND); else glEnable(GL_BLEND); }

void hud_loading_screen(unsigned tex, float t)
{
    if (!tex) return;
    glDisable(GL_BLEND);
    if (!P_LOAD.p) { hud_image(tex, 0, 0, 960, 544, 0xFFFFFFFF); glEnable(GL_BLEND); return; }
    use(&P_LOAD);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform4f(P_LOAD.uColor, t, 0, 0, 0);
    quad(0, 0, 0, 960, 544, 0, 0, 1, 1);
    flush(1);
    glEnable(GL_BLEND);
}

void hud_rect(float x, float y, float w, float h, unsigned rgba)
{
    use(&P_TEX);
    glBindTexture(GL_TEXTURE_2D, white);
    set_color(&P_TEX, rgba);
    quad(0, x, y, x + w, y + h, 0, 0, 1, 1);
    flush(1);
}

void hud_image_uv(unsigned tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1, unsigned rgba)
{
    if (!tex) return;
    use(&P_TEX);
    glBindTexture(GL_TEXTURE_2D, tex);
    set_color(&P_TEX, rgba);
    quad(0, x, y, x + w, y + h, u0, v0, u1, v1);
    flush(1);
}
void hud_image(unsigned tex, float x, float y, float w, float h, unsigned rgba) { hud_image_uv(tex, x, y, w, h, 0, 0, 1, 1, rgba); }

void hud_countdown(unsigned tex, unsigned height, float cx, float cy, float size, float F)
{
    if (!tex) return;
    if (!height || !P_COUNT.p) {                 /* (no height map: faded in and out) */
        float a = F < 0.9f ? F / 0.9f : F > 1.1f ? (2.0f - F) / 0.9f : 1.0f;
        hud_image_rot(tex, cx, cy, size, size, 0, 0xFFFFFF00 | (unsigned)(fmaxf(0, fminf(1, a)) * 255)); return;
    }
    use(&P_COUNT);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, height);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex);
    glUniform4f(P_COUNT.uColor, F, 0, 0, 0);
    quad(0, cx - size / 2, cy - size / 2, cx + size / 2, cy + size / 2, 0, 0, 1, 1);
    flush(1);
}

void hud_image_rot(unsigned tex, float cx, float cy, float w, float h, float angle, unsigned rgba)
{
    if (!tex) return;
    use(&P_TEX);
    glBindTexture(GL_TEXTURE_2D, tex);
    set_color(&P_TEX, rgba);
    float c = cosf(angle), s = sinf(angle), hw = w / 2, hh = h / 2;
    const float lp[4][2] = { { -hw, -hh }, { hw, -hh }, { hw, hh }, { -hw, hh } };
    float xy[4][2];
    for (int i = 0; i < 4; i++) { xy[i][0] = cx + lp[i][0] * c - lp[i][1] * s; xy[i][1] = cy + lp[i][0] * s + lp[i][1] * c; }
    quad4(0, xy, 0, 0, 1, 1);
    flush(1);
}

void hud_button(unsigned tex, float x, float y, float w, float h, const float c0[4], const float c1[4])
{
    if (!tex) return;
    use(&P_BTN);
    glUniform4f(P_BTN.uC0, c0[0], c0[1], c0[2], c0[3]);
    glUniform4f(P_BTN.uC1, c1[0], c1[1], c1[2], c1[3]);
    glBindTexture(GL_TEXTURE_2D, tex);
    quad(0, x, y, x + w, y + h, 0, 0, 1, 1);
    flush(1);
}

static void draw_mesh(int which)
{
    if (which < 0 || which >= HUD_NMESH || !M[which].nv) return;
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(mvert), &M[which].v[0].x);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(mvert), &M[which].v[0].u);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(mvert), &M[which].v[0].r);
    glDrawElements(GL_TRIANGLES, M[which].ni, GL_UNSIGNED_SHORT, M[which].idx);
}
void hud_mesh(int which, unsigned tex, unsigned rgba)
{
    if (!tex) return;
    use(&P_TEX);
    glBindTexture(GL_TEXTURE_2D, tex);
    set_color(&P_TEX, rgba);
    draw_mesh(which);
}
void hud_heat_meter(unsigned tex, float heat, float hue)
{
    if (!tex) return;
    use(&P_HEAT);
    glBindTexture(GL_TEXTURE_2D, tex);
    set_color(&P_HEAT, 0xFFFFFFFF);
    glUniform2f(P_HEAT.uHeat, heat, hue);
    draw_mesh(HUD_MESH_HEAT);
}

/* ------------------------------------------------------------------ text */
float hud_text_width(int font_i, float size, const char *s)
{
    font *f = &F[font_i & 1];
    if (!f->ok) return 0;
    float k = size / f->line, w = 0, uv[4], wf; const char *lb;
    for (; *s && *s != '|'; s++) {
        if (icon_info((unsigned char)*s, uv, &lb, &wf)) w += size * (wf + 0.15f);
        else w += f->g[(unsigned char)*s].xa * k;
    }
    return w;
}

float hud_text(int font_i, float x, float y, float size, unsigned rgba, int align, const char *s)
{
    font *f = &F[font_i & 1];
    if (!f->ok) return 0;
    float k = size / f->line, w = hud_text_width(font_i, size, s);
    if (align == HUD_CENTER) x -= w / 2; else if (align == HUD_RIGHT) x -= w;
    use(&P_TEX);
    glBindTexture(GL_TEXTURE_2D, f->tex);
    set_color(&P_TEX, rgba);
    int n = 0;
    for (; *s && *s != '|'; s++) {
        float uv[4], wf; const char *lb;
        if (icon_info((unsigned char)*s, uv, &lb, &wf)) {     /* a button icon in the line */
            flush(n); n = 0;
            float ih = size * 1.08f, iw = size * wf, ix = x + size * 0.075f, iy = y + size * 0.5f - ih * 0.5f;
            if (icon_tex) {
                glBindTexture(GL_TEXTURE_2D, icon_tex);
                set_color(&P_TEX, 0xFFFFFF00 | (rgba & 255));
                quad(0, ix, iy, ix + iw, iy + ih, uv[0], uv[1], uv[2], uv[3]);
                flush(1);
            }
            if (lb[0]) {
                float ls = size * (strlen(lb) > 1 ? 0.5f : 0.66f);
                hud_text(font_i, ix + iw * 0.5f, iy + ih * 0.5f - ls * 0.5f, ls, 0xF0F0F400 | (rgba & 255), HUD_CENTER, lb);
            }
            glBindTexture(GL_TEXTURE_2D, f->tex);
            set_color(&P_TEX, rgba);
            x += size * (wf + 0.15f);
            continue;
        }
        glyph *g = &f->g[(unsigned char)*s];
        if (g->w && g->h) {
            float x0 = x + g->xo * k, y0 = y + g->yo * k;
            quad(n++, x0, y0, x0 + g->w * k, y0 + g->h * k,
                 (float)g->x / f->tw, (float)g->y / f->th, (float)(g->x + g->w) / f->tw, (float)(g->y + g->h) / f->th);
            if (n == MAXQ) { flush(n); n = 0; }
        }
        x += g->xa * k;
    }
    flush(n);
    return w;
}

float hud_text_lines(int font_i, float x, float y, float size, float gap, unsigned rgba, int align, const char *s)
{
    float y0 = y;
    for (;;) {
        hud_text(font_i, x, y, size, rgba, align, s);
        const char *b = strchr(s, '|');
        y += size * gap;
        if (!b) break;
        s = b + 1;
    }
    return y - y0;
}
