/* 3D drawing: the converted levels and models with vitaGL (GLSL), lit and post-processed the
 * way the PC game's renderer does it (see gfx.h). */
#include "gfx.h"
#ifndef NR_PC
#include <psp2/gxm.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#else
#include <vitasdk.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

gfx_switches GSW;
int gfx_draws, gfx_tris, gfx_dbg;
void (*gfx_load_tick)(void);
float VP[4][4];
float gfx_time;
float gfx_T, gfx_ab;              /* the PC shaders' Time (0..1, 0.05 a second) and AudioBeat (seconds since the beat) */
float gfx_bloom_tune[4] = { 0.65f, 0.75f, 0, 1.3f };   /* weights of the quarter and sixteenth glow, -, scale */
float gfx_bloom_thr = 0.5f;                                 /* brightness where the glow starts */
static void nolog(const char *fmt, ...) { (void)fmt; }
static void (*logm)(const char *fmt, ...) = nolog;
void gfx_set_log(void (*fn)(const char *fmt, ...)) { logm = fn ? fn : nolog; }

#define SW 960
#define SH 544

/* ------------------------------------------------------------------ matrices */
static float VIEW[3][3];   /* world -> view rotation (rows: right, up, back) */
static int vp_id;          /* bumped with every view-projection: a program's frame uniforms are set once per id */
static void build_vp(v3 eye, v3 fwd, v3 upv, int no_translate, float fov_y, float znear, float zfar)
{
    v3 r = norm(cross(fwd, upv)), u = cross(r, fwd);
    float V[4][4] = {
        { r.x, r.y, r.z, no_translate ? 0 : -dot(r, eye) },
        { u.x, u.y, u.z, no_translate ? 0 : -dot(u, eye) },
        { -fwd.x, -fwd.y, -fwd.z, no_translate ? 0 : dot(fwd, eye) },
        { 0, 0, 0, 1 } };
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) VIEW[i][j] = V[i][j];
    vp_id++;
    float f = 1.0f / tanf(fov_y * 0.5f), a = (float)SW / SH;
    float Pm[4][4] = {
        { f / a, 0, 0, 0 },
        { 0, f, 0, 0 },
        { 0, 0, (zfar + znear) / (znear - zfar), 2 * zfar * znear / (znear - zfar) },
        { 0, 0, -1, 0 } };
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        float s = 0; for (int k = 0; k < 4; k++) s += Pm[i][k] * V[k][j];
        VP[i][j] = s;
    }
}
static float planes[6][4];
static void build_planes(void)
{
    for (int i = 0; i < 3; i++) for (int s = 0; s < 2; s++) {
        float *p = planes[i * 2 + s];
        for (int j = 0; j < 4; j++) p[j] = VP[3][j] + (s ? -VP[i][j] : VP[i][j]);
    }
}
static int aabb_visible(const float *mn, const float *mx)
{
    for (int i = 0; i < 6; i++) {
        const float *p = planes[i];
        float x = p[0] > 0 ? mx[0] : mn[0], y = p[1] > 0 ? mx[1] : mn[1], z = p[2] > 0 ? mx[2] : mn[2];
        if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0) return 0;
    }
    return 1;
}
/* a sphere against the view's planes (they are not normalised: the distance is scaled by |n|) */
static int sphere_visible(const float c[3], float r)
{
    for (int i = 0; i < 6; i++) {
        const float *p = planes[i];
        float n = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (p[0] * c[0] + p[1] * c[1] + p[2] * c[2] + p[3] < -r * n) return 0;
    }
    return 1;
}
static float aabb_dist(const float *mn, const float *mx, v3 e)
{
    float dx = fmaxf(fmaxf(mn[0] - e.x, 0), e.x - mx[0]);
    float dy = fmaxf(fmaxf(mn[1] - e.y, 0), e.y - mx[1]);
    float dz = fmaxf(fmaxf(mn[2] - e.z, 0), e.z - mx[2]);
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

/* ------------------------------------------------------------------ shaders
 * Every 3D program uses the same attributes and varyings (in the same order), so vitaGL's
 * GLSL translation pairs them up the same way in every program. */
#define ATTRS \
    "attribute vec3 aPos;\n" "attribute vec2 aUV;\n" "attribute vec4 aNrm;\n" "attribute vec4 aCol;\n" "attribute vec4 aLit;\n"
#define VARYINGS \
    "varying vec2 vUV;\n" "varying vec4 vCol;\n" "varying vec3 vLit;\n" "varying vec2 vFog;\n" "varying vec2 vRef;\n"
#define FRAME_UNIFORMS \
    "uniform vec4 uVP0;\n" "uniform vec4 uVP1;\n" "uniform vec4 uVP2;\n" "uniform vec4 uVP3;\n" \
    "uniform vec3 uEye;\n" "uniform vec4 uSun;\n" "uniform vec4 uFog;\n" "uniform vec4 uSky;\n" \
    "uniform vec4 uV0;\n" "uniform vec4 uV1;\n" "uniform vec4 uV2;\n" "uniform vec4 uBeat;\n"
/* uBeat (and uFT in the fragment shaders): LightPulse factor, AudioBeat, Time, fract(5 Time) */
/* skybox.fx's fog: distance fog (smoothstep FogNear..FogFar) that thins out with height
 * (gone above z = 200, where the tracks are), and the fade into the sky from FogFar to SkyFar.
 * vFog.x = fog amount (towards the fog colour), vFog.y = how much of the sky/fog shows */
#define FOG_CODE \
    "  float fd = distance(w, uEye);\n" \
    "  float ff = clamp((fd - uFog.x) * uFog.y, 0.0, 1.0);\n" \
    "  float fs = clamp((fd - uSky.x) * uSky.y, 0.0, 1.0);\n" \
    "  float hc = clamp(w.z, -200.0, 200.0);\n" \
    "  float hx = clamp(((hc + uEye.z) * 0.5 + 200.0) * 0.0025, 0.0, 1.0);\n" \
    "  float dz = uEye.z - w.z;\n" \
    "  float hr = mix(1.0, clamp((uEye.z - hc) / (sign(dz) * max(abs(dz), 1.0)), 0.0, 1.0), step(200.0, abs(w.z)));\n" \
    "  float fog = clamp(ff * ff * (3.0 - 2.0 * ff) * hr * (1.0 - hx * hx * (3.0 - 2.0 * hx)), 0.0, 1.0) * uFog.z;\n" \
    "  vFog = vec2(fog, max(fog, fs * fs * (3.0 - 2.0 * fs)));\n"
/* phong_ref: the sphere map is looked up by the half vector between the normal and the eye
 * (view space); phong_ref_pulse (the car) by the plain normal */
#define SPHERE_CODE(PLAIN) \
    "  vec3 hn = " PLAIN " ? n : normalize(n + normalize(uEye - w));\n" \
    "  vec3 vn = vec3(dot(uV0.xyz, hn), dot(uV1.xyz, hn), dot(uV2.xyz, hn));\n" \
    "  vRef = vn.xy * vec2(0.5, -0.5) + 0.5;\n"

/* level geometry: world-space vertices with the level's lights baked in; the sun shines
 * straight down (light_sun: colour * (max(N.L, 0) + ambient)) */
static const char *VS_WORLD = ATTRS VARYINGS FRAME_UNIFORMS
    "void main() {\n"
    "  vec3 w = aPos;\n"
    "  vec4 p = vec4(w, 1.0);\n"
    "  gl_Position = vec4(dot(uVP0, p), dot(uVP1, p), dot(uVP2, p), dot(uVP3, p));\n"
    "  vUV = aUV;\n"
    "  float pi = aLit.a * 2.55;\n"                       /* LightPulse x 100: the glow flashes on the beat */
    "  vCol = vec4(aCol.rgb, aCol.a * 8.0 * (pi > 0.0 ? pi * uBeat.x : 1.0));\n"
    "  vec3 n = aNrm.xyz;\n"
    "  float pf = aNrm.w;\n"                              /* the share of pulsing lights in the baked light */
    "  vLit = aLit.rgb * 4.0 * (1.0 - pf + pf * uBeat.x) + uSun.rgb * (max(n.z, 0.0) + uSun.a);\n"
    FOG_CODE SPHERE_CODE("false")
    "}\n";
/* the same without the sphere-map coordinate, for every shader that doesn't read it (all but
 * phong_ref): a quarter less work per vertex */
static const char *VS_WORLD_NR = ATTRS VARYINGS FRAME_UNIFORMS
    "void main() {\n"
    "  vec3 w = aPos;\n"
    "  vec4 p = vec4(w, 1.0);\n"
    "  gl_Position = vec4(dot(uVP0, p), dot(uVP1, p), dot(uVP2, p), dot(uVP3, p));\n"
    "  vUV = aUV;\n"
    "  float pi = aLit.a * 2.55;\n"
    "  vCol = vec4(aCol.rgb, aCol.a * 8.0 * (pi > 0.0 ? pi * uBeat.x : 1.0));\n"
    "  vec3 n = aNrm.xyz;\n"
    "  float pf = aNrm.w;\n"
    "  vLit = aLit.rgb * 4.0 * (1.0 - pf + pf * uBeat.x) + uSun.rgb * (max(n.z, 0.0) + uSun.a);\n"
    "  vRef = vec2(0.0);\n"
    FOG_CODE
    "}\n";
/* tvscreen.fx on level geometry: the static noise scrolls (two coordinates in vRef and vLit.xy) */
static const char *VS_WTV = ATTRS VARYINGS FRAME_UNIFORMS
    "void main() {\n"
    "  vec3 w = aPos;\n"
    "  vec4 p = vec4(w, 1.0);\n"
    "  gl_Position = vec4(dot(uVP0, p), dot(uVP1, p), dot(uVP2, p), dot(uVP3, p));\n"
    "  vUV = aUV;\n"
    "  vCol = vec4(aCol.rgb, aCol.a * 8.0);\n"
    "  vRef = aUV + uBeat.z * vec2(1.3, -3.0);\n"
    "  vLit = vec3(aUV + uBeat.z * vec2(17.3, -120.123), 0.0);\n"
    FOG_CODE
    "}\n";
/* unlit level geometry (city.fx): the light attribute carries the shader's own values */
static const char *VS_WUNLIT = ATTRS VARYINGS FRAME_UNIFORMS
    "void main() {\n"
    "  vec3 w = aPos;\n"
    "  vec4 p = vec4(w, 1.0);\n"
    "  gl_Position = vec4(dot(uVP0, p), dot(uVP1, p), dot(uVP2, p), dot(uVP3, p));\n"
    "  vUV = aUV;\n"
    "  vCol = aCol;\n"
    "  vLit = aLit.rgb; vRef = vec2(0.0);\n"
    FOG_CODE
    "}\n";

/* moving things (the car and its parts, gibs, flames): a model matrix, and the sun plus the
 * four nearest level lights per vertex (light_point: colour * att * (max(N.L, 0) + ambient)) */
static const char *VS_OBJ = ATTRS VARYINGS FRAME_UNIFORMS
    "uniform vec4 uM0;\n" "uniform vec4 uM1;\n" "uniform vec4 uM2;\n"
    "uniform vec4 uLA0;\n" "uniform vec4 uLA1;\n" "uniform vec4 uLA2;\n" "uniform vec4 uLA3;\n"
    "uniform vec4 uLB0;\n" "uniform vec4 uLB1;\n" "uniform vec4 uLB2;\n" "uniform vec4 uLB3;\n"
    "uniform vec4 uLamb;\n" "uniform vec4 uOCol;\n" "uniform vec4 uOFlag;\n" "uniform vec4 uVFlag;\n"
    "vec3 plight(vec4 A, vec4 B, float amb, vec3 q, vec3 n) {\n"
    "  vec3 v = A.xyz - q;\n"
    "  float d = max(length(v), 0.001);\n"
    "  float att = clamp(A.w * (B.w - d) / (d * (B.w - A.w)), 0.0, 1.5);\n"
    "  return B.rgb * (att * (max(dot(n, v / d), 0.0) + amb));\n"
    "}\n"
    "void main() {\n"
    "  vec4 p0 = vec4(aPos, 1.0);\n"
    "  vec3 w = vec3(dot(uM0, p0), dot(uM1, p0), dot(uM2, p0));\n"
    "  vec4 p = vec4(w, 1.0);\n"
    "  gl_Position = vec4(dot(uVP0, p), dot(uVP1, p), dot(uVP2, p), dot(uVP3, p));\n"
    "  vUV = aUV;\n"
    "  vCol = uOFlag.x > 0.5 ? uOCol : vec4(aCol.rgb, aCol.a * 8.0);\n"   /* RenderMesh Color1 set by the object's logic */
    "  vec3 n = normalize(vec3(dot(uM0.xyz, aNrm.xyz), dot(uM1.xyz, aNrm.xyz), dot(uM2.xyz, aNrm.xyz)));\n"
    "  vLit = uSun.rgb * (max(n.z, 0.0) + uSun.a) + plight(uLA0, uLB0, uLamb.x, w, n) + plight(uLA1, uLB1, uLamb.y, w, n)\n"
    "       + plight(uLA2, uLB2, uLamb.z, w, n) + plight(uLA3, uLB3, uLamb.w, w, n);\n"
    FOG_CODE SPHERE_CODE("uVFlag.x > 0.5")
    "}\n";
#define VS_OBJ_HEAD ATTRS VARYINGS FRAME_UNIFORMS \
    "uniform vec4 uM0;\n" "uniform vec4 uM1;\n" "uniform vec4 uM2;\n" \
    "uniform vec4 uLA0;\n" "uniform vec4 uLA1;\n" "uniform vec4 uLA2;\n" "uniform vec4 uLA3;\n" \
    "uniform vec4 uLB0;\n" "uniform vec4 uLB1;\n" "uniform vec4 uLB2;\n" "uniform vec4 uLB3;\n" \
    "uniform vec4 uLamb;\n" "uniform vec4 uOCol;\n" "uniform vec4 uOFlag;\n" "uniform vec4 uVFlag;\n"
static const char *VS_OBJ_NR = VS_OBJ_HEAD
    "vec3 plight(vec4 A, vec4 B, float amb, vec3 q, vec3 n) {\n"
    "  vec3 v = A.xyz - q;\n"
    "  float d = max(length(v), 0.001);\n"
    "  float att = clamp(A.w * (B.w - d) / (d * (B.w - A.w)), 0.0, 1.5);\n"
    "  return B.rgb * (att * (max(dot(n, v / d), 0.0) + amb));\n"
    "}\n"
    "void main() {\n"
    "  vec4 p0 = vec4(aPos, 1.0);\n"
    "  vec3 w = vec3(dot(uM0, p0), dot(uM1, p0), dot(uM2, p0));\n"
    "  vec4 p = vec4(w, 1.0);\n"
    "  gl_Position = vec4(dot(uVP0, p), dot(uVP1, p), dot(uVP2, p), dot(uVP3, p));\n"
    "  vUV = aUV;\n"
    "  vCol = uOFlag.x > 0.5 ? uOCol : vec4(aCol.rgb, aCol.a * 8.0);\n"
    "  vec3 n = normalize(vec3(dot(uM0.xyz, aNrm.xyz), dot(uM1.xyz, aNrm.xyz), dot(uM2.xyz, aNrm.xyz)));\n"
    "  vLit = uSun.rgb * (max(n.z, 0.0) + uSun.a) + plight(uLA0, uLB0, uLamb.x, w, n) + plight(uLA1, uLB1, uLamb.y, w, n)\n"
    "       + plight(uLA2, uLB2, uLamb.z, w, n) + plight(uLA3, uLB3, uLamb.w, w, n);\n"
    "  vRef = vec2(0.0);\n"
    FOG_CODE
    "}\n";
static const char *VS_OBJ_ADD = VS_OBJ_HEAD
    "void main() {\n"
    "  vec4 p0 = vec4(aPos, 1.0);\n"
    "  vec3 w = vec3(dot(uM0, p0), dot(uM1, p0), dot(uM2, p0));\n"
    "  vec4 p = vec4(w, 1.0);\n"
    "  gl_Position = vec4(dot(uVP0, p), dot(uVP1, p), dot(uVP2, p), dot(uVP3, p));\n"
    "  vUV = aUV;\n"
    "  vCol = uOFlag.x > 0.5 ? uOCol : vec4(aCol.rgb, aCol.a * 8.0);\n"
    "  vLit = vec3(0.0); vRef = vec2(0.0);\n"
    FOG_CODE
    "}\n";
/* phong_ref_intro.fx's vertex shader (the warp-in / warp-out stand-in, IntroCarLogic 0x68d0e0): each triangle
 * leaves for the portal (uPortal: direction and distance, model space) when its own t passes 0, bulging out along
 * its normal on the way; t = k + hash / 4 + (n.P / 2 + 1/2) / 2 + (pos.P / 2 + 1/2) / 4, k = 1.25 - 2.7 progress
 * (uIntro.x); the hash frac((3.1 + n.x) 243) comes in aLit.x. vT carries t to the glow */
static const char *VS_OBJ_INTRO = VS_OBJ_HEAD
    "uniform vec4 uIntro;\n" "uniform vec4 uPortal;\n" "varying float vT;\n"
    "vec3 plight(vec4 A, vec4 B, float amb, vec3 q, vec3 n) {\n"
    "  vec3 v = A.xyz - q;\n"
    "  float d = max(length(v), 0.001);\n"
    "  float att = clamp(A.w * (B.w - d) / (d * (B.w - A.w)), 0.0, 1.5);\n"
    "  return B.rgb * (att * (max(dot(n, v / d), 0.0) + amb));\n"
    "}\n"
    "void main() {\n"
    "  vec3 nn = aNrm.xyz;\n"
    "  float t = uIntro.x + 0.25 * aLit.x + 0.5 * (0.5 * dot(nn, uPortal.xyz) + 0.5) + 0.25 * (0.5 * dot(aPos, uPortal.xyz) + 0.5);\n"
    "  float ts = clamp(t, 0.0, 1.0);\n"
    "  vT = t;\n"
    "  vec4 p0 = vec4(aPos + nn * (5.0 * sin(3.14159265 * ts)) + uPortal.xyz * (ts * ts * uPortal.w), 1.0);\n"
    "  vec3 w = vec3(dot(uM0, p0), dot(uM1, p0), dot(uM2, p0));\n"
    "  vec4 p = vec4(w, 1.0);\n"
    "  gl_Position = vec4(dot(uVP0, p), dot(uVP1, p), dot(uVP2, p), dot(uVP3, p));\n"
    "  vUV = aUV;\n"
    "  vCol = vec4(aCol.rgb, aCol.a * 8.0);\n"
    "  vec3 n = normalize(vec3(dot(uM0.xyz, nn), dot(uM1.xyz, nn), dot(uM2.xyz, nn)));\n"
    "  vLit = uSun.rgb * (max(n.z, 0.0) + uSun.a) + plight(uLA0, uLB0, uLamb.x, w, n) + plight(uLA1, uLB1, uLamb.y, w, n)\n"
    "       + plight(uLA2, uLB2, uLamb.z, w, n) + plight(uLA3, uLB3, uLamb.w, w, n);\n"
    FOG_CODE SPHERE_CODE("uVFlag.x > 0.5")
    "}\n";
/* the tyre tracks (tire_track.fx), already in world space: vCol = (the ends' fade coordinate, fade, -, -),
 * vLit.xy = the track's glow and rainbow amounts (no fog: the PC's pixel shader has none) */
static const char *VS_TRACK = ATTRS VARYINGS FRAME_UNIFORMS
    "void main() {\n"
    "  vec4 p = vec4(aPos, 1.0);\n"
    "  gl_Position = vec4(dot(uVP0, p), dot(uVP1, p), dot(uVP2, p), dot(uVP3, p));\n"
    "  vUV = aUV; vCol = aCol;\n"
    "  vLit = vec3(aNrm.xy, 0.0); vRef = vec2(0.0); vFog = vec2(0.0);\n"
    "}\n";
/* tvscreen.fx on objects (uOFlag.z: the object's own speed factor, 1 + fract(0.211 id)) */
static const char *VS_OTV = ATTRS VARYINGS FRAME_UNIFORMS
    "uniform vec4 uM0;\n" "uniform vec4 uM1;\n" "uniform vec4 uM2;\n" "uniform vec4 uOCol;\n" "uniform vec4 uOFlag;\n"
    "void main() {\n"
    "  vec4 p0 = vec4(aPos, 1.0);\n"
    "  vec3 w = vec3(dot(uM0, p0), dot(uM1, p0), dot(uM2, p0));\n"
    "  vec4 p = vec4(w, 1.0);\n"
    "  gl_Position = vec4(dot(uVP0, p), dot(uVP1, p), dot(uVP2, p), dot(uVP3, p));\n"
    "  vUV = aUV;\n"
    "  vCol = uOFlag.x > 0.5 ? uOCol : vec4(aCol.rgb, aCol.a * 8.0);\n"
    "  vRef = aUV + uBeat.z * vec2(1.3, -3.0) * uOFlag.z;\n"
    "  vLit = vec3(aUV + uBeat.z * vec2(17.3, -120.123), 0.0);\n"
    FOG_CODE
    "}\n";

/* GroundLaser.fx (the laser floors under the last levels): the texture tiled in world space every
 * 40 units, a second look-up for the laser lines; vRef / vLit carry the two coordinates */
static const char *VS_GLASER = ATTRS VARYINGS FRAME_UNIFORMS
    "void main() {\n"
    "  vec3 w = aPos;\n"
    "  vec4 p = vec4(w, 1.0);\n"
    "  gl_Position = vec4(dot(uVP0, p), dot(uVP1, p), dot(uVP2, p), dot(uVP3, p));\n"
    "  vUV = aUV;\n"
    "  vCol = vec4(aCol.rgb, aCol.a * 8.0);\n"
    "  vRef = (w.xy + 30.0) * vec2(0.025, -0.025);\n"
    "  vLit = vec3(vRef * 0.5 + aUV * 3.0, 0.0);\n"
    FOG_CODE
    "}\n";

/* the sky cube around the camera */
static const char *VS_SKY = ATTRS VARYINGS FRAME_UNIFORMS
    "void main() {\n"
    "  vec4 p = vec4(aPos, 1.0);\n"
    "  gl_Position = vec4(dot(uVP0, p), dot(uVP1, p), dot(uVP2, p), dot(uVP3, p));\n"
    "  vUV = aUV; vCol = vec4(1.0); vLit = vec3(0.0); vFog = vec2(0.0); vRef = vec2(0.0);\n"
    "}\n";

#define FS_HEAD VARYINGS \
    "uniform sampler2D uDiff;\n" "uniform sampler2D uEmit;\n" "uniform sampler2D uRefl;\n" \
    "uniform vec4 uMat;\n" "uniform vec4 uTint;\n" "uniform vec3 uFogCol;\n" "uniform vec3 uSkyCol;\n" \
    "uniform vec4 uFT;\n" "uniform vec4 uMP;\n" "uniform vec4 uMC2;\n"
/* the scene texture keeps how bright a pixel really is (beyond 1.0) in alpha, as max/4, for the bloom */
#define FS_OUT \
    "  c = mix(c, mix(uSkyCol, uFogCol, vFog.x), vFog.y);\n" \
    "  gl_FragColor = vec4(c, max(c.r, max(c.g, c.b)) * 0.25);\n"

/* phong_emit / building / road: diffuse x light + emissive x colour x strength */
static const char *FS_LIT = FS_HEAD
    "void main() {\n"
    "  vec4 d = texture2D(uDiff, vUV);\n"
    "  vec4 e = texture2D(uEmit, vUV);\n"
    "  vec3 c = d.rgb * vLit + e.rgb * mix(vec3(1.0), vCol.rgb, e.a) * (vCol.a * uMat.x);\n"
    FS_OUT
    "}\n";
/* phong_ref (the car, checkpoints): plus the sphere-mapped reflection where the diffuse alpha
 * is above one half; the emissive colour can be overridden (uTint) */
static const char *FS_REF = FS_HEAD
    "void main() {\n"
    "  vec4 d = texture2D(uDiff, vUV);\n"
    "  vec4 e = texture2D(uEmit, vUV);\n"
    "  vec3 r = texture2D(uRefl, vRef).rgb;\n"
    "  vec3 tint = mix(vCol.rgb, uTint.rgb, uTint.a);\n"
    "  float rs = uMP.w > 0.0 ? uMP.w : (d.a - 0.5) * 2.0;\n"         /* phong_ref_spec: x ReflectStrength1 */
    "  vec3 c = d.rgb * vLit + e.rgb * mix(vec3(1.0), tint, e.a) * (vCol.a * uMat.x)\n"
    "         + clamp(r * (rs * uMat.y), 0.0, 1.0);\n"
    FS_OUT
    "}\n";
/* phong_ref_intro: the car as phong_ref, and the triangles' edges (TriangleEdges - 0.3) glowing: Color1 x (2.5, 2.5, 5)
 * while t is near 0.2, white-blue (7.5, 22.5, 30) once past 0.5 (the triangles gathered at the portal) */
static const char *FS_INTRO = FS_HEAD
    "varying float vT;\n" "uniform sampler2D uTri;\n"
    "void main() {\n"
    "  vec4 d = texture2D(uDiff, vUV);\n"
    "  vec4 e = texture2D(uEmit, vUV);\n"
    "  vec3 r = texture2D(uRefl, vRef).rgb;\n"
    "  vec3 tint = mix(vCol.rgb, uTint.rgb, uTint.a);\n"
    "  float rs = uMP.w > 0.0 ? uMP.w : (d.a - 0.5) * 2.0;\n"
    "  vec3 c = d.rgb * vLit + e.rgb * mix(vec3(1.0), tint, e.a) * (vCol.a * uMat.x)\n"
    "         + clamp(r * (rs * uMat.y), 0.0, 1.0);\n"
    "  vec3 eg = clamp(texture2D(uTri, vUV).rgb - 0.3, 0.0, 1.0);\n"
    "  float g1 = clamp(3.0 - 15.0 * (vT - 0.2) * (vT - 0.2), 0.0, 1.0);\n"
    "  c += (uMC2.rgb * vec3(2.5, 2.5, 5.0) * g1 + clamp(2.0 * vT - 1.0, 0.0, 1.0) * vec3(7.5, 22.5, 30.0)) * eg;\n"
    FS_OUT
    "}\n";
/* the tyre tracks' glow (tire_track.fx with its glow full): the sparkle (tread b) x the ends' fade (g) x 80, blue
 * (0.1, 0.4, 0.8) or, faster, the rainbow across the width; alpha-blended on the PC, added here (it is far
 * brighter than what it covers) */
static const char *FS_TRACK = FS_HEAD
    "void main() {\n"
    "  vec3 t0 = texture2D(uDiff, vUV).rgb;\n"
    "  float cap = texture2D(uDiff, vec2(vUV.x, vCol.r)).g;\n"
    "  vec3 rb = texture2D(uEmit, vec2(1.5 * vUV.x, 0.5)).rgb;\n"
    "  float k = t0.b * cap * 80.0;\n"
    "  float a = clamp(mix(0.5, 1.0, vLit.y) * k * vCol.g, 0.0, 1.0) * vLit.x;\n"
    "  vec3 c = mix(vec3(0.1, 0.4, 0.8), rb, vLit.y) * (k * a);\n"
    "  gl_FragColor = vec4(c, max(c.r, max(c.g, c.b)) * 0.25);\n"
    "}\n";
/* ... and the slower tracks' dark tread (0.8 x tread r x the ends' fade): the scene under it darkened (blend 0, 1 - a) */
static const char *FS_TRACKDARK = FS_HEAD
    "void main() {\n"
    "  float r = texture2D(uDiff, vUV).r;\n"
    "  float cap = texture2D(uDiff, vec2(vUV.x, vCol.r)).g;\n"
    "  gl_FragColor = vec4(0.0, 0.0, 0.0, clamp(0.8 * cap * r * vCol.g, 0.0, 1.0) * (1.0 - vLit.x));\n"
    "}\n";
/* tvscreen (posters, screens): the picture shines by itself (35%, red posters 50%), with
 * scrolling static from Emit1 (slow grain uMP.x, fast bands uMP.y); not lit */
static const char *FS_TV = FS_HEAD
    "void main() {\n"
    "  vec4 d = texture2D(uDiff, vUV);\n"
    "  float n1 = texture2D(uEmit, vRef).g; vec3 n2 = texture2D(uEmit, vLit.xy).rgb;\n"
    "  vec3 c = d.rgb * (uMat.z * vCol.a * uMat.x) + vec3(uMP.x * n1 + uMP.y * (n2.r - n2.b));\n"
    FS_OUT
    "}\n";
/* phong_newpulse (ramps, lava): Emit1.r glows in Color1, the chevrons of Emit1.g light up in
 * Color2 one after another in the order of Emit1.b (uMP.x = phase, uMP.y = band sharpness) */
static const char *FS_PULSE = FS_HEAD
    "void main() {\n"
    "  vec4 d = texture2D(uDiff, vUV);\n"
    "  vec4 e = texture2D(uEmit, vUV);\n"
    "  float k = clamp(1.0 - abs(abs(e.b - uMP.x) - 0.5) * uMP.y, 0.0, 1.0) * e.g;\n"
    "  vec3 c = d.rgb * vLit + e.r * vCol.rgb * (vCol.a * uMat.x) + k * uMC2.rgb * uMC2.w;\n"
    FS_OUT
    "}\n";
/* phong_emit_anim (turbines): plus green bands crawling along Anim1's three phase maps */
static const char *FS_ANIM = FS_HEAD
    "void main() {\n"
    "  vec4 d = texture2D(uDiff, vUV);\n"
    "  vec4 e = texture2D(uEmit, vUV);\n"
    "  vec4 an = texture2D(uRefl, vUV);\n"
    "  vec3 c = d.rgb * vLit + e.rgb * vCol.rgb * (vCol.a * uMat.x);\n"
    "  vec3 b = max(1.0 - 12.5 * abs(abs(an.rgb - uFT.w) - 0.5), 0.0);\n"
    "  c += clamp(an.a - 0.7, 0.0, 1.0) * 20.0 * (b.r + b.g + b.b) * vec3(0.2, 0.8, 0.2);\n"
    FS_OUT
    "}\n";
/* tronics (the ground plane): Color2 x the circuit, pulses along its traces (Tron1 on unit 1) */
static const char *FS_TRON = FS_HEAD
    "void main() {\n"
    "  vec4 d = texture2D(uDiff, vUV);\n"
    "  vec4 a = texture2D(uEmit, vUV);\n"
    "  vec3 b = clamp(1.0 - abs(abs(a.rgb - uFT.z) - 0.5) * uMP.x, 0.0, 1.0);\n"
    "  vec3 c = uMC2.rgb * uMC2.w * d.rgb\n"
    "         + (vCol.rgb * dot(vec3(0.2, 0.8, 0.5), b) + uMC2.rgb * dot(vec3(0.8, 0.2, 0.5), b)) * d.a * (vCol.a * uMat.x);\n"
    FS_OUT
    "}\n";
/* city (the backdrop city, unlit): tron bands creep round its base, the buildings' edges and
 * window cells show Color2 x the texture. vUV = the object's x, y seen from above; vCol.r = its
 * height / 0.5, vCol.gb = the mesh's own coordinates; uMP = Color1 */
static const char *FS_CITY = FS_HEAD
    "void main() {\n"
    "  vec4 d = texture2D(uDiff, vUV);\n"
    "  vec4 a = texture2D(uEmit, vUV);\n"
    "  vec3 b = clamp(1.0 - abs(abs(a.rgb - uFT.z) - 0.5) * 10.0, 0.0, 1.0);\n"
    "  float z = vCol.r * 0.5;\n"
    "  vec3 c = (uMP.rgb * dot(vec3(0.2, 0.8, 0.5), b) + uMC2.rgb * dot(vec3(0.8, 0.2, 0.5), b)) * (uMP.w * uMat.x) * a.a * clamp(1.0 - 15.0 * z, 0.0, 1.0);\n"
    "  vec2 m = vCol.gb, po = abs((vUV - 0.5) * 2.0);\n"
    "  float edge = (m.x < 0.05 || m.y < 0.05 || m.x > 0.95 || m.y > 0.95) ? 1.0 : 0.0;\n"
    "  vec2 q = po * 100.0; q -= floor(q); float zc = z * 50.0; zc -= floor(zc);\n"     /* fract / mod spelt out */
    "  float win = (q.x > 0.05 && q.y > 0.05 && zc > 0.35) ? 0.25 : 0.0;\n"
    "  c += d.rgb * uMC2.rgb * uMC2.w * max(edge, win);\n"
    FS_OUT
    "}\n";
/* additive: texture x colour x strength, faded by the fog */
static const char *FS_ADD = FS_HEAD
    "void main() {\n"
    "  vec4 t = texture2D(uDiff, vUV);\n"
    "  vec3 tc = mix(t.rgb, vec3(dot(t.rgb, vec3(1.0, 0.5, 0.2))), uMat.w);\n"
    "  vec3 c = tc * vCol.rgb * (vCol.a * uMat.x * (1.0 - vFog.y));\n"
    "  gl_FragColor = vec4(c, max(c.r, max(c.g, c.b)) * 0.25);\n"
    "}\n";
/* pulse_veins (additive): the veins flash on the music's beat and fade in 0.275 s; a thin band
 * sweeps down the green gradient (uMP.x = band position, uMP.y = flash) */
static const char *FS_VEINS = FS_HEAD
    "void main() {\n"
    "  vec4 t = texture2D(uDiff, vUV);\n"
    "  float a = 1.0 - 10.0 * abs(abs(t.g - uMP.x) - 0.5);\n"
    "  vec3 c = clamp(t.g * t.r * uMP.y * vCol.rgb, 0.0, 1.0) * (vCol.a * uMat.x)\n"
    "         + (a >= 0.0 ? clamp(t.g * a * t.r * uMC2.rgb, 0.0, 1.0) * uMC2.w : vec3(0.0));\n"
    "  gl_FragColor = vec4(c, max(c.r, max(c.g, c.b)) * 0.25);\n"
    "}\n";
/* traffic.fx / jet_flame.fx: the texture's red channel, masked by two scrolling copies of its
 * other channels (moving streaks of traffic lights, flickering flames) */
static const char *FS_SCROLL = FS_HEAD
    "uniform vec4 uSc;\n" "uniform vec4 uOff;\n" "uniform vec4 uC1;\n" "uniform vec4 uC2;\n"
    "void main() {\n"
    "  vec4 a = texture2D(uDiff, vUV * uSc.x + uOff.xy * uSc.w);\n"
    "  vec4 b = texture2D(uDiff, vUV * uSc.y + uOff.zw * uSc.w);\n"
    "  float m = max(dot(a, uC1) + dot(b, uC2) + uSc.z, 0.0);\n"
    "  vec3 c = texture2D(uDiff, vUV).r * m * vCol.rgb * (vCol.a * uMat.x * (1.0 - vFog.y));\n"
    "  gl_FragColor = vec4(c, max(c.r, max(c.g, c.b)) * 0.25);\n"
    "}\n";
/* GroundLaser.fx: glow masks from the red and green channels (faded towards the edges of the
 * mesh), laser lines from blue, x Color1 */
static const char *FS_GLASER = FS_HEAD
    "void main() {\n"
    "  vec2 q = (vUV - 0.5) * 2.0; float r = max(1.0 - dot(q, q), 0.0); r *= r;\n"
    "  vec4 t = texture2D(uDiff, vRef);\n"
    "  float m = (clamp(r + t.r - 1.0, 0.0, 1.0) + clamp(r + t.g - 1.0, 0.0, 1.0)) * vCol.a * uMat.x;\n"
    "  vec3 c = m * texture2D(uDiff, vLit.xy).b * vCol.rgb * (1.0 - vFog.y);\n"
    "  gl_FragColor = vec4(c, max(c.r, max(c.g, c.b)) * 0.25);\n"
    "}\n";
/* skybox: the faces' picture x the sky colour (Color1 rgb x a x 2) */
static const char *FS_SKY = FS_HEAD
    "void main() {\n"
    "  vec3 c = texture2D(uDiff, vUV).rgb * uTint.rgb;\n"
    "  gl_FragColor = vec4(c, max(c.r, max(c.g, c.b)) * 0.25);\n"
    "}\n";

/* screen passes. The full-screen ones fetch only at coordinates that come straight from the
 * vertex shader (no "dependent" reads, which are slow on the Vita's PowerVR GPU) */
static const char *VS_POST =
    "attribute vec2 aPos;\n" "attribute vec2 aUV;\n" "varying vec2 vUV;\n"
    "void main() { gl_Position = vec4(aPos, 0.0, 1.0); vUV = aUV; }\n";
/* the radial blur's taps, worked out at the corners of a 16x9 grid (the blur grows with the
 * distance from the centre, so the car stays sharp) */
static const char *VS_POST_SPEED =
    "attribute vec2 aPos;\n" "attribute vec2 aUV;\n" "uniform vec4 uStep;\n" "uniform vec4 uScale;\n"
    "varying vec2 vUV;\n" "varying vec2 vS1;\n" "varying vec2 vS2;\n" "varying vec2 vS3;\n" "varying vec2 vS4;\n"
    "void main() {\n"
    "  gl_Position = vec4(aPos, 0.0, 1.0); vUV = aUV * uScale.xy;\n"
    "  vec2 q = aUV - 0.5; vec2 d = q * (length(q) * uStep.x * 0.18);\n"
    "  vS1 = (aUV - d) * uScale.xy; vS2 = (aUV + d) * uScale.xy; vS3 = (aUV - 2.0 * d) * uScale.xy; vS4 = (aUV + 2.0 * d) * uScale.xy;\n"
    "}\n";
/* scene -> quarter size (4x4 texels each through 4 filtered taps), bright parts only, / 4 */
static const char *FS_DOWN =
    "varying vec2 vUV;\n" "uniform sampler2D uTex;\n" "uniform vec4 uStep;\n"
    "vec3 dec(vec4 c) { float m = max(c.r, max(c.g, c.b)); return c.rgb * min(c.a * 4.0 / max(m, 0.004), 2.5); }\n"
    "void main() {\n"
    "  vec3 s = dec(texture2D(uTex, vUV + vec2(-uStep.x, -uStep.y))) + dec(texture2D(uTex, vUV + vec2(uStep.x, -uStep.y)))\n"
    "         + dec(texture2D(uTex, vUV + vec2(-uStep.x, uStep.y))) + dec(texture2D(uTex, vUV + vec2(uStep.x, uStep.y)));\n"
    "  gl_FragColor = vec4(max(s * 0.25 - uStep.z, 0.0) * 0.25, 1.0);\n"
    "}\n";
/* one-pass blur: 9 filtered taps 1.5 texels apart (about 5x5 texels), with the sixteenth-size wide
 * glow added in (weights uStep.zw), stored at half so the sum fits: the final pass then reads one
 * glow texture instead of two */
static const char *FS_BLUR =
    "varying vec2 vUV;\n" "uniform sampler2D uTex;\n" "uniform sampler2D uB2;\n" "uniform vec4 uStep;\n"
    "void main() {\n"
    "  vec2 a = uStep.xy * 1.5, b = vec2(a.x, -a.y);\n"
    "  vec3 c = texture2D(uTex, vUV).rgb * 0.2\n"
    "    + (texture2D(uTex, vUV + vec2(a.x, 0.0)).rgb + texture2D(uTex, vUV - vec2(a.x, 0.0)).rgb\n"
    "     + texture2D(uTex, vUV + vec2(0.0, a.y)).rgb + texture2D(uTex, vUV - vec2(0.0, a.y)).rgb) * 0.12\n"
    "    + (texture2D(uTex, vUV + a).rgb + texture2D(uTex, vUV - a).rgb + texture2D(uTex, vUV + b).rgb + texture2D(uTex, vUV - b).rgb) * 0.08;\n"
    "  gl_FragColor = vec4((c * uStep.z + texture2D(uB2, vUV).rgb * uStep.w) * 0.5, 1.0);\n"
    "}\n";
/* quarter -> sixteenth: the 4x4 block plus a wider ring, for the big soft glow */
static const char *FS_WIDE =
    "varying vec2 vUV;\n" "uniform sampler2D uTex;\n" "uniform vec4 uStep;\n"
    "void main() {\n"
    "  vec2 a = uStep.xy, b = vec2(a.x, -a.y), c3 = uStep.xy * 3.5;\n"
    "  vec3 c = texture2D(uTex, vUV).rgb * 0.16\n"
    "    + (texture2D(uTex, vUV + a).rgb + texture2D(uTex, vUV - a).rgb + texture2D(uTex, vUV + b).rgb + texture2D(uTex, vUV - b).rgb) * 0.12\n"
    "    + (texture2D(uTex, vUV + vec2(c3.x, 0.0)).rgb + texture2D(uTex, vUV - vec2(c3.x, 0.0)).rgb\n"
    "     + texture2D(uTex, vUV + vec2(0.0, c3.y)).rgb + texture2D(uTex, vUV - vec2(0.0, c3.y)).rgb) * 0.09;\n"
    "  gl_FragColor = vec4(c, 1.0);\n"
    "}\n";
/* the final pass's grid: the screen edges' tint worked out per vertex */
static const char *VS_POST_EDGE =
    "attribute vec2 aPos;\n" "attribute vec2 aUV;\n" "uniform vec4 uEdge;\n" "uniform vec4 uScale;\n"
    "varying vec2 vUV;\n" "varying vec2 vSc;\n" "varying vec4 vE;\n"
    "void main() {\n"
    "  gl_Position = vec4(aPos, 0.0, 1.0); vUV = aUV; vSc = aUV * uScale.xy;\n"
    "  vec2 q = (aUV - 0.5) * vec2(1.7647, 1.0);\n"
    "  vE = vec4(uEdge.rgb, clamp((dot(q, q) - 0.12) * uEdge.a * 3.0, 0.0, 1.0));\n"
    "}\n";
/* the final picture (screenfinal2): scene + bloom, screen edges towards a colour, flash; with
 * SPEED, the scene smeared out from the centre (the PC game's radial blur at speed) */
#define COMP_HEAD \
    "varying vec2 vUV;\n" "varying vec2 vSc;\n" "varying vec4 vE;\n" "uniform sampler2D uTex;\n" "uniform sampler2D uB1;\n" \
    "uniform vec4 uBloom;\n" "uniform vec4 uFx;\n"
#define COMP_TAIL \
    "  c += texture2D(uB1, vUV).rgb * uBloom.w;\n" \
    "  c *= 1.0 + uFx.z;\n" \
    "  c = mix(c, vE.rgb, vE.a);\n" \
    "  c += uFx.y * vec3(0.55, 0.75, 1.0);\n" \
    "  gl_FragColor = vec4(c, 1.0);\n"
static const char *FS_COMP = COMP_HEAD
    "void main() {\n"
    "  vec3 c = texture2D(uTex, vSc).rgb;\n"
    COMP_TAIL
    "}\n";
/* the speed blur: the scene at half size (one filtered tap = 2x2 texels), smeared out from the
 * centre at half size (five taps), then mixed into the sharp picture by how long the smear is
 * there (the centre stays sharp). Full-size reads of the scene stay one per pixel: scattered
 * reads of the big scene texture were what made the blur cost 11 ms */
static const char *FS_HALF =
    "varying vec2 vUV;\n" "uniform sampler2D uTex;\n"
    "void main() { gl_FragColor = texture2D(uTex, vUV); }\n";
static const char *FS_RADIAL =
    "varying vec2 vUV;\n" "varying vec2 vS1;\n" "varying vec2 vS2;\n" "varying vec2 vS3;\n" "varying vec2 vS4;\n"
    "uniform sampler2D uTex;\n"
    "void main() {\n"
    "  gl_FragColor = texture2D(uTex, vUV) * 0.2857 + (texture2D(uTex, vS1) + texture2D(uTex, vS2)) * 0.2143\n"
    "               + (texture2D(uTex, vS3) + texture2D(uTex, vS4)) * 0.1429;\n"
    "}\n";
static const char *VS_POST_MIX =
    "attribute vec2 aPos;\n" "attribute vec2 aUV;\n" "uniform vec4 uStep;\n" "uniform vec4 uEdge;\n" "uniform vec4 uScale;\n"
    "varying vec2 vUV;\n" "varying vec2 vSc;\n" "varying float vW;\n" "varying vec4 vE;\n"
    "void main() {\n"
    "  gl_Position = vec4(aPos, 0.0, 1.0); vUV = aUV; vSc = aUV * uScale.xy;\n"
    "  vec2 q = aUV - 0.5; vec2 d = q * (length(q) * uStep.x * 0.18);\n"
    "  vW = clamp((length(d * vec2(960.0, 544.0)) - 3.0) * 0.2, 0.0, 1.0);\n"
    "  vec2 e = q * vec2(1.7647, 1.0);\n"
    "  vE = vec4(uEdge.rgb, clamp((dot(e, e) - 0.12) * uEdge.a * 3.0, 0.0, 1.0));\n"
    "}\n";
static const char *FS_COMP_SPEED = COMP_HEAD
    "varying float vW;\n" "uniform sampler2D uB3;\n"
    "void main() {\n"
    "  vec3 c = mix(texture2D(uTex, vSc).rgb, texture2D(uB3, vUV).rgb, vW);\n"
    COMP_TAIL
    "}\n";

typedef struct {
    GLuint prog;
    GLint uVP[4], uEye, uSun, uFog, uSky, uV[3], uM[3], uLA[4], uLB[4], uLamb;
    GLint uMat, uTint, uFogCol, uSkyCol;
    GLint uStep, uBloom, uEdge, uFx, uScale;
    GLint uSc, uOff, uC1, uC2;
    GLint uBeat, uOCol, uOFlag;
    GLint uFT, uMP, uMC2, uVFlag;
    GLint uRB, uRB2, uRB3, uRBE;
    GLint uIntro, uPortal;
    int stamp;                /* vp_id its frame uniforms were last set for */
} prog_t;
enum { P_WORLD, P_WREF, P_WTV, P_WADD, P_WSCROLL, P_WGLASER, P_SKY, P_OBJ, P_OBJADD, P_OBJSCROLL, P_OBJTV,
       P_WPULSE, P_WVEINS, P_WANIM, P_WTRON, P_WCITY, P_OBJPULSE, P_OBJVEINS, P_OBJANIM, P_OBJLIT,
       P_DOWN, P_BLUR, P_WIDE, P_COMP, P_HALF, P_RADIAL, P_COMP_SPEED,
       P_OBJINTRO, P_TRACK, P_TRACKDARK, P_COUNT };       /* (after the screen passes: no cinematics' variants) */
static int prog_additive(int p) { return p == P_WADD || p == P_WSCROLL || p == P_WGLASER || p == P_WVEINS || p == P_OBJADD || p == P_OBJSCROLL || p == P_OBJVEINS; }
static prog_t P[P_COUNT];
static GLuint tex_white, tex_black, tex_glow;
/* the 3D programs' sources (for the cinematics' variants) and the variants themselves */
static const char *SRC_VS[P_COUNT], *SRC_FS[P_COUNT], *SRC_NAME[P_COUNT];
static prog_t PRB[P_COUNT];
gfx_redblue_t gfx_redblue;
static prog_t *prog_get(int i) { return gfx_redblue.on && i < P_DOWN && PRB[i].prog ? &PRB[i] : &P[i]; }

#ifdef NR_PC
#define FS_PREFIX "precision mediump float;\n"
#else
#define FS_PREFIX ""
#endif
static int compile(GLuint sh, const char *src, const char *what, int frag)
{
#ifdef NR_PC
    const char *srcs[3] = { frag ? FS_PREFIX : "", NRPC_VGL_MACROS, src };
    glShaderSource(sh, 3, srcs, NULL);
#else
    (void)frag;
    glShaderSource(sh, 1, &src, NULL);
#endif
    glCompileShader(sh);
    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[1024] = ""; GLsizei n = 0;
        glGetShaderInfoLog(sh, sizeof buf - 1, &n, buf);
        logm("  %s shader failed to compile: %s", what, buf);
    }
    return ok;
}
/* compiled programs are kept in ux0:data/nitronic/cache: compiling them all takes the Vita
 * about ten seconds at every start otherwise */
static int cache_loaded, cache_built;
#ifndef NR_PC
static uint32_t src_hash(const char *a, const char *b, const char *c)
{
    uint32_t h = 2166136261u;
    for (const char *s = a; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    h = (h ^ 0xFF) * 16777619u;
    for (const char *s = b; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    for (const char *s = c; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}
#endif
static void bind_attribs(GLuint prog, int post)
{
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aUV");
    if (!post) {
        glBindAttribLocation(prog, 2, "aNrm");
        glBindAttribLocation(prog, 3, "aCol");
        glBindAttribLocation(prog, 4, "aLit");
    }
}
static int build(prog_t *o, const char *vsrc, const char *fsrc, const char *name, int post)
{
    GLint linked = 0;
    if (o >= P && o < P + P_COUNT) { SRC_VS[o - P] = vsrc; SRC_FS[o - P] = fsrc; SRC_NAME[o - P] = name; }
    if (gfx_load_tick) gfx_load_tick();                /* (the loading screen, while they compile) */
#ifndef NR_PC
    char path[160];
    snprintf(path, sizeof path, "ux0:data/nitronic/cache/%08x.prg", (unsigned)src_hash(vsrc, fsrc, name));
    FILE *f = fopen(path, "rb");
    if (f) {
        GLenum fmt = 0; GLsizei len = 0; void *bin = NULL;
        if (fread(&fmt, 4, 1, f) == 1 && fread(&len, 4, 1, f) == 1 && len > 0 && len < (8 << 20) && (bin = malloc(len)) && fread(bin, 1, len, f) == (size_t)len) {
            o->prog = glCreateProgram();          /* the binary carries its attribute bindings */
            glProgramBinary(o->prog, fmt, bin, len);
            glGetProgramiv(o->prog, GL_LINK_STATUS, &linked);
            if (!linked) { glDeleteProgram(o->prog); o->prog = 0; }
        }
        free(bin); fclose(f);
        if (linked) cache_loaded++;
    }
#endif
    if (!linked) {
        GLuint vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER);
        if (!compile(vs, vsrc, name, 0) || !compile(fs, fsrc, name, 1)) { logm("  program %s failed", name); return 0; }
        o->prog = glCreateProgram();
        glAttachShader(o->prog, vs); glAttachShader(o->prog, fs);
        bind_attribs(o->prog, post);
        glLinkProgram(o->prog);
        glGetProgramiv(o->prog, GL_LINK_STATUS, &linked);
        if (!linked) {
            char buf[1024] = ""; GLsizei n = 0;
            glGetProgramInfoLog(o->prog, sizeof buf - 1, &n, buf);
            logm("  program %s: link failed %s", name, buf); return 0;
        }
        cache_built++;
#ifndef NR_PC
        GLint blen = 0; glGetProgramiv(o->prog, GL_PROGRAM_BINARY_LENGTH, &blen);
        if (blen <= 0) blen = 1 << 20;
        void *bin = malloc(blen); GLsizei len = 0; GLenum fmt = 0;
        if (bin) {
            glGetProgramBinary(o->prog, blen, &len, &fmt, bin);
            sceIoMkdir("ux0:data/nitronic/cache", 0777);
            FILE *w = len > 0 ? fopen(path, "wb") : NULL;
            if (w) { fwrite(&fmt, 4, 1, w); fwrite(&len, 4, 1, w); fwrite(bin, 1, len, w); fclose(w); }
            free(bin);
        }
#endif
    }
    glUseProgram(o->prog);
    char nm[16];
    for (int i = 0; i < 4; i++) {
        snprintf(nm, sizeof nm, "uVP%d", i); o->uVP[i] = glGetUniformLocation(o->prog, nm);
        snprintf(nm, sizeof nm, "uLA%d", i); o->uLA[i] = glGetUniformLocation(o->prog, nm);
        snprintf(nm, sizeof nm, "uLB%d", i); o->uLB[i] = glGetUniformLocation(o->prog, nm);
    }
    for (int i = 0; i < 3; i++) {
        snprintf(nm, sizeof nm, "uV%d", i); o->uV[i] = glGetUniformLocation(o->prog, nm);
        snprintf(nm, sizeof nm, "uM%d", i); o->uM[i] = glGetUniformLocation(o->prog, nm);
    }
    o->uLamb = glGetUniformLocation(o->prog, "uLamb");
    o->uEye = glGetUniformLocation(o->prog, "uEye");
    o->uSun = glGetUniformLocation(o->prog, "uSun");
    o->uFog = glGetUniformLocation(o->prog, "uFog");
    o->uSky = glGetUniformLocation(o->prog, "uSky");
    o->uMat = glGetUniformLocation(o->prog, "uMat");
    o->uTint = glGetUniformLocation(o->prog, "uTint");
    o->uFogCol = glGetUniformLocation(o->prog, "uFogCol");
    o->uSkyCol = glGetUniformLocation(o->prog, "uSkyCol");
    o->uStep = glGetUniformLocation(o->prog, "uStep");
    o->uBloom = glGetUniformLocation(o->prog, "uBloom");
    o->uEdge = glGetUniformLocation(o->prog, "uEdge");
    o->uScale = glGetUniformLocation(o->prog, "uScale");
    o->uFx = glGetUniformLocation(o->prog, "uFx");
    o->uSc = glGetUniformLocation(o->prog, "uSc");
    o->uOff = glGetUniformLocation(o->prog, "uOff");
    o->uC1 = glGetUniformLocation(o->prog, "uC1");
    o->uC2 = glGetUniformLocation(o->prog, "uC2");
    o->uBeat = glGetUniformLocation(o->prog, "uBeat");
    o->uOCol = glGetUniformLocation(o->prog, "uOCol");
    o->uOFlag = glGetUniformLocation(o->prog, "uOFlag");
    o->uFT = glGetUniformLocation(o->prog, "uFT");
    o->uMP = glGetUniformLocation(o->prog, "uMP");
    o->uMC2 = glGetUniformLocation(o->prog, "uMC2");
    o->uVFlag = glGetUniformLocation(o->prog, "uVFlag");
    o->uIntro = glGetUniformLocation(o->prog, "uIntro"); o->uPortal = glGetUniformLocation(o->prog, "uPortal");
    o->uRB = glGetUniformLocation(o->prog, "uRB"); o->uRB2 = glGetUniformLocation(o->prog, "uRB2"); o->uRB3 = glGetUniformLocation(o->prog, "uRB3"); o->uRBE = glGetUniformLocation(o->prog, "uRBE");
    static const char *samplers[] = { "uDiff", "uEmit", "uRefl", "uTex", "uB1", "uB2", "uB3", "uTri" };
    static const int units[] = { 0, 1, 2, 0, 1, 2, 3, 3 };
    for (int i = 0; i < 8; i++) { GLint s = glGetUniformLocation(o->prog, samplers[i]); if (s >= 0) glUniform1i(s, units[i]); }
    return 1;
}
static int make_programs(void)
{
    logm("building GLSL shaders");
    int ok = build(&P[P_WORLD], VS_WORLD_NR, FS_LIT, "world", 0)
          && build(&P[P_WREF], VS_WORLD, FS_REF, "world-reflect", 0)
          && build(&P[P_WTV], VS_WTV, FS_TV, "world-screen", 0)
          && build(&P[P_WADD], VS_WORLD_NR, FS_ADD, "world-additive", 0)
          && build(&P[P_WSCROLL], VS_WORLD_NR, FS_SCROLL, "world-scrolling", 0)
          && build(&P[P_WGLASER], VS_GLASER, FS_GLASER, "ground-laser", 0)
          && build(&P[P_OBJSCROLL], VS_OBJ_ADD, FS_SCROLL, "object-scrolling", 0)
          && build(&P[P_SKY], VS_SKY, FS_SKY, "sky", 0)
          && build(&P[P_OBJ], VS_OBJ, FS_REF, "object", 0)
          && build(&P[P_OBJADD], VS_OBJ_ADD, FS_ADD, "object-additive", 0)
          && build(&P[P_OBJTV], VS_OTV, FS_TV, "object-screen", 0)
          && build(&P[P_WPULSE], VS_WORLD_NR, FS_PULSE, "world-pulse", 0)
          && build(&P[P_WVEINS], VS_WORLD_NR, FS_VEINS, "world-veins", 0)
          && build(&P[P_WANIM], VS_WORLD_NR, FS_ANIM, "world-anim", 0)
          && build(&P[P_WTRON], VS_WORLD_NR, FS_TRON, "world-tronics", 0)
          && build(&P[P_WCITY], VS_WUNLIT, FS_CITY, "world-city", 0)
          && build(&P[P_OBJPULSE], VS_OBJ_NR, FS_PULSE, "object-pulse", 0)
          && build(&P[P_OBJVEINS], VS_OBJ_ADD, FS_VEINS, "object-veins", 0)
          && build(&P[P_OBJANIM], VS_OBJ_NR, FS_ANIM, "object-anim", 0)
          && build(&P[P_OBJLIT], VS_OBJ_NR, FS_LIT, "object-lit", 0)
          && build(&P[P_OBJINTRO], VS_OBJ_INTRO, FS_INTRO, "object-warp", 0)
          && build(&P[P_TRACK], VS_TRACK, FS_TRACK, "tyre-tracks", 0)
          && build(&P[P_TRACKDARK], VS_TRACK, FS_TRACKDARK, "tyre-tracks-dark", 0);
    if (!ok) return 0;
    logm("GLSL shaders ready (%d from the cache, %d compiled)", cache_loaded, cache_built);
    return 1;
}
/* the cinematics' variants (re_O 7.2, applied in the scene's own shaders: no depth texture to read here): the vertex
 * shader passes the distance from the CORE's eye, the fragment shader's colour goes through nrRB(). The distance is
 * interpolated across a triangle: close to exact away from the eye, where the sphere's edge is */
static char *str_sub(const char *s, const char *from, const char *to, int last)
{
    const char *at = NULL;
    for (const char *q = strstr(s, from); q; q = strstr(q + 1, from)) { at = q; if (!last) break; }
    if (!at) return NULL;
    size_t a = (size_t)(at - s), lf = strlen(from), lt = strlen(to), ls = strlen(s);
    char *o = malloc(ls - lf + lt + 1);
    if (!o) return NULL;
    memcpy(o, s, a); memcpy(o + a, to, lt); memcpy(o + a + lt, at + lf, ls - a - lf + 1);
    return o;
}
static const char *RB_FS_HEAD =
    "varying float vD;\n" "uniform vec4 uRB;\n" "uniform vec4 uRB2;\n" "uniform vec4 uRB3;\n"
    "vec4 nrRB(vec4 o) {\n"
    "  float x = clamp((vD * uRB.w - 0.8) * 2.5, 0.0, 1.0);\n"
    "  float inner = x * x * (3.0 - 2.0 * x);\n"
    "  float ring = clamp(0.3 * abs(vD - uRB2.x + 30.0) - 0.6, 0.0, 1.0);\n"
    "  vec3 C = o.rgb;\n"
    "  vec3 base = mix(C, C.bgr * 0.8 + uRB2.yzw, inner);\n"
    "  vec3 c = mix(2.0 * C + uRB3.xyz, base, ring);\n"
    "  return vec4(c, max(c.r, max(c.g, c.b)) * 0.25);\n"
    "}\n";
int gfx_redblue_init(void)
{
    static int done; if (done) return done > 0;
    done = -1;
    int built = 0;
    for (int i = 0; i < P_DOWN; i++) {
        if (!SRC_VS[i] || !SRC_FS[i]) continue;
        const char *vend = i == P_SKY ? "  vD = 60000.0 + uRBE.w;\n}\n" : "  vD = distance(w, uRBE.xyz);\n}\n";
        char *v1 = str_sub(SRC_VS[i], "}\n", vend, 1);
        char *vs = v1 ? malloc(strlen(v1) + 64) : NULL;
        if (vs) { strcpy(vs, "varying float vD;\nuniform vec4 uRBE;\n"); strcat(vs, v1); }
        /* gl_FragColor = X;  ->  gl_FragColor = nrRB(X); */
        char *f1 = str_sub(SRC_FS[i], "gl_FragColor = ", "gl_FragColor = nrRB(", 0), *f2 = NULL, *fs = NULL;
        if (f1) { char *q = strstr(f1, "gl_FragColor = nrRB("); char *sc = q ? strchr(q, ';') : NULL;
                  if (sc) { size_t a = (size_t)(sc - f1); f2 = malloc(strlen(f1) + 2); if (f2) { memcpy(f2, f1, a); f2[a] = ')'; strcpy(f2 + a + 1, sc); } } }
        if (f2) { fs = malloc(strlen(f2) + strlen(RB_FS_HEAD) + 1); if (fs) { strcpy(fs, RB_FS_HEAD); strcat(fs, f2); } }
        char nm[48]; snprintf(nm, sizeof nm, "cinema-%s", SRC_NAME[i] ? SRC_NAME[i] : "?");
        if (vs && fs && build(&PRB[i], vs, fs, nm, 0)) built++;
        free(v1); free(vs); free(f1); free(f2); free(fs);
    }
    logm("cinematics' screen effect: %d programs", built);
    done = built ? 1 : -1;
    return built > 0;
}
static int post_ok;
static int make_post_programs(void)
{
    return build(&P[P_DOWN], VS_POST, FS_DOWN, "downscale", 1)
        && build(&P[P_BLUR], VS_POST, FS_BLUR, "blur", 1)
        && build(&P[P_WIDE], VS_POST, FS_WIDE, "wide-glow", 1)
        && build(&P[P_COMP], VS_POST_EDGE, FS_COMP, "final", 1)
        && build(&P[P_HALF], VS_POST, FS_HALF, "half", 1)
        && build(&P[P_RADIAL], VS_POST_SPEED, FS_RADIAL, "radial-blur", 1)
        && build(&P[P_COMP_SPEED], VS_POST_MIX, FS_COMP_SPEED, "final-speed", 1);
}

static GLuint solid_texture(uint8_t v)
{
    uint8_t px[4 * 4 * 4]; memset(px, v, sizeof px);
    GLuint id; glGenTextures(1, &id); glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return id;
}
static GLuint glow_texture(void)
{
    static uint8_t px[64 * 64 * 4];
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        float dx = (x - 31.5f) / 31.5f, dy = (y - 31.5f) / 31.5f, r = sqrtf(dx * dx + dy * dy);
        float v = r >= 1 ? 0 : (1 - r) * (1 - r);
        uint8_t c = (uint8_t)(v * 255);
        px[(y * 64 + x) * 4 + 0] = px[(y * 64 + x) * 4 + 1] = px[(y * 64 + x) * 4 + 2] = c; px[(y * 64 + x) * 4 + 3] = 255;
    }
    GLuint id; glGenTextures(1, &id); glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return id;
}

/* ------------------------------------------------------------------ render targets */
typedef struct { GLuint fbo, tex, depth; int w, h; } target_t;
/* quarter, quarter blurred, sixteenth, quarter (the speed blur's own: a render target drawn into twice in a frame
 * makes the Vita wait for the GPU in between, so no target is used for two passes) */
static target_t T_SCENE, T_B[4];
static int make_target(target_t *t, int w, int h, int depth)
{
    t->w = w; t->h = h;
    glGenTextures(1, &t->tex); glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &t->fbo); glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    if (depth) {
        glGenRenderbuffers(1, &t->depth); glBindRenderbuffer(GL_RENDERBUFFER, t->depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t->depth);
    }
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) { logm("render target %dx%d incomplete (0x%x)", w, h, st); return 0; }
    return 1;
}

int gfx_init(void)
{
    if (!make_programs()) return -1;
    glCullFace(GL_BACK); glFrontFace(GL_CCW);   /* the meshes' front faces are counter-clockwise */
    glDepthFunc(GL_LEQUAL);
    tex_white = solid_texture(255); tex_black = solid_texture(0); tex_glow = glow_texture();
    post_ok = 0;
    if (!GSW.no_post && make_post_programs() && make_target(&T_SCENE, SW, SH, 1)) {
        post_ok = 1;
        for (int i = 0; i < 4 && post_ok; i++)
            post_ok = make_target(&T_B[i], i != 2 ? SW / 4 : SW / 16, i != 2 ? SH / 4 : SH / 16, 0);
        /* (no half-size targets: the speed blur works at a quarter size) */
    }
    logm("screen effects (bloom, speed blur): %s", post_ok ? "on" : "off");
    return 0;
}

/* ------------------------------------------------------------------ textures */
static int compressed_mips_ok = 1;
typedef struct { char name[64]; GLuint id; } texent;
static texent texcache[1024]; static int ntexcache; static int texcache_bytes;
float gfx_texture_mb(void) { return texcache_bytes / 1048576.0f; }

static GLuint load_texture(nr_pak *pak, const char *name, int *bytes)
{
    char p[96]; snprintf(p, sizeof p, "textures/%s.nrt", name);
    uint32_t sz; uint8_t *b = nr_pak_read(pak, p, &sz);
    if (!b) { logm("missing texture %s", name); return 0; }
    nr_texdata t;
    if (nr_tex_parse(b, sz, &t)) { logm("bad texture %s", name); free(b); return 0; }
    GLuint id; glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    int mipped = 0;
    if (t.fmt == 8) {
        const uint8_t *m = t.data;
        int levelsn = GSW.no_mips ? 1 : t.nmips;
        for (int i = 0; i < levelsn; i++) {             /* (with mips, down to 1 x 1: the tyre tracks' textures) */
            int w, h; uint32_t sz = nr_tex_mip_size(&t, i, NULL, NULL);
            w = t.w >> i; h = t.h >> i; if (w < 1) w = 1; if (h < 1) h = 1;
            glTexImage2D(GL_TEXTURE_2D, i, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, m);
            m += sz; *bytes += sz;
            if (i > 0) mipped = 1;
        }
    } else if (GSW.rgba) {
        int w, h; nr_tex_mip_size(&t, 0, &w, &h);
        uint8_t *rgba = malloc(w * h * 4);
        nr_dxt_decode(&t, t.data, w, h, rgba);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        free(rgba);
        if (!GSW.no_mips && t.nmips > 1) { glGenerateMipmap(GL_TEXTURE_2D); mipped = 1; }   /* (one level: kept so, as on the Vita) */
        *bytes += w * h * 4 * (mipped ? 4 : 3) / 3;
    } else {
        GLenum f = t.fmt == 1 ? GL_COMPRESSED_RGB_S3TC_DXT1_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        const uint8_t *m = t.data;
        int levelsn = (GSW.no_mips || !compressed_mips_ok) ? 1 : t.nmips;
        for (int i = 0; i < levelsn; i++) {
            int w, h; uint32_t s = nr_tex_mip_size(&t, i, &w, &h);
            while (glGetError() != GL_NO_ERROR) {}
            glCompressedTexImage2D(GL_TEXTURE_2D, i, f, w, h, 0, s, m);
#ifndef NR_PC
            /* vitaGL copies the level with an asynchronous GPU transfer and may move the texture's
             * memory when the next level is added; wait so no copy lands in memory it just freed */
            if (!GSW.no_transfer_wait) sceGxmTransferFinish();
#endif
            GLenum e = glGetError();
            if (e != GL_NO_ERROR) {
                if (i == 0) { logm("texture %s: glCompressedTexImage2D error 0x%x", name, e); break; }
                logm("compressed mip levels not accepted (error 0x%x), using level 0 only", e);
                compressed_mips_ok = 0; break;
            }
            *bytes += s; m += s;
            if (i > 0) mipped = 1;
        }
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipped ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    free(b);
    return id;
}

GLuint gfx_texture(nr_pak *pak, const char *name)
{
    for (int i = 0; i < ntexcache; i++) if (!strcmp(texcache[i].name, name)) return texcache[i].id;
    int b = 0;
    GLuint id = load_texture(pak, name, &b);
    texcache_bytes += b;
    if (!GSW.no_tex_cache && ntexcache < 1024) {
        snprintf(texcache[ntexcache].name, 64, "%s", name);
        texcache[ntexcache++].id = id;
    }
    return id;
}

/* ------------------------------------------------------------------ levels */
static const level_rt *sort_lv;
static int chunk_key(const gchunk *c)
{
    const nr_mat *m = &sort_lv->d.mats[c->mat];
    return (c->kind == NR_KIND_SKY ? 0 : 1 + (m->blend == NR_BLEND_ADD)) * 65536 + m->kind * 4096 + c->mat;
}
static int chunk_order(const void *a, const void *b) { return chunk_key((const gchunk *)a) - chunk_key((const gchunk *)b); }

int gfx_load_nrl(level_rt *lv, nr_pak *pak, const char *pakname, const char *label)
{
    memset(lv, 0, sizeof *lv);
    snprintf(lv->name, sizeof lv->name, "%s", label);
    uint32_t sz; uint8_t *blob = nr_pak_read(pak, pakname, &sz);
    if (!blob) { logm("level %s not in the pak", lv->name); return 0; }
    int r = nr_level_parse(blob, sz, &lv->d);
    if (r) { logm("level %s: parse error %d", lv->name, r); free(blob); return 0; }
    int tb0 = texcache_bytes;
    lv->tex = calloc(lv->d.ntex + 1, sizeof(GLuint));
    for (uint32_t i = 0; i < lv->d.ntex; i++) { lv->tex[i] = gfx_texture(pak, lv->d.texnames[i]); if (gfx_load_tick) gfx_load_tick(); }
    lv->ch = calloc(lv->d.nchunk + 1, sizeof(gchunk));
    int vbytes = 0, tris = 0;
    for (int k = 0; k < 3; k++) { lv->bmin[k] = 1e30f; lv->bmax[k] = -1e30f; }
    for (uint32_t i = 0; i < lv->d.nchunk; i++) {
        nr_chunk *c = &lv->d.chunks[i]; gchunk *g = &lv->ch[lv->nch++];
        g->ni = c->ni; g->mat = c->mat; g->kind = c->kind;
        memcpy(g->bmin, c->bmin, 12); memcpy(g->bmax, c->bmax, 12);
        if (c->kind == NR_KIND_WORLD) for (int k = 0; k < 3; k++) {
            lv->bmin[k] = fminf(lv->bmin[k], c->bmin[k]); lv->bmax[k] = fmaxf(lv->bmax[k], c->bmax[k]);
        }
        glGenBuffers(1, &g->vbo); glGenBuffers(1, &g->ibo);
        glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
        glBufferData(GL_ARRAY_BUFFER, c->nv * NR_VERTEX_SIZE, c->verts, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, c->ni * 2, c->idx, GL_STATIC_DRAW);
        vbytes += c->nv * NR_VERTEX_SIZE + c->ni * 2; tris += c->ni / 3;
        if (gfx_load_tick && (i & 15) == 15) { gfx_load_tick(); }    /* (the loop binds its own buffers again) */
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    sort_lv = lv; qsort(lv->ch, lv->nch, sizeof(gchunk), chunk_order);
    nr_mat *mats = lv->d.mats; float (*path)[3] = lv->d.path;
    lv->d.texnames = NULL; lv->d.chunks = (free(lv->d.chunks), NULL);
    free(blob);
    lv->d.mats = mats; lv->d.path = path;
    lv->pathlen = calloc(lv->d.npath + 1, sizeof(float));
    for (uint32_t i = 1; i < lv->d.npath; i++) {
        v3 a = v3make(path[i - 1][0], path[i - 1][1], path[i - 1][2]), b = v3make(path[i][0], path[i][1], path[i][2]);
        lv->pathlen[i] = lv->pathlen[i - 1] + len(sub(b, a));
    }
#ifdef NR_PC
    float fr = 0;
#else
    float fr = vglMemFree(VGL_MEM_ALL) / 1048576.0f;
#endif
    logm("%s: %u chunks, %d tris, %u textures (%.1f MB new), geometry %.1f MB, free GPU/RAM %.1f MB",
         lv->name, lv->nch, tris, lv->d.ntex, (texcache_bytes - tb0) / 1048576.0, vbytes / 1048576.0, fr);
    return 1;
}

void gfx_set_lights(level_rt *lv, const gfx_light *l, int n)
{
    free(lv->lights); lv->lights = NULL; lv->nlights = 0;
    if (n <= 0) return;
    lv->lights = malloc(sizeof(gfx_light) * n);
    memcpy(lv->lights, l, sizeof(gfx_light) * n);
    lv->nlights = n;
}

void gfx_free_level(level_rt *lv)
{
    for (uint32_t i = 0; i < lv->nch; i++) { glDeleteBuffers(1, &lv->ch[i].vbo); glDeleteBuffers(1, &lv->ch[i].ibo); }
    if (lv->tex && GSW.no_tex_cache) for (uint32_t i = 0; i < lv->d.ntex; i++) if (lv->tex[i]) glDeleteTextures(1, &lv->tex[i]);
    free(lv->tex); free(lv->ch); free(lv->pathlen); free(lv->lights);
    nr_level_data_free(&lv->d);
    memset(lv, 0, sizeof *lv);
}

/* ------------------------------------------------------------------ drawing */
float gfx_beat = 0.75f;
static const level_rt *cur_lv;     /* the level being drawn (its lighting) */
static v3 cur_eye;
static float sky_tint[3], fog_col[3], sky_col[3], fog_u[4], sky_u[4], sun_u[4];

static void lighting_from(const level_rt *lv)
{
    const nr_lighting *L = &lv->d.light;
    sun_u[0] = L->sun[0]; sun_u[1] = L->sun[1]; sun_u[2] = L->sun[2]; sun_u[3] = L->amb;
    float fn = L->fog_near, ff = L->fog_far > L->fog_near + 1 ? L->fog_far : L->fog_near + 1;
    float sf = L->sky_far > ff + 1 ? L->sky_far : ff + 1;
    fog_u[0] = fn; fog_u[1] = 1.0f / (ff - fn); fog_u[2] = GSW.no_fog ? 0.0f : 1.0f; fog_u[3] = 0;
    sky_u[0] = GSW.no_fog ? 1e9f : ff; sky_u[1] = 1.0f / (sf - ff); sky_u[2] = sky_u[3] = 0;
    for (int i = 0; i < 3; i++) {
        fog_col[i] = L->fog[i];
        sky_col[i] = L->sky_avg[i];
        sky_tint[i] = L->sky_tint[i] * L->sky_tint[3] * 2.0f;
    }
}
static void frame_uniforms(prog_t *p)
{
    glUseProgram(p->prog);
    for (int i = 0; i < 4; i++) glUniform4f(p->uVP[i], VP[i][0], VP[i][1], VP[i][2], VP[i][3]);
    for (int i = 0; i < 3; i++) if (p->uV[i] >= 0) glUniform4f(p->uV[i], VIEW[i][0], VIEW[i][1], VIEW[i][2], 0);
    if (p->uEye >= 0) glUniform3f(p->uEye, cur_eye.x, cur_eye.y, cur_eye.z);
    if (p->uSun >= 0) glUniform4fv(p->uSun, 1, sun_u);
    if (p->uFog >= 0) glUniform4fv(p->uFog, 1, fog_u);
    if (p->uSky >= 0) glUniform4fv(p->uSky, 1, sky_u);
    if (p->uFogCol >= 0) glUniform3fv(p->uFogCol, 1, fog_col);
    if (p->uSkyCol >= 0) glUniform3fv(p->uSkyCol, 1, sky_col);
    if (p->uTint >= 0) glUniform4f(p->uTint, sky_tint[0], sky_tint[1], sky_tint[2], 0);
    float t5 = gfx_T * 5.0f; t5 -= floorf(t5);
    if (p->uBeat >= 0) glUniform4f(p->uBeat, gfx_beat, gfx_ab, gfx_T, t5);
    if (p->uFT >= 0) glUniform4f(p->uFT, gfx_beat, gfx_ab, gfx_T, t5);
    if (p->uOFlag >= 0) glUniform4f(p->uOFlag, 0, 0, 1, 0);
    if (p->uVFlag >= 0) glUniform4f(p->uVFlag, 0, 0, 0, 0);
    if (p->uRB >= 0 || p->uRBE >= 0) {
        /* (the PC's noise term, 10 x caustics 0..1, taken at its mean: + 5) */
        int add = p >= PRB && p < PRB + P_COUNT && prog_additive((int)(p - PRB));
        if (p->uRBE >= 0) glUniform4f(p->uRBE, gfx_redblue.e[0], gfx_redblue.e[1], gfx_redblue.e[2], 0);
        if (p->uRB >= 0) glUniform4f(p->uRB, 0, 0, 0, 1.0f / fmaxf(gfx_redblue.r3 + 5, 0.01f));
        if (p->uRB2 >= 0) glUniform4f(p->uRB2, gfx_redblue.r2 + 5, add ? 0 : 0.05f, add ? 0 : 0.025f, 0);   /* the swap's lift: once, not per glowing layer */
        if (p->uRB3 >= 0) glUniform4f(p->uRB3, add ? 0 : 2, add ? 0 : 0.4f, add ? 0 : 0.2f, 0);              /* the ring's hot colour */
    }
}
/* the program, with this view's frame uniforms (set once per view, not per object: uniforms stay with
 * the program) */
static void use_prog(prog_t *p)
{
    if (p->stamp != vp_id) { frame_uniforms(p); p->stamp = vp_id; }
    else glUseProgram(p->prog);
}
static void vertex_layout(const void *base)
{
    const uint8_t *b = base;
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, NR_VERTEX_SIZE, b);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, NR_VERTEX_SIZE, b + 12);
    glVertexAttribPointer(2, 4, GL_BYTE, GL_TRUE, NR_VERTEX_SIZE, b + 20);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, NR_VERTEX_SIZE, b + 24);
    glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, NR_VERTEX_SIZE, b + 28);
}
static int prog_for(const nr_mat *m, int object)
{
    if (m->blend == NR_BLEND_ADD || m->kind == NR_SHADE_ADD) {
        if (m->fx == NR_FX_VEINS) return object ? P_OBJVEINS : P_WVEINS;
        if (m->variant == NR_ADD_TRAFFIC || m->variant == NR_ADD_FLAME) return object ? P_OBJSCROLL : P_WSCROLL;
        if (m->variant == NR_ADD_GROUND && !object) return P_WGLASER;
        return object ? P_OBJADD : P_WADD;
    }
    switch (m->fx) {
    case NR_FX_PULSE: return object ? P_OBJPULSE : P_WPULSE;
    case NR_FX_ANIM: return object ? P_OBJANIM : P_WANIM;
    case NR_FX_TRONICS: if (!object) return P_WTRON; break;
    case NR_FX_CITY: if (!object) return P_WCITY; break;
    case NR_FX_INTRO: if (object) return P_OBJINTRO; break;
    }
    if (object) return m->kind == NR_SHADE_TV || m->kind == NR_SHADE_UNLIT ? P_OBJTV : P_OBJ;
    switch (m->kind) {
    case NR_SHADE_REF: return P_WREF;
    case NR_SHADE_TV: case NR_SHADE_UNLIT: return P_WTV;
    case NR_SHADE_SKY: return P_SKY;
    default: return P_WORLD;
    }
}
static float fmodh(float x) { return x < 0 ? -(-x - floorf(-x)) : x - floorf(x); }    /* HLSL fmod(x, 1) */
static float cur_phase;               /* the object being drawn: its own phase (traffic, screens) */
/* glow: the object's glow factor; vscale: what the vertex colour's alpha is worth (the model's
 * glow scale / 8; 1 when the object's logic sets the colour) */
static void bind_material(const level_rt *lv, const nr_mat *m, prog_t *p, float glow, float vscale)
{
    GLuint d = m->diffuse >= 0 && lv->tex[m->diffuse] ? lv->tex[m->diffuse] : 0;
    GLuint e = m->emit >= 0 && lv->tex[m->emit] ? lv->tex[m->emit] : 0;
    GLuint r = m->reflect >= 0 && lv->tex[m->reflect] ? lv->tex[m->reflect] : 0;
    GLuint an = m->anim >= 0 && (uint32_t)m->anim < lv->d.ntex && lv->tex[m->anim] ? lv->tex[m->anim] : 0;
    if (m->fx == NR_FX_ANIM) r = an;                        /* Anim1 on unit 2 */
    if (m->fx == NR_FX_TRONICS || m->fx == NR_FX_CITY) e = an;   /* Tron1 on unit 1 */
    int add = m->blend == NR_BLEND_ADD || m->kind == NR_SHADE_ADD;
    if (add && !d) d = e;                          /* additive: its one texture is the glow */
    if (m->kind == NR_SHADE_SKY && !d) d = e;
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, d ? d : ((m->flags & NR_MAT_NO_DIFFUSE) && !add ? tex_black : tex_white));
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, e ? e : (m->flags & NR_MAT_EMIT_COLOUR) ? tex_white : tex_black);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, r ? r : tex_black);
    if (m->fx == NR_FX_INTRO) {                    /* TriangleEdges on unit 3 */
        GLuint te = m->detail >= 0 && (uint32_t)m->detail < lv->d.ntex && lv->tex[m->detail] ? lv->tex[m->detail] : 0;
        glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, te ? te : tex_black);
    }
    float tv = m->kind == NR_SHADE_UNLIT ? 1.0f : m->fx == NR_FX_TV && m->p[0] > 0 ? m->p[0] : 0.35f;
    float w = add ? (m->variant == NR_ADD_GLASS ? 1.0f : 0.0f) : 0.0f;
    if (p->uMat >= 0) glUniform4f(p->uMat, glow * vscale, r && !(gfx_dbg & 2) ? 1.0f : 0.0f, tv, w);
    if (p->uMP >= 0) {
        float mp[4] = { 0, 0, 0, 0 };
        switch (m->fx) {
        case NR_FX_PULSE: mp[0] = fmodh(m->p[0] * gfx_T + m->p[1]); mp[1] = m->p[2]; break;     /* Float1..3 */
        case NR_FX_VEINS: mp[0] = fmodh(1.66f - 4.0f * gfx_ab); mp[1] = 1.1f - 4.0f * gfx_ab; break;
        case NR_FX_TRONICS: mp[0] = 60.0f; break;      /* the PC's 100 aliases at the Vita's resolution */
        case NR_FX_CITY: memcpy(mp, m->p, 16); break;  /* Color1 */
        case NR_FX_TV: mp[0] = m->p[1]; mp[1] = m->p[2]; break;
        }
        if (m->flags & NR_MAT_REFSPEC) mp[3] = m->p[3] > 0 ? m->p[3] : 1.0f;
        glUniform4fv(p->uMP, 1, mp);
    }
    if (p->uMC2 >= 0) glUniform4f(p->uMC2, m->c2[0], m->c2[1], m->c2[2], m->c2[3] * glow);
    if (p->uVFlag >= 0) glUniform4f(p->uVFlag, (m->flags & NR_MAT_SPHERE_N) ? 1.0f : 0.0f, 0, 0, 0);
    if (p->uSc >= 0) {
        /* Time (0..1 in 20 s) plus the object's own phase; the offsets are whole texture
         * repeats when Time wraps, so the scrolling is seamless */
        float t = gfx_T + cur_phase;
        if (m->variant == NR_ADD_TRAFFIC) {          /* traffic.fx: fract(0.32 id) */
            glUniform4f(p->uSc, 2.0f, 1.0f, 0.0f, t);
            glUniform4f(p->uOff, -5, 0, -5, 0);
            glUniform4f(p->uC1, 0, 0, 0.4f, 0); glUniform4f(p->uC2, 0, 1, 0, 0);
        } else {                                      /* jet_flame.fx */
            glUniform4f(p->uSc, 1.0f, 1.0f, 0.5f, t);
            glUniform4f(p->uOff, -10, 10, -17, 7);
            glUniform4f(p->uC1, 0, 1, 0, 0); glUniform4f(p->uC2, 0, 0, -5, 0);
        }
    }
}

typedef struct { float d; int i; int prog; } drawitem;
static drawitem items[8192];
static int by_prog_dist(const void *a, const void *b)
{
    const drawitem *x = a, *y = b;
    if (x->prog != y->prog) return x->prog - y->prog;
    return x->d < y->d ? -1 : x->d > y->d;
}

static void draw_chunk(const gchunk *c)
{
    glBindBuffer(GL_ARRAY_BUFFER, c->vbo);
    vertex_layout(NULL);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, c->ibo);
    glDrawElements(GL_TRIANGLES, c->ni, GL_UNSIGNED_SHORT, (void *)0);
    gfx_draws++; gfx_tris += c->ni / 3;
}

static void draw_items(const level_rt *lv, int n, float glow)
{
    int lastprog = -1, lastmat = -1;
    cur_phase = 0;
    for (int k = 0; k < n; k++) {
        const gchunk *c = &lv->ch[items[k].i];
        prog_t *p = prog_get(items[k].prog);
        if (items[k].prog != lastprog) { lastprog = items[k].prog; frame_uniforms(p); lastmat = -1; }
        if (c->mat != lastmat) { lastmat = c->mat; bind_material(lv, &lv->d.mats[c->mat], p, glow, lv->d.glow_scale > 0 ? lv->d.glow_scale / 8.0f : 1.0f); }
#ifdef NR_PC
        if (getenv("SKIPMAT") && atoi(getenv("SKIPMAT")) == c->mat) continue;
#endif
        draw_chunk(c);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

static int collect(const level_rt *lv, v3 eye, int pass)  /* 0 sky, 1 opaque, 2 additive */
{
    int n = 0;
    float far = cur_lv ? cur_lv->d.light.sky_far + 200 : 1e9f;
    for (uint32_t i = 0; i < lv->nch && n < 8192; i++) {
        const gchunk *c = &lv->ch[i];
        const nr_mat *m = &lv->d.mats[c->mat];
        int add = m->blend == NR_BLEND_ADD || m->kind == NR_SHADE_ADD;
        if (pass == 0) {
            if (c->kind != NR_KIND_SKY) continue;
            items[n].d = 0; items[n].prog = P_SKY; items[n++].i = i; continue;
        }
        if (c->kind == NR_KIND_SKY) continue;
        if ((pass == 2) != add) continue;
        float d = aabb_dist(c->bmin, c->bmax, eye);
        if (c->kind == NR_KIND_WORLD && !GSW.no_fog && d > far) continue;
        if (!aabb_visible(c->bmin, c->bmax)) continue;
        items[n].d = c->kind == NR_KIND_BACKDROP ? 1e9f : d; items[n].prog = prog_for(m, 0); items[n++].i = i;
    }
    return n;
}

/* the level's lights in a 2D grid of 64-unit cells (each cell lists the lights whose reach overlaps
 * it), so an object looks at the few near it, not at all of them (Level 5: hundreds of objects x
 * hundreds of lights, twice a frame, was most of the frame's CPU time) */
#define LG_CELL 64.0f
static float LG_cw = LG_CELL, LG_ch = LG_CELL;
static struct { const gfx_light *key; int n, nx, ny; float x0, y0; int *start, *idx; } LG;
static void light_grid(const level_rt *lv)
{
    if (LG.key == lv->lights && LG.n == lv->nlights) return;
    free(LG.start); free(LG.idx); memset(&LG, 0, sizeof LG);
    LG.key = lv->lights; LG.n = lv->nlights;
    if (!lv->nlights) return;
    float mn[2] = { 1e30f, 1e30f }, mx[2] = { -1e30f, -1e30f };
    float (*bb)[4] = malloc(sizeof(float) * 4 * lv->nlights);
    for (int i = 0; i < lv->nlights; i++) {
        const gfx_light *L = &lv->lights[i];
        float r = L->far_, ex = 0, ey = 0;
        if (L->type == 3) { ex = fabsf(L->axis[0]) * L->half; ey = fabsf(L->axis[1]) * L->half; }
        bb[i][0] = L->pos[0] - ex - r; bb[i][1] = L->pos[1] - ey - r; bb[i][2] = L->pos[0] + ex + r; bb[i][3] = L->pos[1] + ey + r;
        mn[0] = fminf(mn[0], bb[i][0]); mn[1] = fminf(mn[1], bb[i][1]); mx[0] = fmaxf(mx[0], bb[i][2]); mx[1] = fmaxf(mx[1], bb[i][3]);
    }
    LG.x0 = mn[0]; LG.y0 = mn[1];
    LG.nx = (int)((mx[0] - mn[0]) / LG_CELL) + 1; LG.ny = (int)((mx[1] - mn[1]) / LG_CELL) + 1;
    if (LG.nx > 256) LG.nx = 256; if (LG.ny > 256) LG.ny = 256;
    float cw = (mx[0] - mn[0]) / LG.nx + 1e-3f, ch = (mx[1] - mn[1]) / LG.ny + 1e-3f;
    int nc = LG.nx * LG.ny;
    LG.start = calloc(nc + 1, sizeof(int));
    for (int pass = 0; pass < 2; pass++) {               /* count, then fill */
        int *cnt = calloc(nc, sizeof(int));
        for (int i = 0; i < lv->nlights; i++) {
            int a0 = (int)((bb[i][0] - LG.x0) / cw), b0 = (int)((bb[i][1] - LG.y0) / ch);
            int a1 = (int)((bb[i][2] - LG.x0) / cw), b1 = (int)((bb[i][3] - LG.y0) / ch);
            if (a0 < 0) a0 = 0; if (b0 < 0) b0 = 0; if (a1 >= LG.nx) a1 = LG.nx - 1; if (b1 >= LG.ny) b1 = LG.ny - 1;
            for (int b = b0; b <= b1; b++) for (int a = a0; a <= a1; a++) {
                int c = b * LG.nx + a;
                if (pass) LG.idx[LG.start[c] + cnt[c]] = i;
                cnt[c]++;
            }
        }
        if (!pass) { for (int c = 0; c < nc; c++) LG.start[c + 1] = LG.start[c] + cnt[c]; LG.idx = malloc(sizeof(int) * (LG.start[nc] + 1)); }
        free(cnt);
    }
    free(bb);
    LG_cw = cw; LG_ch = ch;                              /* (the cells as built) */
}

/* the four level lights that light a point the most (tubes: their nearest point) */
static void object_lights(prog_t *p, v3 at)
{
    if (p->uLA[0] < 0) return;                           /* (a program without lights) */
    float best[4] = { 0, 0, 0, 0 }; int bi[4] = { -1, -1, -1, -1 }; v3 bp[4];
    const level_rt *lv = cur_lv;
    int from = 0, to = 0; const int *list = NULL;
    if (lv && lv->nlights) {
        light_grid(lv);
        int a = (int)((at.x - LG.x0) / LG_cw), b = (int)((at.y - LG.y0) / LG_ch);
        if (a >= 0 && b >= 0 && a < LG.nx && b < LG.ny) { int c = b * LG.nx + a; from = LG.start[c]; to = LG.start[c + 1]; list = LG.idx; }
    }
    for (int j = from; list && j < to; j++) {
        int i = list[j];
        const gfx_light *L = &lv->lights[i];
        v3 q = v3make(L->pos[0], L->pos[1], L->pos[2]);
        if (L->type == 3) {
            v3 ax = v3make(L->axis[0], L->axis[1], L->axis[2]);
            float t = dot(sub(at, q), ax); t = fmaxf(-L->half, fminf(L->half, t));
            q = add(q, mul(ax, t));
        }
        float d = len(sub(q, at));
        if (d >= L->far_) continue;
        float att = fminf(1.5f, L->near_ * (L->far_ - d) / (fmaxf(d, 0.01f) * (L->far_ - L->near_)));
        float s = att * (L->col[0] + L->col[1] + L->col[2]);
        for (int k = 0; k < 4; k++) if (s > best[k]) {
            for (int m_ = 3; m_ > k; m_--) { best[m_] = best[m_ - 1]; bi[m_] = bi[m_ - 1]; bp[m_] = bp[m_ - 1]; }
            best[k] = s; bi[k] = i; bp[k] = q; break;
        }
    }
    float amb[4] = { 0, 0, 0, 0 };
    for (int k = 0; k < 4; k++) {
        if (bi[k] < 0) {
            glUniform4f(p->uLA[k], 0, 0, -1e5f, 1); glUniform4f(p->uLB[k], 0, 0, 0, 2);
            continue;
        }
        const gfx_light *L = &lv->lights[bi[k]];
        glUniform4f(p->uLA[k], bp[k].x, bp[k].y, bp[k].z, L->near_);
        glUniform4f(p->uLB[k], L->col[0], L->col[1], L->col[2], L->far_);
        amb[k] = L->amb;
    }
    glUniform4fv(p->uLamb, 1, amb);
}

/* an object in the view: its model's box as a sphere, through the object's matrix (scale included) */
static int obj_in_view(const gfx_obj *o)
{
    const level_rt *m = o->model;
    if (!m || !m->nch) return 0;
    if (o->no_cull) return 1;
    if (!(m->bmax[0] >= m->bmin[0] && m->bmax[1] >= m->bmin[1] && m->bmax[2] >= m->bmin[2])) return 1;   /* (no box: drawn) */
    float lc[3], h[3], c[3], r2 = 0;
    for (int k = 0; k < 3; k++) { lc[k] = 0.5f * (m->bmin[k] + m->bmax[k]); h[k] = 0.5f * (m->bmax[k] - m->bmin[k]); }
    for (int r = 0; r < 3; r++) c[r] = o->m[r][0] * lc[0] + o->m[r][1] * lc[1] + o->m[r][2] * lc[2] + o->m[r][3];
    for (int k = 0; k < 3; k++) {
        float l2 = o->m[0][k] * o->m[0][k] + o->m[1][k] * o->m[1][k] + o->m[2][k] * o->m[2][k];
        r2 += l2 * h[k] * h[k];
    }
    return sphere_visible(c, sqrtf(r2) + 0.5f);
}
int gfx_objs_drawn, gfx_objs_culled;
float gfx_prof_sky;
float gfx_prof[8];
static uint64_t gnow(void) { return sceKernelGetProcessTimeWide(); }

static void draw_obj(const gfx_obj *o, int pass)    /* 0 solid, 1 additive */
{
    const level_rt *m = o->model;
    if (!m || !m->nch) return;
    int lastprog = -1;
    for (uint32_t i = 0; i < m->nch; i++) {
        const gchunk *c = &m->ch[i];
        const nr_mat *mt = &m->d.mats[c->mat];
        int pi = prog_for(mt, 1);
        if (prog_additive(pi) != (pass == 1)) continue;
        /* no reflection map and no tint: the plain lit shader (no sphere map work, one read less) */
        if (pi == P_OBJ && !(mt->reflect >= 0 && m->tex[mt->reflect]) && o->tint[3] <= 0 && !(mt->flags & (NR_MAT_SPHERE_N | NR_MAT_REFSPEC)))
            pi = P_OBJLIT;
        prog_t *p = prog_get(pi);
        if (pi != lastprog) {
            lastprog = pi;
            use_prog(p);
            if (p->uOFlag >= 0) glUniform4f(p->uOFlag, o->use_col ? 1.0f : 0.0f, 0, 1.0f + o->phase, 0);
            if (o->use_col && p->uOCol >= 0) glUniform4fv(p->uOCol, 1, o->col);
            for (int r = 0; r < 3; r++) glUniform4f(p->uM[r], o->m[r][0], o->m[r][1], o->m[r][2], o->m[r][3]);
            if (p->uIntro >= 0) glUniform4f(p->uIntro, 1.25f - 2.7f * o->intro_p, 0, 0, 0);
            if (p->uPortal >= 0) glUniform4fv(p->uPortal, 1, o->portal);
            object_lights(p, v3make(o->m[0][3], o->m[1][3], o->m[2][3]));
        }
        cur_phase = o->phase;
        bind_material(m, mt, p, o->glow > 0 ? o->glow : 1.0f, o->use_col ? 1.0f : (m->d.glow_scale > 0 ? m->d.glow_scale / 8.0f : 1.0f));
        if (p->uTint >= 0) glUniform4fv(p->uTint, 1, o->tint);
        draw_chunk(c);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

/* camera-facing quads, additive */
static void draw_sprites(const gfx_sprite *s, int n, v3 fwd, v3 upv)
{
    if (n <= 0) return;
    static uint8_t buf[64 * 6 * NR_VERTEX_SIZE];
    v3 r = norm(cross(fwd, upv)), u = cross(r, fwd);
    prog_t *p = &P[P_WADD];
    frame_uniforms(p);
    glUniform4f(p->uMat, 1, 0, 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    int nv = 0;
    for (int i = 0; i < n && i < 64; i++) {
        const gfx_sprite *q0 = &s[i];
        float c = cosf(q0->angle), sn = sinf(q0->angle), h = q0->size * 0.5f;
        v3 ax = add(mul(r, c * h), mul(u, sn * h)), ay = add(mul(r, -sn * h), mul(u, c * h));
        v3 q[4] = { sub(sub(q0->pos, ax), ay), sub(add(q0->pos, ax), ay), add(add(q0->pos, ax), ay), add(sub(q0->pos, ax), ay) };
        static const float uv[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
        static const int ord[6] = { 0, 1, 2, 0, 2, 3 };
        float mx = fmaxf(1.0f, fmaxf(q0->r, fmaxf(q0->g, q0->b)));
        for (int k = 0; k < 6; k++) {
            int j = ord[k];
            uint8_t *v = buf + (nv++) * NR_VERTEX_SIZE;
            float f[5] = { q[j].x, q[j].y, q[j].z, uv[j][0], uv[j][1] };
            memcpy(v, f, 20);
            v[20] = v[21] = 0; v[22] = 127; v[23] = 0;
            v[24] = (uint8_t)fminf(255, q0->r / mx * 255); v[25] = (uint8_t)fminf(255, q0->g / mx * 255); v[26] = (uint8_t)fminf(255, q0->b / mx * 255);
            v[27] = (uint8_t)fminf(255, mx / 8 * 255);
            v[28] = v[29] = v[30] = v[31] = 0;
        }
    }
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex_glow);
    vertex_layout(buf);
    glDrawArrays(GL_TRIANGLES, 0, nv);
}

/* quads in the level vertex format (4 vertices each), additive or subtractive, with the texture tex */
void (*gfx_hook_additive)(v3 eye, v3 fwd, v3 up);
void gfx_quads(const void *verts, int nquads, GLuint tex, int subtract)
{
    static uint16_t idx[16384 / 4 * 6]; static int nidx;
    if (nquads <= 0) return;
    if (nquads > 16384 / 4) nquads = 16384 / 4;
    if (!nidx) for (int q = 0; q < 16384 / 4; q++) {
        static const int o[6] = { 0, 1, 2, 0, 2, 3 };
        for (int k = 0; k < 6; k++) idx[nidx++] = (uint16_t)(q * 4 + o[k]);
    }
    prog_t *p = &P[P_WADD];
    frame_uniforms(p);
    glUniform4f(p->uMat, 1, 0, 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex);
    if (subtract) glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
    vertex_layout(verts);
    glDrawElements(GL_TRIANGLES, nquads * 6, GL_UNSIGNED_SHORT, idx);
    if (subtract) glBlendEquation(GL_FUNC_ADD);
    gfx_draws++; gfx_tris += nquads * 2;
}

/* the tyre tracks: pass 0 (from gfx_hook_decal, after the solid objects) the slow tracks' dark tread, the scene
 * multiplied by 1 - a; pass 1 (from gfx_hook_additive, in the glow pass) the fast ones' glow, added. No culling,
 * depth test, no depth writes (tire_track.fx's states) */
void (*gfx_hook_decal)(v3 eye, v3 fwd, v3 up);
void gfx_tracks(const void *verts, int nquads, GLuint tread, GLuint rainbow, int pass)
{
    static uint16_t idx[4096 / 4 * 6]; static int nidx;
    if (nquads <= 0 || !tread) return;
    if (nquads > 4096 / 4) nquads = 4096 / 4;
    if (!nidx) for (int q = 0; q < 4096 / 4; q++) {
        static const int o[6] = { 0, 1, 2, 0, 2, 3 };
        for (int k = 0; k < 6; k++) idx[nidx++] = (uint16_t)(q * 4 + o[k]);
    }
    prog_t *p = &P[pass ? P_TRACK : P_TRACKDARK];
    use_prog(p);
    glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, rainbow ? rainbow : tex_white);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tread);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND); glDepthMask(GL_FALSE);
    if (pass) glBlendFunc(GL_ONE, GL_ONE); else glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
    vertex_layout(verts);
    glDrawElements(GL_TRIANGLES, nquads * 6, GL_UNSIGNED_SHORT, idx);
    glBlendFunc(GL_ONE, GL_ONE);
    gfx_draws++; gfx_tris += nquads * 2;
}

/* ------------------------------------------------------------------ screen passes */
static void quad_pass_uv(prog_t *p, target_t *dst, float us, float vs)
{
    const float v[16] = { -1, -1, 0, 0,  1, -1, us, 0,  -1, 1, 0, vs,  1, 1, us, vs };
    glBindFramebuffer(GL_FRAMEBUFFER, dst ? dst->fbo : 0);
    glViewport(0, 0, dst ? dst->w : SW, dst ? dst->h : SH);
    glUseProgram(p->prog);
    glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, v);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
static void quad_pass(prog_t *p, target_t *dst) { quad_pass_uv(p, dst, 1, 1); }
static void grid_pass(prog_t *p, target_t *dst)
{   /* full screen as a 16x9 grid of quads, for per-vertex work that isn't linear */
    enum { GX = 16, GY = 9 };
    static float v[GX * GY * 6 * 4]; static int nv;
    if (!nv) {
        for (int y = 0; y < GY; y++) for (int x = 0; x < GX; x++) {
            static const int c[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 0, 1 }, { 1, 0 }, { 1, 1 } };
            for (int k = 0; k < 6; k++) {
                float u = (float)(x + c[k][0]) / GX, w = (float)(y + c[k][1]) / GY;
                float *o = v + nv * 4; o[0] = u * 2 - 1; o[1] = w * 2 - 1; o[2] = u; o[3] = w; nv++;
            }
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, dst ? dst->fbo : 0);
    glViewport(0, 0, dst ? dst->w : SW, dst ? dst->h : SH);
    glUseProgram(p->prog);
    glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, v);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
    glDrawArrays(GL_TRIANGLES, 0, nv);
}
static void tex_unit(int u, GLuint t)
{
    glActiveTexture(GL_TEXTURE0 + u); glBindTexture(GL_TEXTURE_2D, t);
}

/* the 3D scene may be drawn into the top left part of its target (gfx_scene_scale): the passes
 * that read it take these coordinates */
static float scene_us = 1, scene_vs = 1;
float gfx_scene_scale = 1;
static void post_passes(const gfx_post *post)
{
    glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
    for (int i = 2; i < 5; i++) glDisableVertexAttribArray(i);
    int bloom = !GSW.no_bloom;
    if (bloom) {
        /* bright parts at a quarter size, blurred; and a sixteenth-size wide glow. Three small
         * passes: every render target is a separate GPU scene on the Vita, so few of them */
        tex_unit(0, T_SCENE.tex);
        glUseProgram(P[P_DOWN].prog);
        glUniform4f(P[P_DOWN].uStep, 1.0f / SW, 1.0f / SH, gfx_bloom_thr, 0);
        quad_pass_uv(&P[P_DOWN], &T_B[0], scene_us, scene_vs);
        if (!GSW.no_wide) {
            tex_unit(0, T_B[0].tex);
            glUseProgram(P[P_WIDE].prog);
            glUniform4f(P[P_WIDE].uStep, 1.0f / T_B[0].w, 1.0f / T_B[0].h, 0, 0);
            quad_pass(&P[P_WIDE], &T_B[2]);
        }
        tex_unit(0, T_B[0].tex); tex_unit(2, GSW.no_wide ? tex_black : T_B[2].tex);
        glUseProgram(P[P_BLUR].prog);
        glUniform4f(P[P_BLUR].uStep, 1.0f / T_B[0].w, 1.0f / T_B[0].h, gfx_bloom_tune[0], gfx_bloom_tune[1]);
        quad_pass(&P[P_BLUR], &T_B[1]);
        tex_unit(2, 0); glActiveTexture(GL_TEXTURE0);
    }
    int speed = post && post->blur > 0.01f;
    if (speed) {
        /* the speed blur: the scene smeared out from the centre (five taps along the line, straight from the
         * scene) at a quarter size, into the first glow target (free again after the blur pass) - no half-size
         * copy of the scene first: one pass and three quarters of the reads less */
        tex_unit(0, T_SCENE.tex);
        glUseProgram(P[P_RADIAL].prog);
        glUniform4f(P[P_RADIAL].uStep, post->blur, 0, 0, 0);
        if (P[P_RADIAL].uScale >= 0) glUniform4f(P[P_RADIAL].uScale, scene_us, scene_vs, 0, 0);
        grid_pass(&P[P_RADIAL], &T_B[3]);
        tex_unit(3, T_B[3].tex);
    }
    prog_t *p = &P[speed ? P_COMP_SPEED : P_COMP];
    glUseProgram(p->prog);
    tex_unit(0, T_SCENE.tex);
    tex_unit(1, bloom ? T_B[1].tex : tex_black);
    float bs = post && post->bloom > 0 ? post->bloom : 1.0f;
    /* blurred brightness is stored / 4, and the glow sum at half */
    glUniform4f(p->uBloom, gfx_bloom_tune[0], gfx_bloom_tune[1], 0, 8.0f * gfx_bloom_tune[3] * bs);
    if (post) glUniform4fv(p->uEdge, 1, post->edge); else glUniform4f(p->uEdge, 0, 0, 0, 0);
    glUniform4f(p->uFx, post ? post->blur : 0, post ? post->flash : 0, post ? post->gain : 0, 0);
    if (p->uStep >= 0) glUniform4f(p->uStep, post ? post->blur : 0, 0, 0, 0);
    if (p->uScale >= 0) glUniform4f(p->uScale, scene_us, scene_vs, 0, 0);
    grid_pass(p, NULL);                    /* (a grid: the edge tint is worked out per vertex) */
    if (speed) tex_unit(3, 0);
    tex_unit(1, 0); glActiveTexture(GL_TEXTURE0);
    for (int i = 2; i < 5; i++) glEnableVertexAttribArray(i);
    glDepthMask(GL_TRUE);
}

void gfx_draw_frame(const level_rt *lv, v3 eye, v3 fwd, v3 upv, float fov_deg,
                    const gfx_obj *objs, int nobj, const gfx_sprite *spr, int nspr, const gfx_post *post)
{
    gfx_draws = gfx_tris = 0;
    cur_phase = 0;
    uint64_t tp0 = gnow(), tp1 = tp0, tp2 = tp0, tp3 = tp0, tp4 = tp0, tp5 = tp0, tp6 = tp0;
    static unsigned char ovis[1024];
    int sec_d[8] = { 0 }, sec_t[8] = { 0 }, sd0 = 0, st0 = 0;
#define SEC(k) do { sec_d[k] = gfx_draws - sd0; sec_t[k] = gfx_tris - st0; sd0 = gfx_draws; st0 = gfx_tris; } while (0)
    int pp = post_ok && !GSW.no_post;
    for (int i = 0; i < 5; i++) glEnableVertexAttribArray(i);
    if (pp) {
        float sc = gfx_scene_scale < 0.5f ? 0.5f : gfx_scene_scale > 1 ? 1 : gfx_scene_scale;
        int w = ((int)(SW * sc) + 1) & ~1, h = ((int)(SH * sc) + 1) & ~1;
        if (w > SW) w = SW; if (h > SH) h = SH;
        scene_us = (float)w / SW; scene_vs = (float)h / SH;
        glBindFramebuffer(GL_FRAMEBUFFER, T_SCENE.fbo); glViewport(0, 0, w, h);
    }
    cur_lv = lv; cur_eye = eye;
    if (lv) lighting_from(lv);
    glClearColor(lv ? fog_col[0] : 0, lv ? fog_col[1] : 0, lv ? fog_col[2] : 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    uint64_t tpc = gnow(), tps = tpc;
    if (lv && lv->nch) {
        /* the PC's projection (0x605590): the FOV is a mean of the two axes, fovY = 2 atan(tan(FOV/2) /
         * sqrt(aspect)); near 0.1, far SkyFar + 0.01 */
        const float fov = 2.0f * atanf(tanf(fov_deg * 3.14159265f / 360.0f) / sqrtf((float)SW / SH));
        const float zfar = (lv->d.light.sky_far > 50 ? lv->d.light.sky_far : 1600.0f) + 0.01f;
        /* the sky: a unit cube around the camera */
        build_vp(eye, fwd, upv, 1, fov, 0.05f, 100.0f);
        glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
        draw_items(lv, collect(lv, eye, 0), 1);
        tps = gnow();
        /* the level, near to far */
        build_vp(eye, fwd, upv, 0, fov, 0.1f, zfar);
        build_planes();
        /* the objects outside the view are left out (the caller only drops those behind or far off) */
        gfx_objs_drawn = gfx_objs_culled = 0;
        if (nobj > (int)sizeof ovis) nobj = (int)sizeof ovis;
        for (int i = 0; i < nobj; i++) { ovis[i] = (unsigned char)obj_in_view(&objs[i]); if (ovis[i]) gfx_objs_drawn++; else gfx_objs_culled++; }
#ifdef NR_PC
        if (getenv("NOOBJCULL")) for (int i = 0; i < nobj; i++) ovis[i] = objs[i].model && objs[i].model->nch;   /* (the PC test machine: to compare) */
#endif
        tp1 = gnow();
        glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE);
        int n = collect(lv, eye, 1);
        qsort(items, n, sizeof(drawitem), by_prog_dist);
        if (!GSW.no_cull) glEnable(GL_CULL_FACE);
        if (!(gfx_dbg & 4)) draw_items(lv, n, 1);          /* (gfx_dbg 4: the bench leaves the level out) */
        tp2 = gnow(); SEC(1);
        if (!(gfx_dbg & 16)) for (int i = 0; i < nobj; i++) if (ovis[i]) draw_obj(&objs[i], 0);   /* (gfx_dbg 16 / 8: the bench) */
        glDisable(GL_CULL_FACE);
        if (gfx_hook_decal) {
            gfx_hook_decal(eye, fwd, upv);
            glDisable(GL_BLEND); glDepthMask(GL_TRUE);
        }
        tp3 = gnow(); SEC(2);
        /* glowing things on top */
        glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE); glDepthMask(GL_FALSE);
        n = collect(lv, eye, 2);
        if (!(gfx_dbg & 1)) draw_items(lv, n, 1);
        tp4 = gnow(); SEC(3);
        if (!(gfx_dbg & 8)) for (int i = 0; i < nobj; i++) if (ovis[i]) draw_obj(&objs[i], 1);
        tp5 = gnow(); SEC(4);
        draw_sprites(spr, nspr, fwd, upv);
        if (gfx_hook_additive) gfx_hook_additive(eye, fwd, upv);
        glDisable(GL_BLEND); glDepthMask(GL_TRUE);
    }
    tp6 = gnow(); SEC(5);
    if (pp) post_passes(post);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, SW, SH);
    glDisable(GL_DEPTH_TEST);
    for (int i = 2; i < 5; i++) glDisableVertexAttribArray(i);
    /* where the frame's building went (ms): sky and culling, level, objects, glowing level, glowing objects,
     * sprites and particles, screen passes */
    uint64_t te = gnow();
    gfx_prof[0] = (tp1 - tp0) / 1000.0f; gfx_prof[1] = (tp2 - tp1) / 1000.0f; gfx_prof[2] = (tp3 - tp2) / 1000.0f;
    gfx_prof[3] = (tp4 - tp3) / 1000.0f; gfx_prof[4] = (tp5 - tp4) / 1000.0f; gfx_prof[5] = (tp6 - tp5) / 1000.0f;
    gfx_prof[6] = (te - tp6) / 1000.0f;
    gfx_prof[7] = (tpc - tp0) / 1000.0f;            /* (of [0]: the target bound and cleared; the sky is tps - tpc) */
    gfx_prof_sky = (tps - tpc) / 1000.0f;
    (void)sec_d; (void)sec_t;
#ifdef NR_PC
    if (getenv("GFXSTAT")) {   /* (the PC test machine: draws and triangles per part, once a second) */
        static int fr; if (++fr % 60 == 0) {
            fprintf(stderr, "GFXSTAT objs %d+%d culled | sky+lvl draws %d tris %d | obj %d/%d | glow lvl %d/%d | glow obj %d/%d | sprites %d/%d\n",
                gfx_objs_drawn, gfx_objs_culled, sec_d[1], sec_t[1], sec_d[2], sec_t[2], sec_d[3], sec_t[3], sec_d[4], sec_t[4], sec_d[5], sec_t[5]);
        }
    }
#endif
}

/* every 3D program drawn once with each blend state it is used with (an empty triangle into the scene
 * target): vitaGL makes a program's GPU code for a blend state the first time it is drawn with it, which
 * mid-race is a hitch */
void gfx_prewarm(void)
{
    static uint8_t v[3 * NR_VERTEX_SIZE];
    int pp = post_ok && !GSW.no_post;
    if (pp) glBindFramebuffer(GL_FRAMEBUFFER, T_SCENE.fbo);
    glViewport(0, 0, 8, 8);
    for (int i = 0; i < 5; i++) glEnableVertexAttribArray(i);
    glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glEnable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
    int n = 0;
    for (int p = 0; p < P_COUNT; p++) {
        if (p >= P_DOWN && p <= P_COMP_SPEED) continue;          /* (the screen passes: used every frame from the start) */
        for (int vr = 0; vr < 2; vr++) {
        GLuint pg = vr ? PRB[p].prog : P[p].prog;
        if (!pg) continue;
        glUseProgram(pg);
        for (int b = 0; b < 3; b++) {
            if (b == 0) glDisable(GL_BLEND);
            else { glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE); glBlendEquation(b == 2 ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD); }
            if (p == P_TRACKDARK) { glEnable(GL_BLEND); glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA); glBlendEquation(GL_FUNC_ADD); }
            vertex_layout(v);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            n++;
        }
        glBlendFunc(GL_ONE, GL_ONE);
        }
    }
    glBlendEquation(GL_FUNC_ADD); glDisable(GL_BLEND); glDepthMask(GL_TRUE);
    for (int i = 2; i < 5; i++) glDisableVertexAttribArray(i);
    if (pp) {   /* the speed blur's passes too (only used once the car is fast: their first use was a 100 ms hitch) */
        gfx_post fake; memset(&fake, 0, sizeof fake); fake.blur = 0.5f;
        if (scene_us <= 0) scene_us = scene_vs = 1;
        glViewport(0, 0, SW, SH);
        post_passes(&fake);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, SW, SH);
    (void)n;
}
