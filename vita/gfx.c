/* 3D drawing: the converted levels and models with vitaGL (GLSL), lit and post-processed the
 * way the PC game's renderer does it (see gfx.h). */
#include "gfx.h"
#include "ghost.h"
#include "gputime.h"
#include "third_party/meshoptimizer/meshoptimizer.h"
#include <stddef.h>
#ifndef NR_PC
#include <psp2/gxm.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <vitashark.h>                                /* (the shader compiler's messages: shark_logm) */
#else
#include <vitasdk.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NR_PC                      /* (OpenGL ES 3's instancing, in the test machine's GL library) */
GL_APICALL void GL_APIENTRY glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei n);
GL_APICALL void GL_APIENTRY glVertexAttribDivisor(GLuint index, GLuint divisor);
#endif

gfx_switches GSW;
int gfx_nrl_lod;
long gfx_lod_tris[3];                                     /* (the parts given simpler shapes: their triangles, in those shapes) */
uint64_t gfx_lod_us;                                      /* (the time making them) */
uint64_t gfx_tex_us; int gfx_tex_n;                       /* (the textures made: the time giving them to the GPU, how many) */
#ifdef NR_PC
#include <time.h>
static uint64_t lod_now(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec * 1000000ull + t.tv_nsec / 1000; }
#else
static uint64_t lod_now(void) { return sceKernelGetProcessTimeWide(); }
#endif
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
/* a box wholly inside the view */
static int aabb_inside(const float *mn, const float *mx)
{
    for (int i = 0; i < 6; i++) {
        const float *p = planes[i];
        float x = p[0] > 0 ? mn[0] : mx[0], y = p[1] > 0 ? mn[1] : mx[1], z = p[2] > 0 ? mn[2] : mx[2];
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
/* (the three smoothsteps - fog distance, sky distance, height - as one vector) */
#define FOG_CODE \
    "  vec3 fe = w - uEye;\n" \
    "  float fd = sqrt(dot(fe, fe));\n" \
    "  float hc = clamp(w.z, -200.0, 200.0);\n" \
    "  vec3 fq = clamp((vec3(fd, fd, hc + uEye.z) - vec3(uFog.x, uSky.x, -400.0)) * vec3(uFog.y, uSky.y, 0.00125), 0.0, 1.0);\n" \
    "  fq = fq * fq * (3.0 - 2.0 * fq);\n" \
    "  float hr = abs(w.z) >= 200.0 ? clamp((uEye.z - hc) * sign(-fe.z) / max(abs(fe.z), 1.0), 0.0, 1.0) : 1.0;\n" \
    "  float fog = clamp(fq.x * hr * (1.0 - fq.z), 0.0, 1.0) * uFog.z;\n" \
    "  vFog = vec2(fog, max(fog, fq.y));\n"
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
/* traffic.fx / jet_flame.fx on level geometry: the two scrolling copies' coordinates worked out here (vRef, vLit.xy),
 * so the fragment shader reads straight from the varyings (no dependent reads: the fire walls and flames cover the
 * screen several times over) */
static const char *VS_WORLD_SCROLL = ATTRS VARYINGS FRAME_UNIFORMS
    "uniform vec4 uScv;\n" "uniform vec4 uOffv;\n"
    "void main() {\n"
    "  vec3 w = aPos;\n"
    "  vec4 p = vec4(w, 1.0);\n"
    "  gl_Position = vec4(dot(uVP0, p), dot(uVP1, p), dot(uVP2, p), dot(uVP3, p));\n"
    "  vUV = aUV;\n"
    "  float pi = aLit.a * 2.55;\n"
    "  vCol = vec4(aCol.rgb, aCol.a * 8.0 * (pi > 0.0 ? pi * uBeat.x : 1.0));\n"
    "  vRef = aUV * uScv.x + uOffv.xy;\n"
    "  vLit = vec3(aUV * uScv.y + uOffv.zw, 0.0);\n"
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
/* light_point for the four at once: att = near (far - d) / (d (far - near)) = K (far / d - 1), K = near / (far - near)
 * (uLK), far in uLR; 1 / d from one reciprocal square root (d at least 0.001), N.L as N.v / d. Four lanes of one vector
 * instead of four calls: under half the vertex shader's work */
#define LIGHTS4 \
    "  vec3 l0 = uLA0.xyz - w; vec3 l1 = uLA1.xyz - w; vec3 l2 = uLA2.xyz - w; vec3 l3 = uLA3.xyz - w;\n" \
    "  vec4 lr = inversesqrt(max(vec4(dot(l0, l0), dot(l1, l1), dot(l2, l2), dot(l3, l3)), vec4(0.000001)));\n" \
    "  vec4 la = clamp(uLK * (uLR * lr - 1.0), 0.0, 1.5) * (max(vec4(dot(n, l0), dot(n, l1), dot(n, l2), dot(n, l3)) * lr, 0.0) + uLamb);\n" \
    "  vLit = uSun.rgb * (max(n.z, 0.0) + uSun.a) + uLB0.rgb * la.x + uLB1.rgb * la.y + uLB2.rgb * la.z + uLB3.rgb * la.w;\n"
/* the same for an object lit by fewer of the level's lights (most are: by none or one). The slots left out are those
 * light_pick leaves empty, whose colour is 0 and attenuation 0: in LIGHTS4 they add exactly nothing, so the picture
 * is the same; the lights kept are worked out lane for lane as there */
#define LIGHTS0 \
    "  vLit = uSun.rgb * (max(n.z, 0.0) + uSun.a);\n"
#define LIGHTS1 \
    "  vec3 l0 = uLA0.xyz - w;\n" \
    "  float lr = inversesqrt(max(dot(l0, l0), 0.000001));\n" \
    "  float la = clamp(uLK.x * (uLR.x * lr - 1.0), 0.0, 1.5) * (max(dot(n, l0) * lr, 0.0) + uLamb.x);\n" \
    "  vLit = uSun.rgb * (max(n.z, 0.0) + uSun.a) + uLB0.rgb * la;\n"
#define LIGHTS2 \
    "  vec3 l0 = uLA0.xyz - w; vec3 l1 = uLA1.xyz - w;\n" \
    "  vec2 lr = inversesqrt(max(vec2(dot(l0, l0), dot(l1, l1)), vec2(0.000001)));\n" \
    "  vec2 la = clamp(uLK.xy * (uLR.xy * lr - 1.0), 0.0, 1.5) * (max(vec2(dot(n, l0), dot(n, l1)) * lr, 0.0) + uLamb.xy);\n" \
    "  vLit = uSun.rgb * (max(n.z, 0.0) + uSun.a) + uLB0.rgb * la.x + uLB1.rgb * la.y;\n"
static const char *VS_OBJ = ATTRS VARYINGS FRAME_UNIFORMS
    "uniform vec4 uM0;\n" "uniform vec4 uM1;\n" "uniform vec4 uM2;\n"
    "uniform vec4 uLA0;\n" "uniform vec4 uLA1;\n" "uniform vec4 uLA2;\n" "uniform vec4 uLA3;\n"
    "uniform vec4 uLB0;\n" "uniform vec4 uLB1;\n" "uniform vec4 uLB2;\n" "uniform vec4 uLB3;\n"
    "uniform vec4 uLamb;\n" "uniform vec4 uLK;\n" "uniform vec4 uLR;\n" "uniform vec4 uOCol;\n" "uniform vec4 uOFlag;\n" "uniform vec4 uVFlag;\n"
    "void main() {\n"
    "  vec4 p0 = vec4(aPos, 1.0);\n"
    "  vec3 w = vec3(dot(uM0, p0), dot(uM1, p0), dot(uM2, p0));\n"
    "  vec4 p = vec4(w, 1.0);\n"
    "  gl_Position = vec4(dot(uVP0, p), dot(uVP1, p), dot(uVP2, p), dot(uVP3, p));\n"
    "  vUV = aUV;\n"
    "  vCol = uOFlag.x > 0.5 ? uOCol : vec4(aCol.rgb, aCol.a * 8.0);\n"   /* RenderMesh Color1 set by the object's logic */
    "  vec3 n = normalize(vec3(dot(uM0.xyz, aNrm.xyz), dot(uM1.xyz, aNrm.xyz), dot(uM2.xyz, aNrm.xyz)));\n"
    LIGHTS4
    FOG_CODE SPHERE_CODE("uVFlag.x > 0.5")
    "}\n";
#define VS_OBJ_HEAD ATTRS VARYINGS FRAME_UNIFORMS \
    "uniform vec4 uM0;\n" "uniform vec4 uM1;\n" "uniform vec4 uM2;\n" \
    "uniform vec4 uLA0;\n" "uniform vec4 uLA1;\n" "uniform vec4 uLA2;\n" "uniform vec4 uLA3;\n" \
    "uniform vec4 uLB0;\n" "uniform vec4 uLB1;\n" "uniform vec4 uLB2;\n" "uniform vec4 uLB3;\n" \
    "uniform vec4 uLamb;\n" "uniform vec4 uLK;\n" "uniform vec4 uLR;\n" "uniform vec4 uOCol;\n" "uniform vec4 uOFlag;\n" "uniform vec4 uVFlag;\n"
static const char *VS_OBJ_NR = VS_OBJ_HEAD
    "void main() {\n"
    "  vec4 p0 = vec4(aPos, 1.0);\n"
    "  vec3 w = vec3(dot(uM0, p0), dot(uM1, p0), dot(uM2, p0));\n"
    "  vec4 p = vec4(w, 1.0);\n"
    "  gl_Position = vec4(dot(uVP0, p), dot(uVP1, p), dot(uVP2, p), dot(uVP3, p));\n"
    "  vUV = aUV;\n"
    "  vCol = uOFlag.x > 0.5 ? uOCol : vec4(aCol.rgb, aCol.a * 8.0);\n"
    "  vec3 n = normalize(vec3(dot(uM0.xyz, aNrm.xyz), dot(uM1.xyz, aNrm.xyz), dot(uM2.xyz, aNrm.xyz)));\n"
    LIGHTS4
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
/* the same for objects (traffic.fx / jet_flame.fx: the car's flames, the traffic) */
static const char *VS_OBJ_SCROLL = VS_OBJ_HEAD
    "uniform vec4 uScv;\n" "uniform vec4 uOffv;\n"
    "void main() {\n"
    "  vec4 p0 = vec4(aPos, 1.0);\n"
    "  vec3 w = vec3(dot(uM0, p0), dot(uM1, p0), dot(uM2, p0));\n"
    "  vec4 p = vec4(w, 1.0);\n"
    "  gl_Position = vec4(dot(uVP0, p), dot(uVP1, p), dot(uVP2, p), dot(uVP3, p));\n"
    "  vUV = aUV;\n"
    "  vCol = uOFlag.x > 0.5 ? uOCol : vec4(aCol.rgb, aCol.a * 8.0);\n"
    "  vRef = aUV * uScv.x + uOffv.xy;\n"
    "  vLit = vec3(aUV * uScv.y + uOffv.zw, 0.0);\n"
    FOG_CODE
    "}\n";
/* phong_ref_intro.fx's vertex shader (the warp-in / warp-out stand-in, IntroCarLogic 0x68d0e0): each triangle
 * leaves for the portal (uPortal: direction and distance, model space) when its own t passes 0, bulging out along
 * its normal on the way; t = k + hash / 4 + (n.P / 2 + 1/2) / 2 + (pos.P / 2 + 1/2) / 4, k = 1.25 - 2.7 progress
 * (uIntro.x); the hash frac((3.1 + n.x) 243) comes in aLit.x. vT carries t to the glow */
static const char *VS_OBJ_INTRO = VS_OBJ_HEAD
    "uniform vec4 uIntro;\n" "uniform vec4 uPortal;\n" "varying float vT;\n"
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
    LIGHTS4
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
    "  vec3 eg = clamp(vec3(texture2D(uTri, vUV).rgb) - 0.3, 0.0, 1.0);\n"
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
/* the glow but glass (uMat.w 0): FS_ADD as it works out for it - the mix is the texture - with less work a pixel, for
 * the glow can cover the screen twice over (the cooling pads close by). (Glass keeps FS_ADD: the grey alone came out a
 * shade different from the mix at 1 on the PC's GPU) */
static const char *FS_ADD0 = FS_HEAD
    "void main() {\n"
    "  vec3 c = vec3(texture2D(uDiff, vUV).rgb) * vCol.rgb * (vCol.a * uMat.x * (1.0 - vFog.y));\n"
    "  gl_FragColor = vec4(c, max(c.r, max(c.g, c.b)) * 0.25);\n"
    "}\n";
/* the sprites (the jets' glow, the explosion): FS_ADD as it works out for them (uMat = 1, 0, 0, 0: the mix is t, the
 * factor 1) and their texture grey (r = g = b): the same colours with less work, for the explosion's flash covers the
 * screen several times over */
static const char *FS_SPRITE = FS_HEAD
    "void main() {\n"
    "  vec3 c = float(texture2D(uDiff, vUV).r) * vCol.rgb * (vCol.a * (1.0 - vFog.y));\n"
    "  gl_FragColor = vec4(c, max(c.r, max(c.g, c.b)) * 0.25);\n"
    "}\n";
/* the particles (particlesnew.fx, re_F2 4.3), moved here instead of by the CPU: age t = now - birth; at pos + vel t
 * (gravity on z: the PC adds it to vel.z every frame); a camera-facing quad of half size 'size', turned by its rotation;
 * dead (age outside 0..1): no area. The slice of the 64-slice texture is w = tex + dtex t, between two slices blended
 * as the volume texture's filter does; the colour is the palette at (age, row) x intensity, added (re_F2 4.3: out = t x
 * colour). aPos: where and when it was made; aUV: velocity, gravity; aNrm: 1 / life, half size, rotation, its rate;
 * aCol: slice, slices a second, palette row, intensity; aLit: the corner */
static const char *VS_PART =
    "attribute vec4 aPos;\n" "attribute vec4 aUV;\n" "attribute vec4 aNrm;\n" "attribute vec4 aCol;\n" "attribute vec4 aLit;\n"
    "uniform vec4 uVP0;\n" "uniform vec4 uVP1;\n" "uniform vec4 uVP2;\n" "uniform vec4 uVP3;\n"
    "uniform vec4 uPN;\n" "uniform vec3 uPR;\n" "uniform vec3 uPU;\n" "uniform vec4 uPK;\n"
    "varying vec2 vUV;\n" "varying vec2 vUV1;\n" "varying vec2 vPal;\n" "varying vec2 vS;\n"
    "void main() {\n"
    "  float t = uPN.x - aPos.w;\n"
    "  float age = t * aNrm.x;\n"
    "  float alive = step(0.0, age) * (1.0 - step(1.0, age));\n"
    "  vec3 p = aPos.xyz + aUV.xyz * t;\n"
    "  p.z += 0.5 * aUV.w * t * t;\n"
    "  float ang = aNrm.z + aNrm.w * t;\n"
    "  float c = cos(ang), s = sin(ang);\n"
    /* uPK (gfx_particles_scale): shrinking from full size at a depth of uPK.x + 1 / uPK.y in front of the camera to
     * nothing at uPK.x, the draw's size x uPK.z and brightness x uPK.w (as made but for the short-lived ones: see
     * particles_draw) */
    "  float nk = clamp((dot(uVP3, vec4(p, 1.0)) - uPK.x) * uPK.y, 0.0, 1.0);\n"
    "  vec2 o = aLit.xy * (aNrm.y * alive * nk * uPK.z);\n"
    "  vec3 w = p + uPR * (c * o.x + s * o.y) + uPU * (c * o.y - s * o.x);\n"
    "  vec4 h = vec4(w, 1.0);\n"
    "  gl_Position = vec4(dot(uVP0, h), dot(uVP1, h), dot(uVP2, h), dot(uVP3, h));\n"
    "  vec2 uv = vec2(0.5 + 0.5 * aLit.x, 0.5 - 0.5 * aLit.y) * (63.0 / 512.0) + 0.5 / 512.0;\n"
    "  vPal = vec2(clamp(age, 0.0, 1.0), aCol.z);\n"
    /* the two slices and the blend between them: the same at the quad's four corners, so worked out here, and the
     * fragment shader's reads come straight from the varyings (no dependent reads, slow on the Vita's GPU) */
    "  float sl = (aCol.x + aCol.y * t) * 64.0 - 0.5;\n"
    "  float s0 = floor(sl);\n"
    "  vS = vec2(sl - s0, aCol.w * uPK.w);\n"
    "  s0 = mod(s0, 64.0);\n"
    "  float s1 = mod(s0 + 1.0, 64.0);\n"
    "  vUV = vec2(mod(s0, 8.0), floor(s0 * 0.125)) * 0.125 + uv;\n"
    "  vUV1 = vec2(mod(s1, 8.0), floor(s1 * 0.125)) * 0.125 + uv;\n"
    "}\n";
static const char *FS_PART =
    "uniform sampler2D uDiff;\n" "uniform sampler2D uPal;\n"
    "varying vec2 vUV;\n" "varying vec2 vUV1;\n" "varying vec2 vPal;\n" "varying vec2 vS;\n"
    "void main() {\n"
    "  vec3 t = mix(vec3(texture2D(uDiff, vUV).rgb), vec3(texture2D(uDiff, vUV1).rgb), float(vS.x));\n"
    "  vec3 c = t * vec3(texture2D(uPal, vPal).rgb) * float(vS.y);\n"
    "  gl_FragColor = vec4(c, max(c.r, max(c.g, c.b)) * 0.25);\n"
    "}\n";
/* the same when every particle of the draw stays on one slice (no Texture_Delta, no randomness, a whole Texture_Index:
 * the blend's weight vS.x is exactly 0, so the second slice adds nothing): one read less a pixel (MountainTop's
 * VolcanoSmoke, the only darkening emitter: its puffs cover the screen ten times over as the car flies through it) */
static const char *FS_PART1 =
    "uniform sampler2D uDiff;\n" "uniform sampler2D uPal;\n"
    "varying vec2 vUV;\n" "varying vec2 vPal;\n" "varying vec2 vS;\n"
    "void main() {\n"
    "  vec3 c = vec3(texture2D(uDiff, vUV).rgb) * vec3(texture2D(uPal, vPal).rgb) * float(vS.y);\n"
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
static const char *FS_SCROLL =
    "#ifdef vec4\n#define FV3 float3\n#else\n#define FV3 vec3\n#endif\n"        /* (vLit carries coordinates here: float) */
    "varying vec2 vUV;\n" "varying vec4 vCol;\n" "varying FV3 vLit;\n" "varying vec2 vFog;\n" "varying vec2 vRef;\n"
    "uniform sampler2D uDiff;\n" "uniform sampler2D uEmit;\n" "uniform sampler2D uRefl;\n"
    "uniform vec4 uMat;\n" "uniform vec4 uTint;\n" "uniform vec3 uFogCol;\n" "uniform vec3 uSkyCol;\n"
    "uniform vec4 uFT;\n" "uniform vec4 uMP;\n" "uniform vec4 uMC2;\n"
    "uniform vec4 uSc;\n" "uniform vec4 uC1;\n" "uniform vec4 uC2;\n"
    "void main() {\n"
    "  vec4 a = texture2D(uDiff, vRef);\n"
    "  vec4 b = texture2D(uDiff, vLit.xy);\n"
    "  float m = max(dot(a, uC1) + dot(b, uC2) + uSc.z, 0.0);\n"
    "  vec3 c = float(texture2D(uDiff, vUV).r) * m * vCol.rgb * (vCol.a * uMat.x * (1.0 - vFog.y));\n"
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
    "  vec3 c = vec3(texture2D(uDiff, vUV).rgb) * uTint.rgb;\n"
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
    "  c += vec3(texture2D(uB1, vUV).rgb) * uBloom.w;\n" \
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
    "  gl_FragColor = vec4(texture2D(uTex, vUV)) * 0.2857 + (vec4(texture2D(uTex, vS1)) + vec4(texture2D(uTex, vS2))) * 0.2143\n"
    "               + (vec4(texture2D(uTex, vS3)) + vec4(texture2D(uTex, vS4))) * 0.1429;\n"
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

/* ---- the screen passes as they are drawn now: four render targets after the scene instead of five (each one is a
 * GPU scene of its own on the Vita, a fixed cost), and every texture read at a coordinate that comes straight from
 * the vertex shader (reads at coordinates worked out per pixel make the Vita's GPU wait). The quarter-size target
 * holds the bright parts (left) and a quarter-size copy of the scene (right), the latter for the speed blur, which
 * reads that small picture instead of scattered texels of the big one; the blur target holds the glow (left) and the
 * speed blur (right). Columns between the halves repeat their edge columns, so a read past a half's edge gets what
 * the edge of a texture of its own would give (CLAMP_TO_EDGE) */
static const char *VS_TAP4 =
    "attribute vec2 aPos;\n" "attribute vec2 aUV;\n" "uniform vec4 uTap;\n"
    "varying vec2 vT0;\n" "varying vec2 vT1;\n" "varying vec2 vT2;\n" "varying vec2 vT3;\n"
    "void main() {\n"
    "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "  vT0 = aUV - uTap.xy; vT1 = aUV + vec2(uTap.x, -uTap.y); vT2 = aUV + vec2(-uTap.x, uTap.y); vT3 = aUV + uTap.xy;\n"
    "}\n";
#define TAP4_IN "varying vec2 vT0;\n" "varying vec2 vT1;\n" "varying vec2 vT2;\n" "varying vec2 vT3;\n" "uniform sampler2D uTex;\n"
/* the bright parts, as FS_DOWN */
static const char *FS_DOWN4 = TAP4_IN "uniform vec4 uStep;\n"
    "vec3 dec(vec4 c) { float m = max(c.r, max(c.g, c.b)); return c.rgb * min(c.a * 4.0 / max(m, 0.004), 2.5); }\n"
    "void main() {\n"
    "  vec3 s = dec(texture2D(uTex, vT0)) + dec(texture2D(uTex, vT1)) + dec(texture2D(uTex, vT2)) + dec(texture2D(uTex, vT3));\n"
    "  gl_FragColor = vec4(max(s * 0.25 - uStep.z, 0.0) * 0.25, 1.0);\n"
    "}\n";
/* the scene at a quarter size (each texel the mean of its 4 x 4) */
static const char *FS_COPY4 = TAP4_IN
    "void main() { gl_FragColor = (vec4(texture2D(uTex, vT0)) + vec4(texture2D(uTex, vT1)) + vec4(texture2D(uTex, vT2)) + vec4(texture2D(uTex, vT3))) * 0.25; }\n";
#define TAP9_OUT "varying vec2 vT0;\n" "varying vec2 vT1;\n" "varying vec2 vT2;\n" "varying vec2 vT3;\n" "varying vec2 vT4;\n" \
    "varying vec2 vT5;\n" "varying vec2 vT6;\n" "varying vec2 vT7;\n" "varying vec2 vT8;\n"
/* as FS_WIDE: the texel, its four diagonal neighbours, four at 3.5 texels on the axes */
static const char *VS_TAP9W =
    "attribute vec2 aPos;\n" "attribute vec2 aUV;\n" "uniform vec4 uTap;\n" TAP9_OUT
    "void main() {\n"
    "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "  vec2 a = uTap.xy, b = vec2(a.x, -a.y), c3 = uTap.xy * 3.5;\n"
    "  vT0 = aUV; vT1 = aUV + a; vT2 = aUV - a; vT3 = aUV + b; vT4 = aUV - b;\n"
    "  vT5 = aUV + vec2(c3.x, 0.0); vT6 = aUV - vec2(c3.x, 0.0); vT7 = aUV + vec2(0.0, c3.y); vT8 = aUV - vec2(0.0, c3.y);\n"
    "}\n";
static const char *FS_WIDE9 = TAP9_OUT "uniform sampler2D uTex;\n"
    "void main() {\n"
    "  vec3 c = vec3(texture2D(uTex, vT0).rgb) * 0.16\n"
    "    + (vec3(texture2D(uTex, vT1).rgb) + vec3(texture2D(uTex, vT2).rgb) + vec3(texture2D(uTex, vT3).rgb) + vec3(texture2D(uTex, vT4).rgb)) * 0.12\n"
    "    + (vec3(texture2D(uTex, vT5).rgb) + vec3(texture2D(uTex, vT6).rgb) + vec3(texture2D(uTex, vT7).rgb) + vec3(texture2D(uTex, vT8).rgb)) * 0.09;\n"
    "  gl_FragColor = vec4(c, 1.0);\n"
    "}\n";
/* as FS_BLUR: the texel, four on the axes and four diagonal at 1.5 texels; the wide glow (its target a quarter of
 * this one's size each way, so the same coordinate) added in */
static const char *VS_TAP9B =
    "attribute vec2 aPos;\n" "attribute vec2 aUV;\n" "uniform vec4 uTap;\n" TAP9_OUT
    "void main() {\n"
    "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "  vec2 a = uTap.xy * 1.5, b = vec2(a.x, -a.y);\n"
    "  vT0 = aUV; vT1 = aUV + vec2(a.x, 0.0); vT2 = aUV - vec2(a.x, 0.0); vT3 = aUV + vec2(0.0, a.y); vT4 = aUV - vec2(0.0, a.y);\n"
    "  vT5 = aUV + a; vT6 = aUV - a; vT7 = aUV + b; vT8 = aUV - b;\n"
    "}\n";
static const char *FS_BLUR9 = TAP9_OUT "uniform sampler2D uTex;\n" "uniform sampler2D uB2;\n" "uniform vec4 uStep;\n"
    "void main() {\n"
    "  vec3 c = vec3(texture2D(uTex, vT0).rgb) * 0.2\n"
    "    + (vec3(texture2D(uTex, vT1).rgb) + vec3(texture2D(uTex, vT2).rgb) + vec3(texture2D(uTex, vT3).rgb) + vec3(texture2D(uTex, vT4).rgb)) * 0.12\n"
    "    + (vec3(texture2D(uTex, vT5).rgb) + vec3(texture2D(uTex, vT6).rgb) + vec3(texture2D(uTex, vT7).rgb) + vec3(texture2D(uTex, vT8).rgb)) * 0.08;\n"
    "  gl_FragColor = vec4((c * uStep.z + vec3(texture2D(uB2, vT0).rgb) * uStep.w) * 0.5, 1.0);\n"
    "}\n";
/* the speed blur from the quarter-size copy: as VS_POST_SPEED, the taps (screen coordinates) mapped into the copy's
 * part of its target (uMap: scale, offset) */
static const char *VS_RADIAL2 =
    "attribute vec2 aPos;\n" "attribute vec2 aUV;\n" "uniform vec4 uStep;\n" "uniform vec4 uMap;\n"
    "varying vec2 vUV;\n" "varying vec2 vS1;\n" "varying vec2 vS2;\n" "varying vec2 vS3;\n" "varying vec2 vS4;\n"
    "void main() {\n"
    "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "  vec2 q = aUV - 0.5; vec2 d = q * (length(q) * uStep.x * 0.18);\n"
    "  vUV = aUV * uMap.xy + uMap.zw; vS1 = (aUV - d) * uMap.xy + uMap.zw; vS2 = (aUV + d) * uMap.xy + uMap.zw;\n"
    "  vS3 = (aUV - 2.0 * d) * uMap.xy + uMap.zw; vS4 = (aUV + 2.0 * d) * uMap.xy + uMap.zw;\n"
    "}\n";
/* the final passes: the glow (and the speed blur) read where they are in their target (uMap, uMapR) */
static const char *VS_POST_EDGE2 =
    "attribute vec2 aPos;\n" "attribute vec2 aUV;\n" "uniform vec4 uEdge;\n" "uniform vec4 uScale;\n" "uniform vec4 uMap;\n"
    "varying vec2 vUV;\n" "varying vec2 vSc;\n" "varying vec4 vE;\n"
    "void main() {\n"
    "  gl_Position = vec4(aPos, 0.0, 1.0); vUV = aUV * uMap.xy + uMap.zw; vSc = aUV * uScale.xy;\n"
    "  vec2 q = (aUV - 0.5) * vec2(1.7647, 1.0);\n"
    "  vE = vec4(uEdge.rgb, clamp((dot(q, q) - 0.12) * uEdge.a * 3.0, 0.0, 1.0));\n"
    "}\n";
static const char *VS_POST_MIX2 =
    "attribute vec2 aPos;\n" "attribute vec2 aUV;\n" "uniform vec4 uStep;\n" "uniform vec4 uEdge;\n" "uniform vec4 uScale;\n"
    "uniform vec4 uMap;\n" "uniform vec4 uMapR;\n"
    "varying vec2 vUV;\n" "varying vec2 vR;\n" "varying vec2 vSc;\n" "varying float vW;\n" "varying vec4 vE;\n"
    "void main() {\n"
    "  gl_Position = vec4(aPos, 0.0, 1.0); vUV = aUV * uMap.xy + uMap.zw; vR = aUV * uMapR.xy + uMapR.zw; vSc = aUV * uScale.xy;\n"
    "  vec2 q = aUV - 0.5; vec2 d = q * (length(q) * uStep.x * 0.18);\n"
    "  vW = clamp((length(d * vec2(960.0, 544.0)) - 3.0) * 0.2, 0.0, 1.0);\n"
    "  vec2 e = q * vec2(1.7647, 1.0);\n"
    "  vE = vec4(uEdge.rgb, clamp((dot(e, e) - 0.12) * uEdge.a * 3.0, 0.0, 1.0));\n"
    "}\n";
static const char *FS_COMP_SPEED2 = COMP_HEAD
    "varying float vW;\n" "varying vec2 vR;\n" "uniform sampler2D uB3;\n"
    "void main() {\n"
    "  vec3 c = mix(vec3(texture2D(uTex, vSc).rgb), vec3(texture2D(uB3, vR).rgb), vW);\n"
    COMP_TAIL
    "}\n";

typedef struct {
    GLuint prog;
    GLint uVP[4], uEye, uSun, uFog, uSky, uV[3], uM[3], uLA[4], uLB[4], uLamb, uLK, uLR;
    GLint uMat, uTint, uFogCol, uSkyCol;
    GLint uStep, uBloom, uEdge, uFx, uScale, uMap, uMapR, uTap;
    GLint uSc, uOff, uC1, uC2, uScv, uOffv;
    GLint uBeat, uOCol, uOFlag;
    GLint uFT, uMP, uMC2, uVFlag;
    GLint uRB, uRB2, uRB3, uRBE;
    GLint uIntro, uPortal;
    GLint uPN, uPR, uPU, uPK; /* the particles: clock, camera right, up, near shrink and scale */
    int stamp;                /* vp_id its frame uniforms were last set for */
    int lit;                  /* an object program with the level's lights: 1 + how many it works out (5: all four) */
} prog_t;
enum { P_WORLD, P_WREF, P_WTV, P_WADD, P_WSCROLL, P_WGLASER, P_SKY, P_OBJ, P_OBJADD, P_OBJSCROLL, P_OBJTV,
       P_WPULSE, P_WVEINS, P_WANIM, P_WTRON, P_WCITY, P_OBJPULSE, P_OBJVEINS, P_OBJANIM, P_OBJLIT, P_WADDG, P_OBJADDG,
       P_DOWN, P_BLUR, P_WIDE, P_COMP, P_HALF, P_RADIAL, P_COMP_SPEED,
       P_DOWN4, P_COPY4, P_WIDE9, P_BLUR9, P_RADIAL2, P_COMP2, P_COMP_SPEED2,
       P_OBJINTRO, P_TRACK, P_TRACKDARK, P_PART, P_PART1, P_SPRITE, P_COUNT };   /* (after the screen passes: no cinematics' variants) */
static int prog_additive(int p) { return p == P_WADD || p == P_WADDG || p == P_WSCROLL || p == P_WGLASER || p == P_WVEINS || p == P_OBJADD || p == P_OBJADDG || p == P_OBJSCROLL || p == P_OBJVEINS; }
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
    static int hp = -1; if (hp < 0) hp = getenv("FSHIGHP") != NULL;
    const char *srcs[3] = { frag ? (hp ? "precision highp float;\n" : FS_PREFIX) : "", NRPC_VGL_MACROS, src };
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
static int cache_loaded, cache_built, bank_loaded, bank_missed;
#ifndef NR_PC
extern unsigned gfx_shader_opts;
static uint32_t src_hash(const char *a, const char *b, const char *c)
{
    uint32_t h = 2166136261u;
    for (const char *s = a; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    h = (h ^ 0xFF) * 16777619u;
    for (const char *s = b; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    for (const char *s = c; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    if (gfx_shader_opts) h = (h ^ gfx_shader_opts) * 16777619u;   /* (other compiler settings: other programs) */
    return h;
}
/* ---- the built-in programs (app0:shbank.bin)
 * Every program the game builds, compiled once by the Vita's own shader compiler and shipped inside the VPK: the
 * cache files of a Vita run (the .prg files in ux0:data/nitronic/cache: what glGetProgramBinary gave) packed by
 * tools/shbank.py. With them the game starts without compiling anything, and does not need the compiler
 * (ur0:data/libshacccg.suprx) at all. A program found neither here nor in the cache is compiled as before - and
 * logged, for the bank to be made again (a shader changed, or vitaGL did: a binary vitaGL no longer takes fails to
 * link and is compiled too). The file: "NRSB", 1, the count, then per program its source hash, binary format,
 * offset and length (sorted by hash), then the binaries.
 * Every program a run builds is also listed in ux0:data/nitronic/cache/used.txt (build label, hash, name, kind):
 * what tools/shbank.py packs. */
static uint8_t *bank; static uint32_t bank_n, bank_size;
static FILE *used_f;
static void bank_open(void)
{
    FILE *f = fopen("app0:shbank.bin", "rb");
    if (f) {
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        if (n > 12 && n < (16 << 20) && (bank = malloc(n)) && fread(bank, 1, n, f) == (size_t)n && !memcmp(bank, "NRSB", 4)
            && ((uint32_t *)bank)[1] == 1 && 12 + 16 * (uint64_t)((uint32_t *)bank)[2] <= (uint64_t)n) {
            bank_n = ((uint32_t *)bank)[2]; bank_size = (uint32_t)n;
        } else { free(bank); bank = NULL; }
        fclose(f);
    }
    logm("built-in programs: %u%s", bank_n, bank_n ? " (app0:shbank.bin)" : "");
    sceIoMkdir("ux0:data/nitronic", 0777); sceIoMkdir("ux0:data/nitronic/cache", 0777);
    SceIoStat st;                                      /* (kept short: begun again past 256 KB, some 50 starts) */
    const int big = sceIoGetstat("ux0:data/nitronic/cache/used.txt", &st) >= 0 && st.st_size > (256 << 10);
    used_f = fopen("ux0:data/nitronic/cache/used.txt", big ? "w" : "a");
}
static const void *bank_find(uint32_t h, GLenum *fmt, GLsizei *len)
{
    const uint32_t *e = (const uint32_t *)bank + 3;
    int lo = 0, hi = (int)bank_n - 1;
    while (lo <= hi) {
        const int m = (lo + hi) / 2;
        if (e[4 * m] == h) {
            if ((uint64_t)e[4 * m + 2] + e[4 * m + 3] > bank_size) return NULL;
            *fmt = e[4 * m + 1]; *len = (GLsizei)e[4 * m + 3];
            return bank + e[4 * m + 2];
        }
        if (e[4 * m] < h) lo = m + 1; else hi = m - 1;
    }
    return NULL;
}
static int build_cg;                                   /* (see make_half_programs) */
static void used_note(uint32_t h, const char *name)
{
    /* (the last word: "plain" for the game's own programs as they are by default; "cg" for the half-precision ones,
     * "opts" when built with other compiler settings - tools/shbank.py leaves those out unless asked) */
    if (used_f) { fprintf(used_f, "%s %08x %s %s\n", NR_BUILD_LABEL, (unsigned)h, name, build_cg ? "cg" : gfx_shader_opts ? "opts" : "plain"); fflush(used_f); }
}
/* the compiler's own messages, to the log (vitaGL keeps them to itself unless built to log them) */
static void shark_logm(const char *msg, shark_log_level lv, int line)
{
    logm("    the shader compiler: %s, line %d: %s", lv == SHARK_LOG_ERROR ? "error" : lv == SHARK_LOG_WARNING ? "warning" : "note", line, msg ? msg : "");
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
        glBindAttribLocation(prog, 5, "aI0");          /* (the instanced programs: the model matrix's rows) */
        glBindAttribLocation(prog, 6, "aI1");
        glBindAttribLocation(prog, 7, "aI2");
    }
}
static int build1(prog_t *o, const char *vsrc, const char *fsrc, const char *name, int post);
/* ---- the half-precision trial (the switch file "half_test"; the bench's "half precision" ways)
 * The Vita's GPU works a colour out in half precision (16-bit floats: 11 significant bits against the picture's 8)
 * with less work than in float, and Sony's shader language (Cg) has the types for it; GLSL as vitaGL translates it
 * has not: every float is a 32-bit one, and every '*' becomes a call of a function that takes floats (vglMul), so a
 * shader cannot ask for less. (1.0.1-dev18 tried, renaming the types in the GLSL: uniforms and varyings came out as
 * half too, and the Vita stopped at the first such program.) So the trial's fragment shaders are written in Cg
 * outright - vitaGL takes them as GL_CG_FRAGMENT_SHADER_EXT - beside the game's own vertex shaders: uniforms and
 * texture coordinates stay float, the colours are half. vitaGL numbers a GLSL vertex shader's varyings in the order
 * they are declared when its fragment shader is Cg (TEXCOORD0 on: tools/pc/vglsl shows the translation), which is the
 * order the inputs below are given in.
 * Four of the costliest: the scrolling glow (fire walls, flames: screens of it), the plain glow, the one-slice
 * particles (smoke), the final pass (every pixel of every frame). The bench draws its view with each in turn; the
 * game itself does not use them. Each is built twice where it reads textures: the reads as Cg gives them (float,
 * then to half) and asked for in half outright (tex2D<half4>: Sony's extension), should the compiler take that. */
#ifndef NR_PC
#ifndef GL_CG_FRAGMENT_SHADER_EXT
#define GL_CG_FRAGMENT_SHADER_EXT 0x890F
#endif
/* (the scrolling glow's mask m and its scale stay float: the masks' weights uC1 / uC2 run to several units and cancel
 * against uSc.z, and the scale (vCol.a: up to 8 x the beat) multiplies what is left - in half, that came out up to 12/255
 * off on the Vita's picture; only the colour's three channels are worked in half) */
#define CG_SCROLL(TEX) \
    "uniform sampler2D uDiff;\n" "uniform float4 uMat;\n" "uniform float4 uSc;\n" "uniform float4 uC1;\n" "uniform float4 uC2;\n" \
    "void main(float2 vUV : TEXCOORD0, half4 vCol : TEXCOORD1, float3 vLit : TEXCOORD2, half2 vFog : TEXCOORD3, float2 vRef : TEXCOORD4,\n" \
    "          out half4 oCol : COLOR)\n" \
    "{\n" \
    "  half4 a = " TEX "(uDiff, vRef));\n" \
    "  half4 b = " TEX "(uDiff, vLit.xy));\n" \
    "  half4 t = " TEX "(uDiff, vUV));\n" \
    "  float m = max(dot(float4(a), uC1) + dot(float4(b), uC2) + uSc.z, 0.0);\n" \
    "  half3 c = vCol.rgb * half(t.r * m * vCol.a * uMat.x * (1.0 - vFog.y));\n" \
    "  oCol = half4(c, max(c.r, max(c.g, c.b)) * 0.25);\n" \
    "}\n"
#define CG_ADD0(TEX) \
    "uniform sampler2D uDiff;\n" "uniform float4 uMat;\n" \
    "void main(float2 vUV : TEXCOORD0, half4 vCol : TEXCOORD1, half2 vFog : TEXCOORD3, out half4 oCol : COLOR)\n" \
    "{\n" \
    "  half4 t = " TEX "(uDiff, vUV));\n" \
    "  half3 c = t.rgb * vCol.rgb * (vCol.a * half(uMat.x) * (1.0 - vFog.y));\n" \
    "  oCol = half4(c, max(c.r, max(c.g, c.b)) * 0.25);\n" \
    "}\n"
#define CG_PART1(TEX) \
    "uniform sampler2D uDiff;\n" "uniform sampler2D uPal;\n" \
    "void main(float2 vUV : TEXCOORD0, float2 vPal : TEXCOORD2, half2 vS : TEXCOORD3, out half4 oCol : COLOR)\n" \
    "{\n" \
    "  half4 t = " TEX "(uDiff, vUV));\n" \
    "  half4 q = " TEX "(uPal, vPal));\n" \
    "  half3 c = t.rgb * q.rgb * vS.y;\n" \
    "  oCol = half4(c, max(c.r, max(c.g, c.b)) * 0.25);\n" \
    "}\n"
#define CG_COMP_TAIL \
    "  c += b.rgb * half(uBloom.w);\n" \
    "  c *= half(1.0 + uFx.z);\n" \
    "  c = lerp(c, vE.rgb, vE.a);\n" \
    "  c += half(uFx.y) * half3(0.55, 0.75, 1.0);\n" \
    "  oCol = half4(c, 1.0);\n" \
    "}\n"
#define CG_COMP(TEX) \
    "uniform sampler2D uTex;\n" "uniform sampler2D uB1;\n" "uniform float4 uBloom;\n" "uniform float4 uFx;\n" \
    "void main(float2 vUV : TEXCOORD0, float2 vSc : TEXCOORD1, half4 vE : TEXCOORD2, out half4 oCol : COLOR)\n" \
    "{\n" \
    "  half4 s = " TEX "(uTex, vSc));\n" \
    "  half4 b = " TEX "(uB1, vUV));\n" \
    "  half3 c = s.rgb;\n" \
    CG_COMP_TAIL
#define CG_COMP_SPEED(TEX) \
    "uniform sampler2D uTex;\n" "uniform sampler2D uB1;\n" "uniform sampler2D uB3;\n" "uniform float4 uBloom;\n" "uniform float4 uFx;\n" \
    "void main(float2 vUV : TEXCOORD0, float2 vR : TEXCOORD1, float2 vSc : TEXCOORD2, half vW : TEXCOORD3, half4 vE : TEXCOORD4,\n" \
    "          out half4 oCol : COLOR)\n" \
    "{\n" \
    "  half4 s = " TEX "(uTex, vSc));\n" \
    "  half4 r = " TEX "(uB3, vR));\n" \
    "  half4 b = " TEX "(uB1, vUV));\n" \
    "  half3 c = lerp(s.rgb, r.rgb, vW);\n" \
    CG_COMP_TAIL
#define CG_READ "half4(tex2D"                         /* (the read as Cg gives it, then to half) */
#define CG_READH "(tex2D<half4>"                      /* (the read asked for in half) */
#endif
/* ---- the game's own fragment shaders in Cg with their colour work in half precision (the switch file "half_all")
 * The trial above is four shaders written out by hand. This makes the same of any of the game's fragment shaders
 * from its GLSL, for they are all of one plain kind: declarations a line each, perhaps a helper function, main; a
 * handful of functions (texture2D, mix, max, min, clamp, dot, abs, floor); every texture read at a coordinate that
 * is a varying as it comes (tyre tracks apart: those stay GLSL, as does the city's, whose windows are a pattern worked
 * out from a varying). In the Cg:
 *   - uniforms stay float, and each vec3 / vec4 one is copied to a half at the top of main (worked out once a draw,
 *     not a pixel) - but the clocks and the cinematics' distances (CG_KEEP), which stay float where they are used;
 *   - the varyings are main's inputs, TEXCOORD by their place in the vertex shader's list (vitaGL numbers a GLSL
 *     vertex shader's that way beside a Cg fragment shader): the texture coordinates float (CG_COORD), the rest half;
 *   - vec3, vec4 and float in the body become half; vec2 stays float (the few there are work on coordinates);
 *   - a texture read gives half (texH).
 * A float uniform or coordinate in an expression makes that expression float, as Cg's rules have it, so nothing that
 * needs the precision loses it. cg_half returns NULL for a shader that is not of that kind (it is built as GLSL
 * then). tools/pc/cgcheck.sh has NVIDIA's Cg compiler check every one and draws the game with them on the PC. */
static const char *const CG_COORD[] = { "vUV", "vUV1", "vRef", "vPal", "vSc", "vR", "vT0", "vT1", "vT2", "vT3", "vT4", "vT5", "vT6",
                                        "vT7", "vT8", "vS1", "vS2", "vS3", "vS4", "vD", NULL };
static const char *const CG_KEEP[] = { "uFT", "uRB", "uRB2", "uRB3", "uRBE", "uIntro", "uPortal", NULL };
typedef struct { char *p; size_t n, cap; int bad; } cg_buf;
static void cg_add(cg_buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) { size_t c = (b->cap ? b->cap * 2 : 2048) + n; char *q = realloc(b->p, c); if (!q) { b->bad = 1; return; } b->p = q; b->cap = c; }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
static void cg_str(cg_buf *b, const char *s) { cg_add(b, s, strlen(s)); }
static int cg_idc(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
static int cg_in(const char *const *list, const char *s, size_t n) { for (; *list; list++) if (strlen(*list) == n && !memcmp(*list, s, n)) return 1; return 0; }
typedef struct { char name[24]; int comps, coord; } cg_var;
/* "TYPE NAME;" (after "varying " / "uniform "): the name and the type's components (0: a sampler, or not one of ours) */
static int cg_decl(const char *q, char *name, size_t nmax, int *comps, int *fv3)
{
    const char *t = q; while (cg_idc(*q)) q++;
    size_t tl = (size_t)(q - t);
    *comps = tl == 5 && !memcmp(t, "float", 5) ? 1 : tl == 4 && !memcmp(t, "vec2", 4) ? 2 : tl == 4 && !memcmp(t, "vec3", 4) ? 3 :
             tl == 4 && !memcmp(t, "vec4", 4) ? 4 : tl == 3 && !memcmp(t, "FV3", 3) ? 3 : 0;
    if (fv3) *fv3 = tl == 3 && !memcmp(t, "FV3", 3);
    while (*q == ' ') q++;
    const char *n0 = q; while (cg_idc(*q)) q++;
    if (q == n0 || (size_t)(q - n0) >= nmax || *q != ';') return 0;
    memcpy(name, n0, (size_t)(q - n0)); name[q - n0] = 0;
    return 1;
}
/* a stretch of the shader's code, its names changed for Cg's (hoist: the uniforms that have a half copy) */
static void cg_code(cg_buf *o, const char *s, const char *end, const cg_var *hoist, int nhoist)
{
    static const char *const FROM[] = { "vec2", "vec3", "vec4", "float", "mix", "texture2D", "gl_FragColor", NULL };
    static const char *const TO[] = { "float2", "half3", "half4", "half", "lerp", "texH", "oCol" };
    while (s < end) {
        if ((*s >= '0' && *s <= '9') || (*s == '.' && s + 1 < end && s[1] >= '0' && s[1] <= '9')) {   /* a number, as it is */
            const char *q = s; while (q < end && ((*q >= '0' && *q <= '9') || *q == '.')) q++;
            if (q < end && (*q == 'e' || *q == 'E')) { q++; if (q < end && (*q == '+' || *q == '-')) q++; while (q < end && *q >= '0' && *q <= '9') q++; }
            cg_add(o, s, (size_t)(q - s)); s = q;
        } else if (cg_idc(*s)) {
            const char *q = s; while (q < end && cg_idc(*q)) q++;
            const size_t n = (size_t)(q - s);
            const int member = o->n && o->p[o->n - 1] == '.';      /* (.rgb, .x: not a name of ours) */
            int k = -1, h = -1;
            if (!member) {
                for (int i = 0; FROM[i]; i++) if (strlen(FROM[i]) == n && !memcmp(FROM[i], s, n)) k = i;
                for (int i = 0; i < nhoist; i++) if (strlen(hoist[i].name) == n && !memcmp(hoist[i].name, s, n)) h = i;
            }
            if (k >= 0) cg_str(o, TO[k]); else { if (h >= 0) cg_str(o, "h_"); cg_add(o, s, n); }
            s = q;
        } else { cg_add(o, s, 1); s++; }
    }
}
static char *cg_half(const char *vs, const char *fs)
{
    cg_var vv[12], fv[12], un[24]; int nvv = 0, nfv = 0, nun = 0, fv3 = 0, c, f3;
    char nm[24];
    /* the vertex shader's varyings, in order */
    for (const char *q = vs; (q = strstr(q, "varying ")); q += 8) {
        if (q != vs && q[-1] != '\n') continue;
        if (!cg_decl(q + 8, nm, sizeof nm, &c, NULL) || !c || nvv == 12) return NULL;
        snprintf(vv[nvv].name, sizeof vv[nvv].name, "%s", nm); vv[nvv].comps = c; nvv++;
    }
    const char *mn = strstr(fs, "void main()");
    if (!mn || !nvv) return NULL;
    if (strstr(fs, "floor(")) return NULL;             /* (a pattern worked out from a varying - the city's windows: float's work) */
    const char *body = strchr(mn, '{'), *bend = strrchr(fs, '}');
    if (!body || !bend || bend <= body) return NULL;
    body++;
    cg_buf o = { 0 }, help = { 0 };
    /* the lines before main: declarations, and whatever else (a helper function) as code */
    for (const char *l = fs; l < mn; ) {
        const char *e = memchr(l, '\n', (size_t)(mn - l)); if (!e) e = mn;
        if (*l == '#') {                               /* (only FS_SCROLL's own: its vLit carries coordinates) */
            if (strncmp(l, "#ifdef vec4", 11) && strncmp(l, "#define FV3", 11) && strncmp(l, "#else", 5) && strncmp(l, "#endif", 6)) goto no;
        } else if (!strncmp(l, "varying ", 8)) {
            if (!cg_decl(l + 8, nm, sizeof nm, &c, &f3) || !c || nfv == 12) goto no;
            if (f3) fv3 = 1;
            snprintf(fv[nfv].name, sizeof fv[nfv].name, "%s", nm); fv[nfv].comps = c;
            fv[nfv].coord = f3 || cg_in(CG_COORD, nm, strlen(nm)); nfv++;
        } else if (!strncmp(l, "uniform ", 8)) {
            if (!strncmp(l + 8, "sampler2D ", 10)) cg_add(&o, l, (size_t)(e - l) + 1);
            else {
                if (!cg_decl(l + 8, nm, sizeof nm, &c, NULL) || !c || nun == 24) goto no;
                char d[64]; snprintf(d, sizeof d, "uniform float%.0d %s;\n", c > 1 ? c : 0, nm); cg_str(&o, d);
                snprintf(un[nun].name, sizeof un[nun].name, "%s", nm); un[nun].comps = c; un[nun].coord = 0; nun++;
            }
        } else cg_add(&help, l, (size_t)(e - l) + (e < mn));
        l = e < mn ? e + 1 : mn;
    }
    (void)fv3;
    /* every texture read: at a coordinate varying as it comes (its first two components) */
    for (const char *q = fs; (q = strstr(q, "texture2D(")); q += 10) {
        const char *a = strchr(q, ','); if (!a) goto no;
        a++; while (*a == ' ') a++;
        const char *z = a; while (cg_idc(*z)) z++;
        int ok = 0;
        for (int i = 0; i < nfv; i++) if (strlen(fv[i].name) == (size_t)(z - a) && !memcmp(fv[i].name, a, (size_t)(z - a)) && fv[i].coord)
            ok = fv[i].comps == 2 ? *z == ')' : !strncmp(z, ".xy)", 4);
        if (!ok) goto no;
    }
    /* the uniforms with a half copy: the vec3 / vec4 ones the code names, but the ones to keep float */
    cg_var hoist[24]; int nh = 0;
    for (int i = 0; i < nun; i++) {
        if (un[i].comps < 3 || cg_in(CG_KEEP, un[i].name, strlen(un[i].name))) continue;
        const size_t n = strlen(un[i].name); int used = 0;
        for (const char *q = help.p ? help.p : ""; !used && (q = strstr(q, un[i].name)); q += n) used = !cg_idc(q[n]) && (q == help.p || !cg_idc(q[-1]));
        for (const char *q = body; !used && (q = strstr(q, un[i].name)) && q < bend; q += n) used = !cg_idc(q[n]) && !cg_idc(q[-1]);
        if (used) hoist[nh++] = un[i];
    }
    if (help.n) {
        /* (a helper function that names such a uniform - none does - or a varying - the cinematics' effect does, by
         * the distance: main's copies and inputs are not a helper's to see, so those stay GLSL) */
        for (int i = 0; i < nh + nfv; i++) { const char *w = i < nh ? hoist[i].name : fv[i - nh].name; const size_t n = strlen(w);
            for (const char *q = help.p; (q = strstr(q, w)); q += n) if (!cg_idc(q[n]) && (q == help.p || !cg_idc(q[-1]))) goto no; }
    }
    cg_str(&o, "inline half4 texH(sampler2D s, float2 uv) { return half4(tex2D(s, uv)); }\n");   /* (as vitaGL words its own such helpers) */
    if (help.n) cg_code(&o, help.p, help.p + help.n, NULL, 0);
    cg_str(&o, "void main(");
    for (int i = 0; i < nfv; i++) {
        int at = -1; for (int k = 0; k < nvv; k++) if (!strcmp(vv[k].name, fv[i].name)) at = k;
        if (at < 0 || vv[at].comps != fv[i].comps) goto no;
        char d[96]; snprintf(d, sizeof d, "%s%.0d %s : TEXCOORD%d, ", fv[i].coord ? "float" : "half", fv[i].comps > 1 ? fv[i].comps : 0, fv[i].name, at);
        cg_str(&o, d);
    }
    cg_str(&o, "out half4 oCol : COLOR)\n{\n");
    for (int i = 0; i < nh; i++) { char d[96]; snprintf(d, sizeof d, "  half%d h_%s = half%d(%s);\n", hoist[i].comps, hoist[i].name, hoist[i].comps, hoist[i].name); cg_str(&o, d); }
    cg_code(&o, body, bend, hoist, nh);
    cg_str(&o, "}\n");
    free(help.p);
    if (o.bad) { free(o.p); return NULL; }
    return o.p;
no:
    free(help.p); free(o.p);
    return NULL;
}
int gfx_half_all, gfx_half_n, gfx_half_glsl;            /* the switch file half_all; programs built that way, and left as GLSL */
int gfx_half_trial, gfx_half_built;                    /* set before gfx_init: build them; which were built (GFX_HALF_ bits) */
/* the final pass in half precision, for the game itself (not only the trial): the Vita's bench at MountainTop and
 * warp_speed (1.0.1-dev30, the switch file half_test) had it at 2.02 ms against 2.42 and 3.31 against 4.25, and its
 * picture at most 1 in 255 off the float one's in any pixel. 0 (the switch file full_final): the float one */
int gfx_half_final = 1;
static int half_keep;                                  /* (GFX_HALF_ bits the game uses whatever the bench asks) */
unsigned gfx_shader_opts;                              /* the switch file shader_opts' settings as one number (0: vitaGL's own) */
static int build_cg;                                   /* build1: the fragment shader is Cg (1), or neither built yet */
static int build(prog_t *o, const char *vsrc, const char *fsrc, const char *name, int post)
{
    if (o >= P && o < P + P_COUNT) { SRC_VS[o - P] = vsrc; SRC_FS[o - P] = fsrc; SRC_NAME[o - P] = name; }
#ifdef NR_PC
    if (getenv("SHDUMP")) {                             /* (SHDUMP=dir: every program's sources as the Vita gets them, for tools/pc/vglsl) */
        char pth[300]; FILE *f;
        snprintf(pth, sizeof pth, "%s/%s.vs", getenv("SHDUMP"), name); if ((f = fopen(pth, "w"))) { fputs(vsrc, f); fclose(f); }
        snprintf(pth, sizeof pth, "%s/%s.fs", getenv("SHDUMP"), name); if ((f = fopen(pth, "w"))) { fputs(fsrc, f); fclose(f); }
        char *cg = cg_half(vsrc, fsrc);                  /* (and its fragment shader as Cg in half precision, where it can be) */
        snprintf(pth, sizeof pth, "%s/%s.%s", getenv("SHDUMP"), name, cg ? "cg" : "nocg"); if ((f = fopen(pth, "w"))) { fputs(cg ? cg : "", f); fclose(f); }
        free(cg);
    }
    if (getenv("CGFS")) {                               /* (CGFS=dir: drawn with name.gles there, cgc's GLSL of that Cg: tools/pc/cgcheck.sh) */
        char pth[300]; snprintf(pth, sizeof pth, "%s/%s.gles", getenv("CGFS"), name);
        FILE *f = fopen(pth, "rb");
        if (f) {
            static char gl[32768]; size_t n = fread(gl, 1, sizeof gl - 1, f); gl[n] = 0; fclose(f);
            /* the vertex shader's varyings as cgc names a Cg shader's inputs: TEX0.. in their order, a vec4 each */
            cg_buf v = { 0 }; int k = 0; char nm[24]; int c;
            for (const char *q = vsrc; (q = strstr(q, "varying ")); q += 8) {
                if ((q != vsrc && q[-1] != '\n') || !cg_decl(q + 8, nm, sizeof nm, &c, NULL) || !c) continue;
                char d[120]; snprintf(d, sizeof d, "varying vec4 TEX%d;\n#define %s TEX%d%s\n", k, nm, k, c == 1 ? ".x" : c == 2 ? ".xy" : c == 3 ? ".xyz" : ""); cg_str(&v, d); k++;
            }
            for (const char *l = vsrc; *l; ) {
                const char *e = strchr(l, '\n'); size_t ln = e ? (size_t)(e - l) + 1 : strlen(l);
                if (strncmp(l, "varying ", 8)) cg_add(&v, l, ln);
                l += ln;
            }
            int r = v.p && !v.bad ? build1(o, v.p, gl, name, post) : 0;
            free(v.p);
            if (r) { gfx_half_n++; return r; }
            logm("  %s: cgc's GLSL of its Cg did not build", name);
        } else gfx_half_glsl++;
    }
#else
    if (gfx_half_all && !build_cg) {
        char *cg = cg_half(vsrc, fsrc);
        if (cg) {
            logm("  %s: its fragment shader in Cg, half precision", name);   /* (which one, should the Vita stop on it) */
            build_cg = 1;
            int r = build1(o, vsrc, cg, name, post);
            build_cg = 0;
            free(cg);
            if (r) { gfx_half_n++; return r; }
            logm("  %s: built as GLSL instead", name);
        }
        gfx_half_glsl++;
    }
#endif
    return build1(o, vsrc, fsrc, name, post);
}
static int build1(prog_t *o, const char *vsrc, const char *fsrc, const char *name, int post)
{
    GLint linked = 0;
    if (gfx_load_tick) gfx_load_tick();                /* (the loading screen, while they compile) */
#ifndef NR_PC
    char path[160];
    const uint32_t hash = src_hash(vsrc, fsrc, name);
    snprintf(path, sizeof path, "ux0:data/nitronic/cache/%08x.prg", (unsigned)hash);
    if (bank_n) {
        GLenum fmt = 0; GLsizei len = 0;
        const void *bin = bank_find(hash, &fmt, &len);
        if (bin) {
            o->prog = glCreateProgram();
            glProgramBinary(o->prog, fmt, bin, len);
            glGetProgramiv(o->prog, GL_LINK_STATUS, &linked);
            if (!linked) { glDeleteProgram(o->prog); o->prog = 0; logm("  %s: its built-in binary did not link", name); }
        }
        if (linked) bank_loaded++;
        else { bank_missed++; logm("  %s: not among the built-in programs (%08x)", name, (unsigned)hash); }
    }
    FILE *f = linked ? NULL : fopen(path, "rb");
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
    if (linked) used_note(hash, name);
#endif
    if (!linked) {
#ifndef NR_PC
        GLuint vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(build_cg ? GL_CG_FRAGMENT_SHADER_EXT : GL_FRAGMENT_SHADER);
        /* (the trial's Cg: compiled here and now - the fragment shader first, as vitaGL wants a Cg one done before the link) */
        if (build_cg && !compile(fs, fsrc, name, 1)) { logm("  program %s: the Vita's compiler turned its fragment shader down", name); return 0; }
        if (!compile(vs, vsrc, name, 0) || (!build_cg && !compile(fs, fsrc, name, 1))) { logm("  program %s failed", name); return 0; }
#else
        GLuint vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER);
        if (!compile(vs, vsrc, name, 0) || !compile(fs, fsrc, name, 1)) { logm("  program %s failed", name); return 0; }
#endif
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
            if (w) { fwrite(&fmt, 4, 1, w); fwrite(&len, 4, 1, w); fwrite(bin, 1, len, w); fclose(w); used_note(hash, name); }
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
    o->uLK = glGetUniformLocation(o->prog, "uLK"); o->uLR = glGetUniformLocation(o->prog, "uLR");
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
    o->uMap = glGetUniformLocation(o->prog, "uMap"); o->uMapR = glGetUniformLocation(o->prog, "uMapR"); o->uTap = glGetUniformLocation(o->prog, "uTap");
    o->uFx = glGetUniformLocation(o->prog, "uFx");
    o->uSc = glGetUniformLocation(o->prog, "uSc");
    o->uOff = glGetUniformLocation(o->prog, "uOff");
    o->uScv = glGetUniformLocation(o->prog, "uScv"); o->uOffv = glGetUniformLocation(o->prog, "uOffv");
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
    o->uPN = glGetUniformLocation(o->prog, "uPN"); o->uPR = glGetUniformLocation(o->prog, "uPR"); o->uPU = glGetUniformLocation(o->prog, "uPU"); o->uPK = glGetUniformLocation(o->prog, "uPK");
    o->uRB = glGetUniformLocation(o->prog, "uRB"); o->uRB2 = glGetUniformLocation(o->prog, "uRB2"); o->uRB3 = glGetUniformLocation(o->prog, "uRB3"); o->uRBE = glGetUniformLocation(o->prog, "uRBE");
    static const char *samplers[] = { "uDiff", "uEmit", "uRefl", "uTex", "uB1", "uB2", "uB3", "uTri", "uPal" };
    static const int units[] = { 0, 1, 2, 0, 1, 2, 3, 3, 1 };
    for (int i = 0; i < 9; i++) { GLint s = glGetUniformLocation(o->prog, samplers[i]); if (s >= 0) glUniform1i(s, units[i]); }
    return 1;
}
static int make_programs(void)
{
    logm("building GLSL shaders");
    int ok = build(&P[P_WORLD], VS_WORLD_NR, FS_LIT, "world", 0)
          && build(&P[P_WREF], VS_WORLD, FS_REF, "world-reflect", 0)
          && build(&P[P_WTV], VS_WTV, FS_TV, "world-screen", 0)
          && build(&P[P_WADD], VS_WORLD_NR, FS_ADD0, "world-additive", 0)
          && build(&P[P_WADDG], VS_WORLD_NR, FS_ADD, "world-additive-glass", 0)
          && build(&P[P_WSCROLL], VS_WORLD_SCROLL, FS_SCROLL, "world-scrolling", 0)
          && build(&P[P_WGLASER], VS_GLASER, FS_GLASER, "ground-laser", 0)
          && build(&P[P_OBJSCROLL], VS_OBJ_SCROLL, FS_SCROLL, "object-scrolling", 0)
          && build(&P[P_SKY], VS_SKY, FS_SKY, "sky", 0)
          && build(&P[P_OBJ], VS_OBJ, FS_REF, "object", 0)
          && build(&P[P_OBJADD], VS_OBJ_ADD, FS_ADD0, "object-additive", 0)
          && build(&P[P_OBJADDG], VS_OBJ_ADD, FS_ADD, "object-additive-glass", 0)
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
          && build(&P[P_TRACKDARK], VS_TRACK, FS_TRACKDARK, "tyre-tracks-dark", 0)
          && build(&P[P_PART], VS_PART, FS_PART, "particles", 0);
    if (ok && !build(&P[P_PART1], VS_PART, FS_PART1, "particles-one-slice", 0)) P[P_PART1].prog = 0;   /* (optional: P_PART does it all) */
    if (ok && !build(&P[P_SPRITE], VS_WORLD_NR, FS_SPRITE, "sprites", 0)) P[P_SPRITE].prog = 0;          /* (optional: P_WADD) */
    if (!ok) return 0;
    logm("GLSL shaders ready (%d built in, %d from the cache, %d compiled)", bank_loaded, cache_loaded, cache_built);
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
        && build(&P[P_COMP_SPEED], VS_POST_MIX, FS_COMP_SPEED, "final-speed", 1)
        && build(&P[P_DOWN4], VS_TAP4, FS_DOWN4, "downscale4", 1)
        && build(&P[P_COPY4], VS_TAP4, FS_COPY4, "quarter-copy", 1)
        && build(&P[P_WIDE9], VS_TAP9W, FS_WIDE9, "wide-glow9", 1)
        && build(&P[P_BLUR9], VS_TAP9B, FS_BLUR9, "blur9", 1)
        && build(&P[P_RADIAL2], VS_RADIAL2, FS_RADIAL, "radial-blur2", 1)
        && build(&P[P_COMP2], VS_POST_EDGE2, FS_COMP, "final2", 1)
        && build(&P[P_COMP_SPEED2], VS_POST_MIX2, FS_COMP_SPEED2, "final-speed2", 1);
}

/* the half-precision trial's programs (see above), and the bench's switch between them and the game's own */
enum { PH_SCROLL, PH_SCROLLH, PH_ADD, PH_ADDH, PH_PART1, PH_PART1H, PH_COMP, PH_COMPH, PH_COMPS, PH_COMPSH, PH_N };
static const int PH_OF[PH_N / 2] = { P_WSCROLL, P_WADD, P_PART1, P_COMP2, P_COMP_SPEED2 };   /* (the game's program each pair stands in for) */
static prog_t PH[PH_N], PH_OWN[PH_N / 2]; static int ph_in;
#ifndef NR_PC
static void make_half_programs(void)
{
    if (!gfx_half_trial && !gfx_half_final) return;
    static const struct { const char *fs, *name; } T[PH_N] = {
        { CG_SCROLL(CG_READ), "half-scrolling" }, { CG_SCROLL(CG_READH), "half-scrolling-reads" },
        { CG_ADD0(CG_READ), "half-additive" }, { CG_ADD0(CG_READH), "half-additive-reads" },
        { CG_PART1(CG_READ), "half-particles" }, { CG_PART1(CG_READH), "half-particles-reads" },
        { CG_COMP(CG_READ), "half-final" }, { CG_COMP(CG_READH), "half-final-reads" },
        { CG_COMP_SPEED(CG_READ), "half-final-speed" }, { CG_COMP_SPEED(CG_READH), "half-final-speed-reads" } };
    for (int k = 0; k < PH_N; k++) {
        const int own = PH_OF[k / 2];
        if (!gfx_half_trial && own != P_COMP2 && own != P_COMP_SPEED2) continue;   /* (the game's own: the final pass only) */
        if (!P[own].prog || !SRC_VS[own]) continue;
        logm("  the half-precision trial: %s", T[k].name);   /* (which one, should the Vita's compiler stop on it) */
        build_cg = 1;
        if (build1(&PH[k], SRC_VS[own], T[k].fs, T[k].name, own >= P_DOWN && own <= P_COMP_SPEED2)) gfx_half_built |= 1 << k; else PH[k].prog = 0;
        build_cg = 0;
    }
    logm("the half-precision trial: built %s%s%s%s%s(reads asked for in half: %s%s%s%s%s)",
         gfx_half_built & 1 ? "scrolling glow, " : "", gfx_half_built & 4 ? "plain glow, " : "", gfx_half_built & 16 ? "particles, " : "",
         gfx_half_built & 64 ? "final pass, " : "", gfx_half_built & 256 ? "final pass with speed blur " : "",
         gfx_half_built & 2 ? "scrolling glow " : "", gfx_half_built & 8 ? "plain glow " : "", gfx_half_built & 32 ? "particles " : "",
         gfx_half_built & 128 ? "final pass " : "", gfx_half_built & 512 ? "final pass with speed blur" : "");
    /* (both final passes, or neither: the frame would change between the two as the speed blur comes and goes) */
    half_keep = gfx_half_final && (gfx_half_built & (1 << PH_COMP | 1 << PH_COMPH)) && (gfx_half_built & (1 << PH_COMPS | 1 << PH_COMPSH)) ? GFX_HALF_FINAL : 0;
    logm("the final pass: %s", half_keep ? "half precision" : gfx_half_final ? "float (its half-precision programs did not build)" : "float (the switch file full_final)");
    gfx_half_use(0);
}
#endif
/* the bench: which of the game's programs the trial's stand in for from now on. what: GFX_HALF_ bits (0: none - the
 * game's own again) */
void gfx_half_use(int what)
{
    if (ph_in) for (int i = 0; i < PH_N / 2; i++) { P[PH_OF[i]] = PH_OWN[i]; P[PH_OF[i]].stamp = -1; }
    ph_in = 0;
    what |= half_keep;
    if (!what || !gfx_half_built) return;
    for (int i = 0; i < PH_N / 2; i++) PH_OWN[i] = P[PH_OF[i]];
    ph_in = 1;
    static const int ask[PH_N / 2] = { GFX_HALF_SCROLL | GFX_HALF_SCROLL_READS, GFX_HALF_GLOW, GFX_HALF_GLOW, GFX_HALF_FINAL, GFX_HALF_FINAL };
    for (int i = 0; i < PH_N / 2; i++) {
        if (!(what & ask[i])) continue;
        const int a = 2 * i, b = 2 * i + 1;
        /* (the scrolling glow has a way for each; the others take the reads in half where those were built) */
        int k = (gfx_half_built >> b & 1) && (i || (what & GFX_HALF_SCROLL_READS)) ? b : a;
        if (!i && k == a && !(what & GFX_HALF_SCROLL)) continue;
        if (!(gfx_half_built >> k & 1)) continue;
        P[PH_OF[i]] = PH[k]; P[PH_OF[i]].stamp = -1;
    }
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
/* the new passes' targets (see VS_TAP4): quarter (bright parts 0..239, their edge 240..247, the copy's edge 248..279,
 * the copy 280..519), wide glow (a quarter of that each way: 0..59, edge 60..61), blur (glow 0..239, edge 240..243,
 * the speed blur's edge 244..247, speed blur 248..487) */
static target_t T_Q, T_W, T_BB;
enum { QW = 240, QH = 136, TQW = 520, QC = 280, TBW = 488, BR = 248 };
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

static prog_t PI[P_COUNT];
/* the object programs for objects lit by 0, 1 or 2 lights (LIGHTS0..2), and their instanced versions. (Three lights
 * in three lanes came out a shade different on the PC's GPU, in a few pixels: those keep LIGHTS4) */
#define NLV 3
static prog_t PLV[NLV][P_COUNT], PILV[NLV][P_COUNT];
static const char *const LIGHTS_N[NLV] = { LIGHTS0, LIGHTS1, LIGHTS2 };
static const int LIT_P[] = { P_OBJ, P_OBJPULSE, P_OBJANIM, P_OBJLIT };
static int lv_built;
static char *light_variant(const char *vs, int n) { return vs && strstr(vs, LIGHTS4) ? str_sub(vs, LIGHTS4, LIGHTS_N[n], 0) : NULL; }
static void make_light_programs(void)
{
    for (unsigned k = 0; k < sizeof LIT_P / sizeof LIT_P[0]; k++) {
        int i = LIT_P[k]; P[i].lit = 5;
        for (int n = 0; n < NLV; n++) {
            char *vs = light_variant(SRC_VS[i], n); char nm[48];
            snprintf(nm, sizeof nm, "%s-%dlight%s", SRC_NAME[i] ? SRC_NAME[i] : "?", n, n == 1 ? "" : "s");
            if (vs && build(&PLV[n][i], vs, SRC_FS[i], nm, 0)) { PLV[n][i].lit = n + 1; lv_built++; }
            free(vs);
        }
    }
}
#ifdef NR_PC
/* ---- (the PC test machine, FRAGAT=<race time>: one frame's draws into the scene counted in the pixels each one has
 * the GPU shade, by what drew it. The stencil buffer counts: a solid draw writes its number where it is still seen at
 * the end of the solid part (the Vita's GPU shades only those: it sorts solid surfaces per tile first); a blended draw
 * is counted alone, every pixel it passes the depth test at) */
int gfx_fd_on; static const char *fd_phase = ""; static char fd_lab[96]; static int fd_dump_post; static long fd_small[3], lt_tris[5]; static int lt_cur = -1;   /* (lit triangles by their lights) */
char gfx_fd_where[96];
typedef struct { char key[160]; int draws, id; long tris; double px; } fdrec;
static fdrec FDR[700]; static int nfdr, fd_ids, fd_pending;
static uint8_t fd_buf[960 * 544];
static const char *prog_name_of(GLint id)
{
    for (int i = 0; i < P_COUNT; i++) {
        if (P[i].prog && (GLint)P[i].prog == id) return SRC_NAME[i] ? SRC_NAME[i] : "?";
        if (PI[i].prog && (GLint)PI[i].prog == id) return SRC_NAME[i] ? SRC_NAME[i] : "?i";
        if (PRB[i].prog && (GLint)PRB[i].prog == id) return SRC_NAME[i] ? SRC_NAME[i] : "?c";
        for (int n = 0; n < NLV; n++) if ((PLV[n][i].prog && (GLint)PLV[n][i].prog == id) || (PILV[n][i].prog && (GLint)PILV[n][i].prog == id))
            return SRC_NAME[i] ? SRC_NAME[i] : "?l";
    }
    return "other";
}
static int fd_scene(void) { GLint fb = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb); return fb && (GLuint)fb == T_SCENE.fbo; }
static void fd_read(void) { glReadPixels(0, 0, SW, SH, 0x1901 /* GL_STENCIL_INDEX_OES */, GL_UNSIGNED_BYTE, fd_buf); }
static void fd_flush_solid(void)
{
    if (!fd_pending) return;
    fd_pending = 0; fd_read();
    static long cnt[256]; memset(cnt, 0, sizeof cnt);
    for (int i = 0; i < SW * SH; i++) cnt[fd_buf[i]]++;
    for (int k = 0; k < nfdr; k++) if (FDR[k].id) FDR[k].px += cnt[FDR[k].id];
    const char *pp = getenv("FDPIX");                 /* (FDPIX=x,y: which solid draw is seen there, picture coordinates) */
    if (pp) { int x = atoi(pp), y = SH - 1 - atoi(strchr(pp, ',') ? strchr(pp, ',') + 1 : "0"); int id = fd_buf[y * SW + x];
        for (int k = 0; k < nfdr; k++) if (FDR[k].id == id) fprintf(stderr, "FRAGS pixel %s: %s\n", pp, FDR[k].key); }
}
static int fd_pre(void)
{
    if (!gfx_fd_on || !fd_scene()) return -1;
    GLint prog = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    char key[160]; snprintf(key, sizeof key, "%-8s %-44s %s", fd_phase, fd_lab, prog_name_of(prog));
    int k = 0; while (k < nfdr && strcmp(FDR[k].key, key)) k++;
    if (k == nfdr) { if (nfdr == 700) return -1; memset(&FDR[k], 0, sizeof FDR[k]); snprintf(FDR[k].key, sizeof FDR[k].key, "%s", key); nfdr++; }
    int blended = glIsEnabled(GL_BLEND);
    glEnable(GL_STENCIL_TEST); glStencilMask(0xff);
    if (blended) {
        fd_flush_solid();
        glClearStencil(0); glClear(GL_STENCIL_BUFFER_BIT);
        glStencilFunc(GL_ALWAYS, 0, 0xff); glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
    } else {
        if (!FDR[k].id) FDR[k].id = fd_ids < 255 ? ++fd_ids : 255;
        glStencilFunc(GL_ALWAYS, FDR[k].id, 0xff); glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        fd_pending = 1;
    }
    return k | blended << 16;
}
static void fd_post(int r, long tris)
{
    if (r < 0) return;
    fdrec *f = &FDR[r & 0xffff];
    f->draws++; f->tris += tris;
    if (r >> 16) { fd_read(); long s = 0; for (int i = 0; i < SW * SH; i++) s += fd_buf[i]; f->px += s; }
    glDisable(GL_STENCIL_TEST);
}
static void fd_glDrawElements(GLenum m, GLsizei n, GLenum t, const void *ix) { int r = fd_pre(); glDrawElements(m, n, t, ix); fd_post(r, n / 3); }
static void fd_glDrawArrays(GLenum m, GLint f, GLsizei n) { int r = fd_pre(); glDrawArrays(m, f, n); fd_post(r, m == GL_TRIANGLES ? n / 3 : n - 2); }
static void fd_glDrawElementsInstanced(GLenum m, GLsizei n, GLenum t, const void *ix, GLsizei k) { int r = fd_pre(); glDrawElementsInstanced(m, n, t, ix, k); fd_post(r, (long)n / 3 * k); }
static int fd_cmp(const void *a, const void *b) { double x = ((const fdrec *)a)->px, y = ((const fdrec *)b)->px; return x < y ? 1 : x > y ? -1 : 0; }
static int fd_cmp_tris(const void *a, const void *b) { long x = ((const fdrec *)a)->tris, y = ((const fdrec *)b)->tris; return x < y ? 1 : x > y ? -1 : 0; }
static void fd_report(void)
{
    fd_flush_solid();
    if (gfx_fd_on == 2) {   /* (one line: solid / blended pixels, triangles, draws by part) */
        double sol = 0, bl = 0; long st = 0, bt = 0, ot = 0, lt = 0, gt = 0, bo = 0; int dr = 0;
        for (int k = 0; k < nfdr; k++) {
            const fdrec *f = &FDR[k]; dr += f->draws;
            int glow = !strncmp(f->key, "lvlglow", 7) || !strncmp(f->key, "objglow", 7) || !strncmp(f->key, "sprites", 7) || !strncmp(f->key, "effects", 7) || !strncmp(f->key, "decals", 6);
            if (glow) { bl += f->px; bt += f->tris; } else { sol += f->px; st += f->tris; }
            if (!strncmp(f->key, "objects", 7)) ot += f->tris;
            if (!strncmp(f->key, "level", 5)) lt += f->tris;
            if (!strncmp(f->key, "objglow", 7)) { gt += f->tris; bo += (long)f->px; }
        }
        fprintf(stderr, "FRAGLINE %s | solid %.0f px %ld tris (objects %ld, level %ld) | blended %.0f px %ld tris (objects %ld tris %ld px) | draws %d | objs %d | object tris by size %ld %ld %ld\n",
                gfx_fd_where, sol, st, ot, lt, bl, bt, gt, bo, dr, gfx_objs_drawn, fd_small[0], fd_small[1], fd_small[2]);
        fprintf(stderr, "LIGHTLINE %s | object tris by lights %ld %ld %ld %ld %ld\n", gfx_fd_where, lt_tris[0], lt_tris[1], lt_tris[2], lt_tris[3], lt_tris[4]);
        nfdr = 0; fd_ids = 0; gfx_fd_on = 0;
        return;
    }
    qsort(FDR, nfdr, sizeof *FDR, fd_cmp);
    double tot = 0; for (int k = 0; k < nfdr; k++) tot += FDR[k].px;
    fprintf(stderr, "FRAGS a frame: %.0f pixels shaded (%.2f screens); by what drew them:\n", tot, tot / (SW * SH));
    static const char *ph[] = { "sky", "level", "objects", "decals", "lvlglow", "objglow", "sprites", "effects" };
    for (int q = 0; q < 8; q++) {
        double s = 0; int d = 0; long t = 0; size_t L = strlen(ph[q]);
        for (int k = 0; k < nfdr; k++) if (!strncmp(FDR[k].key, ph[q], L) && FDR[k].key[L] == ' ') { s += FDR[k].px; d += FDR[k].draws; t += FDR[k].tris; }
        fprintf(stderr, "FRAGS  %-8s %8.0f px (%5.2f screens) %4d draws %7ld tris\n", ph[q], s, s / (SW * SH), d, t);
    }
    for (int k = 0; k < nfdr && k < 60; k++)
        fprintf(stderr, "FRAGS   %8.0f px %5.2f scr %3d draws %6ld tris  %s\n", FDR[k].px, FDR[k].px / (SW * SH), FDR[k].draws, FDR[k].tris, FDR[k].key);
    qsort(FDR, nfdr, sizeof *FDR, fd_cmp_tris);
    for (int k = 0; k < nfdr && k < 30; k++)
        fprintf(stderr, "FRAGTRIS %6ld tris %3d draws %8.0f px  %s\n", FDR[k].tris, FDR[k].draws, FDR[k].px, FDR[k].key);
    nfdr = 0; fd_ids = 0; gfx_fd_on = 0;
    if (getenv("FDPOST")) fd_dump_post = 1;
}
static void fd_save(const target_t *t, const char *nm)   /* (FDPOST: the screen passes' targets as pictures) */
{
    static uint8_t px[1024 * 544 * 4]; char p[300];
    snprintf(p, sizeof p, "%s/post_%s.ppm", getenv("NRPC_OUT") ? getenv("NRPC_OUT") : ".", nm);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo); glReadPixels(0, 0, t->w, t->h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(p, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", t->w, t->h);
    for (int y = t->h - 1; y >= 0; y--) for (int x = 0; x < t->w; x++) fwrite(px + (y * t->w + x) * 4, 1, 3, f);
    fclose(f);
}
#define FD_PHASE(s) (fd_phase = (s))
#define FD_LABEL(...) snprintf(fd_lab, sizeof fd_lab, __VA_ARGS__)
#define glDrawElements fd_glDrawElements
#define glDrawArrays fd_glDrawArrays
#define glDrawElementsInstanced fd_glDrawElementsInstanced
#else
#define FD_PHASE(s) ((void)0)
#define FD_LABEL(...) ((void)0)
#endif

static void *gfx_pring_alloc_bytes(size_t bytes, unsigned *buf);
/* ---- many copies of one small model in one draw call (instancing): the debris of Desaturated Day's buildings,
 * Levels 5 and 6's air mines, the Evil Carwash's crushers. The object programs again, with the model matrix's rows
 * as attributes that step once per copy (aI0..2) instead of uniforms; everything else the copies must share (their
 * lights, colour, glow, material), so the picture is the same as one draw per object */
/* (PI: above) */
static int inst_ok;
int gfx_inst_draws, gfx_inst_objs;
static char *inst_source(const char *vs)
{
    char *a = vs ? str_sub(vs, "uniform vec4 uM0;\nuniform vec4 uM1;\nuniform vec4 uM2;\n",
                           "attribute vec4 aI0;\nattribute vec4 aI1;\nattribute vec4 aI2;\n", 0) : NULL;
    if (!a) return NULL;
    for (int r = 0; r < 3; r++) {                           /* uMr -> aIr everywhere after */
        char from[4] = { 'u', 'M', (char)('0' + r), 0 }, to[4] = { 'a', 'I', (char)('0' + r), 0 };
        char *b;
        while ((b = str_sub(a, from, to, 0))) { free(a); a = b; }
    }
    return a;
}
static int make_inst_programs(void)
{
    static const int L[] = { P_OBJ, P_OBJADD, P_OBJADDG, P_OBJSCROLL, P_OBJPULSE, P_OBJVEINS, P_OBJANIM, P_OBJLIT };
    int built = 0;
    for (unsigned k = 0; k < sizeof L / sizeof L[0]; k++) {
        int i = L[k];
        if (!SRC_VS[i] || !SRC_FS[i]) continue;
        char *a = inst_source(SRC_VS[i]);
        if (!a) continue;
        char nm[48]; snprintf(nm, sizeof nm, "instanced-%s", SRC_NAME[i] ? SRC_NAME[i] : "?");
        if (build(&PI[i], a, SRC_FS[i], nm, 0)) { built++; PI[i].lit = P[i].lit; }
        free(a);
        for (int n = 0; n < NLV && PI[i].prog && P[i].lit; n++) {   /* (and the fewer-lights ones) */
            char *v = light_variant(SRC_VS[i], n), *b = inst_source(v);
            snprintf(nm, sizeof nm, "instanced-%s-%dl", SRC_NAME[i] ? SRC_NAME[i] : "?", n);
            if (b && build(&PILV[n][i], b, SRC_FS[i], nm, 0)) PILV[n][i].lit = n + 1;
            free(v); free(b);
        }
    }
    return built;
}
/* the instances' matrices: a ring in GPU memory written as the frame is drawn, a part of it a frame (four parts: the
 * GPU may still be reading the frames before) */
#define INST_MAX 4096
#define INST_SEGS 4
static float (*inst_ring)[12]; static unsigned inst_buf; static int inst_seg, inst_n;
/* (a ring's GL buffer still pointing at the ring: vitaGL's buffer names are pointers to its small records of each
 * buffer, kept in the C library's heap, the memory's address their first word - and on Old_RedLevel after three levels the
 * particles' one pointed somewhere else, the GPU drawing whatever was there as particles: 100-500 ms frames, while the
 * very same particles from a buffer made then took 1.4 ms. So each draw looks first, and makes the buffer again (logged)
 * when it has to; the old record is left as it is, for it may be another buffer's by now) */
/* (and when: every ring made is listed, and gfx_ring_watch - each frame, and between the steps of loading a level - logs
 * the first place it finds a ring's buffer record pointing elsewhere, once each) */
static struct { const void *mem; unsigned *buf; int said; } RW[8]; static int nrw;
static void ring_watch_add(const void *mem, unsigned *buf) { for (int i = 0; i < nrw; i++) if (RW[i].buf == buf) { RW[i].mem = mem; return; } if (nrw < 8) { RW[nrw].mem = mem; RW[nrw].buf = buf; nrw++; } }
void gfx_ring_watch(const char *where)
{
#ifndef NR_PC
    for (int i = 0; i < nrw; i++) {
        if (RW[i].said || !*RW[i].buf || *(void *const *)(uintptr_t)*RW[i].buf == RW[i].mem) continue;
        const uint32_t *w = (const uint32_t *)(uintptr_t)*RW[i].buf;
        logm("  the GL buffer of GPU ring %d (%08x, for %p) found changed %s: its record %08x %08x %08x %08x", i, *RW[i].buf, RW[i].mem, where, w[0], w[1], w[2], w[3]);
        RW[i].said = 1;
    }
#else
    (void)where;
#endif
}
static void ring_buf_check(const void *mem, unsigned *buf, const char *what)
{
#ifndef NR_PC
    if (*buf && *(void *const *)(uintptr_t)*buf == mem) return;
    static int said;
    if (said++ < 12) {
        const uint32_t *w = *buf ? (const uint32_t *)(uintptr_t)*buf : NULL;   /* (its record now: memory, size, ...) */
        logm("  the GL buffer of %s (%08x) no longer pointed at it (%p, not %p; its record %08x %08x %08x %08x): made again",
             what, *buf, w ? (void *)(uintptr_t)w[0] : NULL, mem, w ? w[0] : 0, w ? w[1] : 0, w ? w[2] : 0, w ? w[3] : 0);
    }
    GLuint b = 0; glGenBuffers(1, &b);
    glBindBuffer(GL_ARRAY_BUFFER, b); vglBufferData(GL_ARRAY_BUFFER, mem); glBindBuffer(GL_ARRAY_BUFFER, 0);
    *buf = b;
#else
    (void)mem; (void)buf; (void)what;
#endif
}
static void inst_attribs(const float (*rows)[12])
{
#ifdef NR_PC
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    const uint8_t *b = (const uint8_t *)rows;
#else
    ring_watch_add(inst_ring, &inst_buf); ring_buf_check(inst_ring, &inst_buf, "the instances' ring");
    glBindBuffer(GL_ARRAY_BUFFER, inst_buf);
    const uint8_t *b = (const uint8_t *)((uintptr_t)((const uint8_t *)rows - (const uint8_t *)inst_ring));
#endif
    for (int a = 0; a < 3; a++) {
        glEnableVertexAttribArray(5 + a);
        glVertexAttribPointer(5 + a, 4, GL_FLOAT, GL_FALSE, 48, b + 16 * a);
        glVertexAttribDivisor(5 + a, 1);
    }
}
static void inst_attribs_off(void)
{
    for (int a = 0; a < 3; a++) { glVertexAttribDivisor(5 + a, 0); glDisableVertexAttribArray(5 + a); }
}
/* does the GPU draw copies where they belong? Two small squares, side by side, into a render target, read back
 * (vitaGL's instancing is used nowhere else in this game: if it fails, it is left off) */
static int inst_selftest(void)
{
    static const char *VS =
        "attribute vec3 aPos;\n" "attribute vec4 aI0;\n" "attribute vec4 aI1;\n" "attribute vec4 aI2;\n"
        "void main() { gl_Position = vec4(aPos.x * 0.2 + aI0.w + aI1.w * 0.0, aPos.y * 0.2 + aI2.w, 0.0, 1.0); }\n";
    static const char *FS = "void main() { gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0); }\n";
    prog_t t; memset(&t, 0, sizeof t);
    if (!build(&t, VS, FS, "instancing-test", 0)) return 0;
    target_t tg; memset(&tg, 0, sizeof tg);
    if (!make_target(&tg, 64, 32, 0)) return 0;
    static uint8_t v[4 * NR_VERTEX_SIZE];
    const float q[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
    for (int i = 0; i < 4; i++) { float p[3] = { q[i][0], q[i][1], 0 }; memcpy(v + i * NR_VERTEX_SIZE, p, 12); }
    static const uint16_t ix[6] = { 0, 1, 2, 0, 2, 3 };
    GLuint vb = 0, ib = 0; glGenBuffers(1, &vb); glGenBuffers(1, &ib);
    glBindBuffer(GL_ARRAY_BUFFER, vb); glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib); glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof ix, ix, GL_STATIC_DRAW);
    float (*rows)[12] = inst_ring;                       /* copy 0 at x -0.5, copy 1 at x +0.5 (both at y 0) */
    memset(rows, 0, 2 * 48);
    rows[0][3] = -0.5f; rows[1][3] = 0.5f;
    glBindFramebuffer(GL_FRAMEBUFFER, tg.fbo); glViewport(0, 0, 64, 32);
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(t.prog);
    for (int i = 1; i < 5; i++) glDisableVertexAttribArray(i);
    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, vb);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, NR_VERTEX_SIZE, (void *)0);
    inst_attribs(rows);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib);
    glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (void *)0, 2);
    inst_attribs_off();
    static uint8_t px[64 * 32 * 4];
    glReadPixels(0, 0, 64, 32, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_FRAMEBUFFER, 0); glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &vb); glDeleteBuffers(1, &ib);
    glDeleteFramebuffers(1, &tg.fbo); glDeleteTextures(1, &tg.tex);
    /* squares of 6x3 pixels round x 16 and x 48 (y 16); nothing at the middle or the edges */
    #define PX(x, y) px[((y) * 64 + (x)) * 4]
    int ok = PX(16, 16) > 200 && PX(48, 16) > 200 && PX(32, 16) < 50 && PX(2, 16) < 50 && PX(62, 16) < 50 && PX(16, 2) < 50;
    logm("instancing test: copies %s (%d %d | %d %d %d %d)", ok ? "where they belong" : "WRONG: drawn one by one", PX(16, 16), PX(48, 16), PX(32, 16), PX(2, 16), PX(62, 16), PX(16, 2));
    #undef PX
    return ok;
}
int gfx_init(void)
{
#ifndef NR_PC
    shark_install_log_cb(shark_logm); shark_set_warnings_level(SHARK_WARN_SILENT);
    bank_open();
#endif
    if (!make_programs()) return -1;
    make_light_programs();
    logm("object programs for fewer lights: %d", lv_built);
    glCullFace(GL_BACK); glFrontFace(GL_CCW);   /* the meshes' front faces are counter-clockwise */
    glDepthFunc(GL_LEQUAL);
    tex_white = solid_texture(255); tex_black = solid_texture(0); tex_glow = glow_texture();
    post_ok = 0;
    if (!GSW.no_post && make_post_programs() && make_target(&T_SCENE, SW, SH, 1)) {
        post_ok = 1;
        for (int i = 0; i < 4 && post_ok; i++)
            post_ok = make_target(&T_B[i], i != 2 ? SW / 4 : SW / 16, i != 2 ? SH / 4 : SH / 16, 0);
        if (post_ok) post_ok = make_target(&T_Q, TQW, QH, 0) && make_target(&T_W, TQW / 4, QH / 4, 0) && make_target(&T_BB, TBW, QH, 0);
        /* (no half-size targets: the speed blur works at a quarter size) */
    }
    logm("screen effects (bloom, speed blur): %s", post_ok ? "on" : "off");
    inst_ok = 0;
    if (!GSW.no_inst) {
        int n = make_inst_programs();
        unsigned b = 0;
        inst_ring = (float (*)[12])gfx_pring_alloc_bytes(INST_MAX * INST_SEGS * 48, &b);
        inst_buf = b;
        inst_ok = n > 0 && inst_ring && inst_selftest();
        logm("instancing: %s (%d programs)", inst_ok ? "on" : "off", n);
    }
#ifndef NR_PC
    make_half_programs();                              /* (last: should the Vita stop on one, all the rest is in the log) */
#else
    if (getenv("SHDUMP")) gfx_redblue_init();          /* (the cinematics' programs dumped too) */
#endif
    if (gfx_half_n || gfx_half_glsl) logm("fragment shaders in Cg with half-precision colours: %d programs (%d as GLSL)", gfx_half_n, gfx_half_glsl);
    logm("programs: %d built in, %d from the cache, %d compiled%s", bank_loaded, cache_loaded, cache_built,
         bank_missed ? " (some not built in: tools/shbank.py makes the bank again)" : "");
    return 0;
}

/* ------------------------------------------------------------------ textures */
static int compressed_mips_ok = 1;
typedef struct { char name[64]; GLuint id; int bytes, refs; } texent;   /* (refs: the loaded levels and models using it) */
static texent texcache[1024]; static int ntexcache; static int texcache_bytes;
float gfx_texture_mb(void) { return texcache_bytes / 1048576.0f; }

static GLuint tex_mipped[1024]; static int ntex_mipped;    /* (the textures with mip levels: the bench's gfx_dbg 256) */
static GLuint bil_tex[64]; static int nbil;                 /* (the ones it has set to no mip blending) */
void gfx_bench_restore(void)
{
    for (int i = 0; i < nbil; i++) { glBindTexture(GL_TEXTURE_2D, bil_tex[i]); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); }
    nbil = 0;
}
uint64_t gfx_tex_ab_us[2]; int gfx_tex_ab_n[2];            /* (textures of more than one level: given one by one, last level second) */
int gfx_texchk_n, gfx_texchk_bad; static uint64_t texchk_us;     /* (the checks' own time: not counted as the upload's) */
#ifndef NR_PC
/* (switch tex_check) a texture's levels as the GPU has them against the file's: each level's blocks, in whatever order
 * the GPU's layout puts them, must be the file's (an order-free sum of the blocks' hashes) - a level given from a
 * misaligned place, as vitaGL can (the GITS:FA port found DXT5 levels spoiled so), would show. Levels under 4 x 4 of
 * more than one block are left out (neither vitaGL nor dxt_give copies those). The test machine: none */
static uint64_t mix64(uint64_t z) { z += 0x9e3779b97f4a7c15ull; z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull; z = (z ^ (z >> 27)) * 0x94d049bb133111ebull; return z ^ (z >> 31); }
static void blocks_hash(const uint8_t *p, uint32_t n, int bs, uint64_t h[2])
{
    h[0] = h[1] = 0;
    for (uint32_t o = 0; o + bs <= n; o += bs) {
        uint64_t a, c = 0; memcpy(&a, p + o, 8); if (bs == 16) memcpy(&c, p + o + 8, 8);
        const uint64_t x = mix64(a ^ mix64(c + 0x632be59bd9b4e019ull));
        h[0] += x; h[1] += mix64(x ^ 0x2545f4914f6cdd1dull);
    }
}
static void tex_check(const char *name, int bs, int levelsn, const uint8_t *const lp[], const uint32_t ls[], const int lw[], const int lh[])
{
    sceGxmTransferFinish();
    const uint64_t c0 = lod_now();
    const uint8_t *base = vglGetTexDataPointer(GL_TEXTURE_2D);
    if (!base) return;
    uint32_t w = 1, h = 1; while (w < (uint32_t)lw[0]) w <<= 1; while (h < (uint32_t)lh[0]) h <<= 1;
    uint32_t off = 0; int bad = 0, first_bad = -1;
    for (int i = 0; i < levelsn; i++) {
        const uint32_t sz = ((w + 3) / 4) * ((h + 3) / 4) * bs;
        if (((lw[i] >= 4 && lh[i] >= 4) || sz == (uint32_t)bs) && sz == ls[i]) {
            uint64_t a[2], g[2]; blocks_hash(lp[i], ls[i], bs, a); blocks_hash(base + off, sz, bs, g);
            if (a[0] != g[0] || a[1] != g[1]) { bad++; if (first_bad < 0) first_bad = i; }
        }
        off += sz; if (w > 1) w >>= 1; if (h > 1) h >>= 1;
    }
    gfx_texchk_n++;
    texchk_us += lod_now() - c0;
    if (bad) { gfx_texchk_bad++; if (gfx_texchk_bad <= 20) logm("  texture %s (%dx%d, DXT%d): %d of %d levels not as in the file (the first: %d)", name, lw[0], lh[0], bs == 8 ? 1 : 5, bad, levelsn, first_bad); }
}
#endif
int gfx_tex_redone, gfx_tex_still_bad;                    /* (textures given again: vitaGL's copy came out wrong; and still wrong after) */
#ifndef NR_PC
/* where vitaGL keeps a compressed texture's levels (its own sizes: whole blocks, the size made a power of two) */
static void dxt_places(int bs, int levelsn, const int lw[], const int lh[], uint32_t off[], uint32_t sz[])
{
    uint32_t w = 1, h = 1, o = 0; while (w < (uint32_t)lw[0]) w <<= 1; while (h < (uint32_t)lh[0]) h <<= 1;
    for (int i = 0; i < levelsn; i++) { sz[i] = ((w + 3) / 4) * ((h + 3) / 4) * bs; off[i] = o; o += sz[i]; if (w > 1) w >>= 1; if (h > 1) h >>= 1; }
}
/* the first block of each level vitaGL copied, read back: the levels not the file's (vitaGL's copy from its data pool
 * at an address 8 past a 16-byte boundary spoils a DXT5 level whole - 1.0.1-dev54 on the Vita: 36 of 567 textures,
 * every level of each, all DXT5; the GITS:FA port found the same) */
static int dxt_bad_levels(int bs, int levelsn, const uint8_t *const lp[], const uint32_t ls[], const int lw[], const int lh[])
{
    const uint8_t *base = vglGetTexDataPointer(GL_TEXTURE_2D);
    if (!base) return 0;
    uint32_t off[16], sz[16]; dxt_places(bs, levelsn, lw, lh, off, sz);
    int bad = 0;
    for (int i = 0; i < levelsn; i++) if (lw[i] >= 4 && lh[i] >= 4 && sz[i] == ls[i] && memcmp(base + off[i], lp[i], (size_t)bs)) bad++;
    return bad;
}
/* vitaGL's data pool moved on by 8 bytes: an 8-byte level (DXT1, 4 x 4) given to a texture of its own */
static void pool_nudge8(void)
{
    static GLuint t; static const uint8_t z[8];
    if (!t) glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_S3TC_DXT1_EXT, 4, 4, 0, 8, z);
    sceGxmTransferFinish();
}
#endif
/* a compressed texture's levels given to vitaGL (the texture bound as id): level 0, the last, then the rest (in_order: one
 * by one). vitaGL makes the texture's memory room for the levels it has so far as each one comes, moving what it holds
 * (reading it back from video memory) when it can't grow it where it is: with the last level second that happens once a
 * texture, not once a level (the GITS:FA port's finding; 1.0.1-dev54 on the Vita: given ~4x faster). The same levels in
 * the same places either way. On the Vita each level's first block is then read back: a level vitaGL's copy spoiled
 * (dxt_bad_levels) has them all given again, the pool moved on 8 bytes first; and the one-block levels under 4 x 4,
 * which vitaGL copies none of (its copy is 0 blocks wide: what video memory held there before was drawn far off), are
 * written here. -> the levels given (0: none) */
int gfx_give_dxt(GLuint id, const char *name, int dxt1, int levelsn, const uint8_t *const lp[], const uint32_t ls[], const int lw[], const int lh[], int in_order, int *bytes)
{
    const GLenum f = dxt1 ? GL_COMPRESSED_RGB_S3TC_DXT1_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
    const int bs = dxt1 ? 8 : 16;
    int given = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        given = 0;
        int got = 0;
        for (int k = 0; k < levelsn; k++) {
            const int i = in_order || k == 0 ? k : k == 1 ? levelsn - 1 : k - 1;
            while (glGetError() != GL_NO_ERROR) {}
            glCompressedTexImage2D(GL_TEXTURE_2D, i, f, lw[i], lh[i], 0, ls[i], lp[i]);
#ifndef NR_PC
            /* vitaGL copies the level with an asynchronous GPU transfer and may move the texture's
             * memory when the next level is added; wait so no copy lands in memory it just freed */
            if (!GSW.no_transfer_wait) sceGxmTransferFinish();
#endif
            GLenum e = glGetError();
            if (e != GL_NO_ERROR) {
                if (i == 0) { logm("texture %s: glCompressedTexImage2D error 0x%x", name, e); return 0; }
                logm("compressed mip levels not accepted (error 0x%x), using level 0 only", e);
                compressed_mips_ok = 0; return 1;
            }
            if (!attempt && bytes) *bytes += ls[i];
            got++;
        }
        given = got;
#ifndef NR_PC
        if (GSW.no_transfer_wait) break;
        int bad = dxt_bad_levels(bs, given, lp, ls, lw, lh);
        static int redo_k;                               /* (switch tex_redo_test: every 5th texture given again, as if spoiled) */
        if (!attempt && !bad && GSW.tex_redo_test && ++redo_k % 5 == 0) bad = -1;
        if (!bad) break;
        if (attempt) { gfx_tex_still_bad++; if (gfx_tex_still_bad <= 10) logm("  texture %s: %d levels still not as in the file after giving it again", name, bad); break; }
        gfx_tex_redone++;
        pool_nudge8();
        glBindTexture(GL_TEXTURE_2D, id);
#else
        break;
#endif
    }
#ifndef NR_PC
    if (!GSW.no_transfer_wait) {                         /* (the one-block levels under 4 x 4) */
        uint8_t *base = vglGetTexDataPointer(GL_TEXTURE_2D);
        uint32_t off[16], sz[16]; dxt_places(bs, given, lw, lh, off, sz);
        for (int i = 1; base && i < given; i++) if ((lw[i] < 4 || lh[i] < 4) && sz[i] == (uint32_t)bs && ls[i] >= (uint32_t)bs) memcpy(base + off[i], lp[i], (size_t)bs);
    }
#endif
    return given;
}
static GLuint load_texture(nr_pak *pak, const char *name, int *bytes)
{
    char p[96]; snprintf(p, sizeof p, "textures/%s.nrt", name);
    uint32_t sz; uint8_t *b = nr_pak_read(pak, p, &sz);
    if (!b) { logm("missing texture %s", name); return 0; }
    nr_texdata t;
    if (nr_tex_parse(b, sz, &t)) { logm("bad texture %s", name); free(b); return 0; }
    const uint64_t up0 = lod_now(), ck0 = texchk_us;
    int in_order = 1, levels_given = 0;
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
        int levelsn = (GSW.no_mips || !compressed_mips_ok) ? 1 : t.nmips;
        if (levelsn > 16) levelsn = 16;
        const uint8_t *lp[16]; uint32_t ls[16]; int lw[16], lh[16];
        { const uint8_t *m = t.data; for (int i = 0; i < levelsn; i++) { ls[i] = nr_tex_mip_size(&t, i, &lw[i], &lh[i]); lp[i] = m; m += ls[i]; } }
        /* the order the levels are given in: level 0, the last, then the rest. vitaGL makes the texture's memory room for
         * the levels it has so far as each one comes, moving what it holds (reading it back from video memory) when it
         * can't grow it where it is: with the last level second that happens once a texture, not once a level (the
         * GITS:FA port's finding). The same levels in the same places either way. (Switch mip_in_order: one by one, as
         * before; tex_ab: every other texture so, the two timed apart for the loading report) */
        static int ab_k;
        in_order = GSW.mip_in_order || (GSW.tex_ab && (ab_k++ & 1));
        levels_given = gfx_give_dxt(id, name, t.fmt == 1, levelsn, lp, ls, lw, lh, in_order, bytes);
        mipped = levels_given > 1;
#ifndef NR_PC
        if (GSW.tex_check && levels_given == levelsn) tex_check(name, t.fmt == 1 ? 8 : 16, levelsn, lp, ls, lw, lh);
#endif
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipped ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    if (mipped && ntex_mipped < 1024) tex_mipped[ntex_mipped++] = id;
    const uint64_t up = lod_now() - up0 - (texchk_us - ck0);
    gfx_tex_us += up; gfx_tex_n++;
    if (levels_given > 1) { gfx_tex_ab_us[!in_order] += up; gfx_tex_ab_n[!in_order]++; }
    free(b);
    return id;
}

GLuint gfx_texture(nr_pak *pak, const char *name)
{
    for (int i = 0; i < ntexcache; i++) if (!strcmp(texcache[i].name, name)) { texcache[i].refs++; return texcache[i].id; }
    int b = 0;
    GLuint id = load_texture(pak, name, &b);
    texcache_bytes += b;
    if (!GSW.no_tex_cache && ntexcache < 1024) {
        snprintf(texcache[ntexcache].name, 64, "%s", name);
        texcache[ntexcache].id = id; texcache[ntexcache].bytes = b; texcache[ntexcache++].refs = 1;
    }
    return id;
}
static void tex_unref(GLuint id)
{
    if (id) for (int i = 0; i < ntexcache; i++) if (texcache[i].id == id) { if (texcache[i].refs > 0) texcache[i].refs--; return; }
}
/* the cache kept every level's textures for the next time: on the Vita they filled its video memory (12.9 MB left
 * by the fourth level of a run, Nov 10), after which textures and buffers go to main memory, slower for the GPU to
 * read. Before a level loads, while less than TEX_KEEP_FREE is free, the textures no loaded level or model uses go,
 * the biggest first (the PC: TEXTRIM=1, always) */
#define TEX_KEEP_FREE (40u << 20)
static void tex_trim(void)
{
#ifdef NR_PC
    if (!getenv("TEXTRIM")) return;
    size_t fr = 0;
#else
    size_t fr = vglMemFree(VGL_MEM_VRAM);
    if (fr >= TEX_KEEP_FREE) return;
#endif
    int n = 0, b = 0;
    while (fr < TEX_KEEP_FREE) {
        int best = -1;
        for (int i = 0; i < ntexcache; i++) if (!texcache[i].refs && (best < 0 || texcache[i].bytes > texcache[best].bytes)) best = i;
        if (best < 0) break;
        GLuint id = texcache[best].id;
        if (id) glDeleteTextures(1, &id);
        for (int i = 0; i < ntex_mipped; i++) if (tex_mipped[i] == id) { tex_mipped[i] = tex_mipped[--ntex_mipped]; break; }
        texcache_bytes -= texcache[best].bytes; fr += (size_t)texcache[best].bytes; b += texcache[best].bytes; n++;
        texcache[best] = texcache[--ntexcache];
    }
    if (n) logm("  textures no longer used: %d let go (%.1f MB)", n, b / 1048576.0);
}

/* ------------------------------------------------------------------ levels */
static uint32_t RUN_TRIS = 128;                           /* (a run of a big level part: triangles) */
static const level_rt *sort_lv;
static int chunk_key(const gchunk *c)
{
    const nr_mat *m = &sort_lv->d.mats[c->mat];
    return (c->kind == NR_KIND_SKY ? 0 : 1 + (m->blend == NR_BLEND_ADD)) * 65536 + m->kind * 4096 + c->mat;
}
static int chunk_order(const void *a, const void *b) { return chunk_key((const gchunk *)a) - chunk_key((const gchunk *)b); }

/* a part's simpler shapes (meshoptimizer's simplifier; a model of several parts keeps their open edges where they
 * are: no cracks between them): about a quarter and a sixteenth of its triangles, however far they stray (they are
 * drawn only where that is under a quarter of a pixel), each kept if it saves at least 40%; their indices (the part's own,
 * its points at pos with NR_VERTEX_SIZE between them) in out[k], how many in n[k], how far they stray in err[k] */
static void make_lods(const float *pos, uint32_t nv, const uint16_t *idx, uint32_t ni, int lock, uint32_t *out[2], uint32_t n[2], float err[2])
{
    n[0] = n[1] = 0; out[0] = out[1] = NULL;
    if (ni < 3 * 96 || nv < 4) return;                    /* (a few triangles: not worth it) */
    uint32_t *ix = malloc(4ull * ni), *l1 = malloc(4ull * ni), *l2 = malloc(4ull * ni);
    if (!ix || !l1 || !l2) { free(ix); free(l1); free(l2); return; }
    for (uint32_t k = 0; k < ni; k++) ix[k] = idx[k];
    float scale = meshopt_simplifyScale(pos, nv, NR_VERTEX_SIZE), e1 = 0, e2 = 0;
    const unsigned opt = (lock ? meshopt_SimplifyLockBorder : 0) | meshopt_SimplifyErrorAbsolute;
    /* the first: the simplifier that keeps the shape's seams (its texture and shading edges); the parts made of flat
     * faces each with points of their own (most of the game's) hardly give way to it: then the one that merges nearby
     * points whatever they carry (only drawn when a few pixels across, where that can't be told) */
    size_t a = meshopt_simplify(l1, ix, ni, pos, nv, NR_VERTEX_SIZE, ni / 4 / 3 * 3, 0.25f * scale, opt, &e1);
    if (a > (size_t)ni * 6 / 10 && !lock) {
        float es = 0;
        size_t s_ = meshopt_simplifySloppy(l1, ix, ni, pos, nv, NR_VERTEX_SIZE, ni / 4 / 3 * 3, 0.25f, &es);
        a = s_; e1 = es * scale;
    }
#ifdef NR_PC
    if (getenv("LODLOG")) fprintf(stderr, "LODLOG %u tris %u verts scale %.2f -> %zu tris (error %.4f)", ni / 3, nv, scale, a / 3, e1);
#endif
    if (a >= 3 && a <= (size_t)ni * 6 / 10) {
        out[0] = l1; n[0] = (uint32_t)a; err[0] = e1;
        size_t b = lock ? meshopt_simplify(l2, l1, a, pos, nv, NR_VERTEX_SIZE, ni / 16 / 3 * 3, 1.0f * scale, opt, &e2) : 0;
        if (!lock) { float es = 0; b = meshopt_simplifySloppy(l2, ix, ni, pos, nv, NR_VERTEX_SIZE, ni / 16 / 3 * 3, 1.0f, &es); e2 = es * scale; }
        else e2 += e1;
#ifdef NR_PC
        if (getenv("LODLOG")) fprintf(stderr, " -> %zu tris (error %.4f)", b / 3, e2);
#endif
        if (b >= 3 && b <= a * 6 / 10) { out[1] = l2; n[1] = (uint32_t)b; err[1] = fmaxf(e2, e1); l2 = NULL; }
        l1 = NULL;
    }
#ifdef NR_PC
    if (getenv("LODLOG")) fprintf(stderr, "\n");
#endif
    free(ix); free(l1); free(l2);
}

int gfx_load_nrl(level_rt *lv, nr_pak *pak, const char *pakname, const char *label)
{
    memset(lv, 0, sizeof *lv);
    snprintf(lv->name, sizeof lv->name, "%s", label);
    uint32_t sz; uint8_t *blob = nr_pak_read(pak, pakname, &sz);
    if (!blob) { logm("level %s not in the pak", lv->name); return 0; }
    int r = nr_level_parse(blob, sz, &lv->d);
    if (r) { logm("level %s: parse error %d", lv->name, r); free(blob); return 0; }
    tex_trim();
    int tb0 = texcache_bytes;
    lv->tex = calloc(lv->d.ntex + 1, sizeof(GLuint));
    for (uint32_t i = 0; i < lv->d.ntex; i++) { lv->tex[i] = gfx_texture(pak, lv->d.texnames[i]); if (gfx_load_tick) gfx_load_tick(); }
    lv->ch = calloc(lv->d.nchunk + 1, sizeof(gchunk));
    int vbytes = 0, tris = 0;
    for (int k = 0; k < 3; k++) { lv->bmin[k] = 1e30f; lv->bmax[k] = -1e30f; }
    float wmn[2] = { 1e30f, 1e30f }, wmx[2] = { -1e30f, -1e30f };
#ifdef NR_PC
    if (getenv("MODELSTAT")) {   /* (the test machine: vertices and triangles of each thing loaded; distinct vertices used) */
        long nv = 0, ni = 0, used = 0;
        for (uint32_t i = 0; i < lv->d.nchunk; i++) {
            nr_chunk *c = &lv->d.chunks[i]; nv += c->nv; ni += c->ni;
            uint8_t *seen = calloc(c->nv + 1, 1);
            for (uint32_t k = 0; k < c->ni; k++) if (c->idx[k] < c->nv && !seen[c->idx[k]]) { seen[c->idx[k]] = 1; used++; }
            free(seen);
        }
        fprintf(stderr, "MODELSTAT %-32s %3u chunks %7ld verts %7ld used %7ld tris (%.2f verts a triangle)\n", label, lv->d.nchunk, nv, used, ni / 3, ni ? used * 3.0 / ni : 0);
    }
#endif
    for (uint32_t i = 0; i < lv->d.nchunk; i++) {
        nr_chunk *c = &lv->d.chunks[i];
        if (c->kind == NR_KIND_WORLD) for (int k = 0; k < 3; k++) {
            lv->bmin[k] = fminf(lv->bmin[k], c->bmin[k]); lv->bmax[k] = fmaxf(lv->bmax[k], c->bmax[k]);
        }
        for (int k = 0; k < 2; k++) { wmn[k] = fminf(wmn[k], c->bmin[k]); wmx[k] = fmaxf(wmx[k], c->bmax[k]); }
    }
    /* the parts in drawing order (sky, solid, glowing; by shader and material), those of one material along a
     * Z-order curve over the ground plan, so the parts in a view tend to lie next to each other in their buffer */
    uint32_t n = lv->d.nchunk;
    uint64_t *ord = malloc(sizeof(uint64_t) * (n + 1));
    sort_lv = lv;
    for (uint32_t i = 0; i < n; i++) {
        nr_chunk *c = &lv->d.chunks[i];
        gchunk g; g.mat = c->mat; g.kind = c->kind;
        uint32_t qx = 0, qy = 0;
        for (int k = 0; k < 2; k++) {
            float ctr = 0.5f * (c->bmin[k] + c->bmax[k]), span = wmx[k] - wmn[k];
            uint32_t q = span > 0 ? (uint32_t)fminf(1023.0f, fmaxf(0.0f, (ctr - wmn[k]) / span * 1023.0f)) : 0;
            if (k == 0) qx = q; else qy = q;
        }
        uint32_t mo = 0;
        for (int b = 0; b < 10; b++) mo |= ((qx >> b) & 1u) << (2 * b) | ((qy >> b) & 1u) << (2 * b + 1);
        ord[i] = (uint64_t)(uint32_t)chunk_key(&g) << 40 | (uint64_t)mo << 16 | i;
    }
    for (uint32_t i = 1; i < n; i++) { uint64_t x = ord[i]; int j = (int)i - 1; while (j >= 0 && ord[j] > x) { ord[j + 1] = ord[j]; j--; } ord[j + 1] = x; }
    lv->gbuf = calloc(2 * (n + 1), sizeof(GLuint));
    for (uint32_t a = 0; a < n; ) {
        /* a group: the run of one material and kind, while its vertices fit 16-bit indices */
        const nr_chunk *c0 = &lv->d.chunks[ord[a] & 0xFFFF];
        uint32_t e = a, nv = 0, ni = 0;
        while (e < n) {
            const nr_chunk *c = &lv->d.chunks[ord[e] & 0xFFFF];
            if (c->mat != c0->mat || c->kind != c0->kind || nv + c->nv > 65536) break;
            nv += c->nv; ni += c->ni; e++;
        }
        if (e == a) e = a + 1;                       /* (a part of its own over 65536 vertices: as it comes) */
        uint8_t *vb = malloc((size_t)nv * NR_VERTEX_SIZE + 1); uint16_t *ib = malloc((size_t)ni * 2 + 2);
        uint32_t vo = 0, io = 0, g = lv->ngrp++, lod_extra = 0;
        for (uint32_t q = a; q < e; q++) {
            const nr_chunk *c = &lv->d.chunks[ord[q] & 0xFFFF]; gchunk *gc = &lv->ch[lv->nch++];
            gc->ni = c->ni; gc->ioff = io; gc->mat = c->mat; gc->kind = c->kind; gc->grp = (int)g;
            memcpy(gc->bmin, c->bmin, 12); memcpy(gc->bmax, c->bmax, 12);
            memcpy(vb + (size_t)vo * NR_VERTEX_SIZE, c->verts, (size_t)c->nv * NR_VERTEX_SIZE);
            for (uint32_t k = 0; k < c->ni; k++) ib[io + k] = (uint16_t)(c->idx[k] + vo);
            gc->lni[0] = gc->lni[1] = 0;
            gc->run0 = gc->nrun = 0;
#ifdef NR_PC
            if (getenv("RUNTRIS")) RUN_TRIS = (uint32_t)atoi(getenv("RUNTRIS"));
#endif
            if (c->kind != NR_KIND_SKY && c->ni >= 3u * 2 * RUN_TRIS) {   /* (big parts: runs; a moving object's in its own space) */
                uint32_t nr = (c->ni / 3 + RUN_TRIS - 1) / RUN_TRIS;
                grun *nrn = realloc(lv->runs, sizeof(grun) * (lv->nruns + nr));
                if (nrn) {
                    lv->runs = nrn; gc->run0 = lv->nruns; gc->nrun = nr;
                    for (uint32_t r = 0; r < nr; r++) {
                        grun *R = &lv->runs[lv->nruns + r];
                        uint32_t a0 = r * RUN_TRIS * 3, a1 = a0 + RUN_TRIS * 3 > c->ni ? c->ni : a0 + RUN_TRIS * 3;
                        R->ioff = gc->ioff + a0; R->ni = a1 - a0;
                        for (int k = 0; k < 3; k++) { R->bmin[k] = 1e30f; R->bmax[k] = -1e30f; }
                        for (uint32_t q = a0; q < a1; q++) {
                            const float *v = (const float *)(vb + (size_t)(vo + c->idx[q]) * NR_VERTEX_SIZE);
                            for (int k = 0; k < 3; k++) { R->bmin[k] = fminf(R->bmin[k], v[k]); R->bmax[k] = fmaxf(R->bmax[k], v[k]); }
                        }
                    }
                    lv->nruns += nr;
#ifdef NR_PC
                    if (getenv("RUNLOG")) {
                        double dsum = 0; float cd = sqrtf((c->bmax[0] - c->bmin[0]) * (c->bmax[0] - c->bmin[0]) + (c->bmax[1] - c->bmin[1]) * (c->bmax[1] - c->bmin[1]) + (c->bmax[2] - c->bmin[2]) * (c->bmax[2] - c->bmin[2]));
                        for (uint32_t r = 0; r < nr; r++) { const grun *R = &lv->runs[gc->run0 + r]; float dx = R->bmax[0] - R->bmin[0], dy = R->bmax[1] - R->bmin[1], dz = R->bmax[2] - R->bmin[2]; dsum += sqrtf(dx * dx + dy * dy + dz * dz); }
                        fprintf(stderr, "RUNLOG %s m%u: %u tris, %u runs, part size %.0f, run size %.0f on average\n", label ? label : "?", c->mat, c->ni / 3, nr, cd, dsum / nr);
                    }
#endif
                }
            }
            if (gfx_nrl_lod && !GSW.no_lod) {             /* its simpler shapes, after the group's own indices */
                uint32_t *L[2], LN[2]; float LE[2];
                uint64_t lt0 = lod_now();
                make_lods((const float *)(vb + (size_t)vo * NR_VERTEX_SIZE), c->nv, c->idx, c->ni, lv->d.nchunk > 1, L, LN, LE);
                gfx_lod_us += lod_now() - lt0;
                if (LN[0]) {
                    uint32_t extra = LN[0] + LN[1];
                    uint16_t *nib = realloc(ib, ((size_t)ni + lod_extra + extra) * 2 + 2);
                    if (nib) {
                        ib = nib;
                        for (int k = 0; k < 2; k++) if (LN[k]) {
                            gc->lni[k] = LN[k]; gc->lioff[k] = ni + lod_extra; gc->lerr[k] = LE[k];
                            for (uint32_t j = 0; j < LN[k]; j++) ib[ni + lod_extra + j] = (uint16_t)(L[k][j] + vo);
                            lod_extra += LN[k];
                        }
                        gfx_lod_tris[0] += c->ni / 3; gfx_lod_tris[1] += LN[0] / 3; gfx_lod_tris[2] += (LN[1] ? LN[1] : LN[0]) / 3;
                    }
                }
                free(L[0]); free(L[1]);
            }
            vo += c->nv; io += c->ni; tris += c->ni / 3;
        }
        glGenBuffers(2, &lv->gbuf[2 * g]);
        glBindBuffer(GL_ARRAY_BUFFER, lv->gbuf[2 * g]);
        glBufferData(GL_ARRAY_BUFFER, (size_t)vo * NR_VERTEX_SIZE, vb, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lv->gbuf[2 * g + 1]);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (size_t)(io + lod_extra) * 2, ib, GL_STATIC_DRAW);
        for (uint32_t q = lv->nch - (e - a); q < lv->nch; q++) { lv->ch[q].vbo = lv->gbuf[2 * g]; lv->ch[q].ibo = lv->gbuf[2 * g + 1]; }
        vbytes += vo * NR_VERTEX_SIZE + (io + lod_extra) * 2;
        free(vb); free(ib);
        if (gfx_load_tick && (g & 7) == 7) gfx_load_tick();
        a = e;
    }
    free(ord);
    glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    nr_mat *mats = lv->d.mats; float (*path)[3] = lv->d.path;
#ifdef NR_PC   /* (the test machine keeps the texture names: the pixel counts name the materials by them) */
    { char (*tn)[64] = lv->d.ntex ? malloc(64 * lv->d.ntex) : NULL; if (tn) memcpy(tn, lv->d.texnames, 64 * lv->d.ntex); lv->d.texnames = tn; }
#else
    lv->d.texnames = NULL;
#endif
    lv->d.chunks = (free(lv->d.chunks), NULL);
    free(blob);
    lv->d.mats = mats; lv->d.path = path;
    lv->pathlen = calloc(lv->d.npath + 1, sizeof(float));
    for (uint32_t i = 1; i < lv->d.npath; i++) {
        v3 a = v3make(path[i - 1][0], path[i - 1][1], path[i - 1][2]), b = v3make(path[i][0], path[i][1], path[i][2]);
        lv->pathlen[i] = lv->pathlen[i - 1] + len(sub(b, a));
    }
#ifdef NR_PC
    float fv = 0, fm = 0;
#else
    float fv = vglMemFree(VGL_MEM_VRAM) / 1048576.0f, fm = vglMemFree(VGL_MEM_RAM) / 1048576.0f;   /* (VGL_MEM_ALL: always 0) */
#endif
    logm("%s: %u chunks (%u buffers), %d tris, %u textures (%.1f MB new), geometry %.1f MB, free video memory %.1f MB, GPU main memory %.1f MB",
         lv->name, lv->nch, lv->ngrp, tris, lv->d.ntex, (texcache_bytes - tb0) / 1048576.0, vbytes / 1048576.0, fv, fm);
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
    if (lv->gbuf && lv->ngrp) glDeleteBuffers((GLsizei)(2 * lv->ngrp), lv->gbuf);
    free(lv->gbuf);
    if (lv->tex && GSW.no_tex_cache) for (uint32_t i = 0; i < lv->d.ntex; i++) if (lv->tex[i]) glDeleteTextures(1, &lv->tex[i]);
    if (lv->tex && !GSW.no_tex_cache) for (uint32_t i = 0; i < lv->d.ntex; i++) tex_unref(lv->tex[i]);
    free(lv->tex); free(lv->ch); free(lv->runs); free(lv->pathlen); free(lv->lights);
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
#ifdef NR_PC
    if (!p->lit) lt_cur = -1;
#endif
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
#ifdef NR_PC
    if (!p->lit) lt_cur = -1;
#endif
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
        if (m->variant == NR_ADD_GLASS) return object ? P_OBJADDG : P_WADDG;
        return object ? P_OBJADD : P_WADD;           /* (uMat.w 0: FS_ADD0) */
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
    if ((gfx_dbg & 256) && d && p->uSc >= 0 && nbil < 64) {   /* (the bench: the scrolling glow without mip blending) */
        int mip = 0, had = 0;
        for (int i = 0; i < ntex_mipped; i++) if (tex_mipped[i] == d) mip = 1;
        for (int i = 0; i < nbil; i++) if (bil_tex[i] == d) had = 1;
        if (mip && !had) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST); bil_tex[nbil++] = d; }
    }
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
        float sc[4], of[4];
        if (m->variant == NR_ADD_TRAFFIC) {          /* traffic.fx: fract(0.32 id) */
            sc[0] = 2.0f; sc[1] = 1.0f; sc[2] = 0.0f; sc[3] = t; of[0] = -5; of[1] = 0; of[2] = -5; of[3] = 0;
            glUniform4f(p->uC1, 0, 0, 0.4f, 0); glUniform4f(p->uC2, 0, 1, 0, 0);
        } else {                                      /* jet_flame.fx */
            sc[0] = 1.0f; sc[1] = 1.0f; sc[2] = 0.5f; sc[3] = t; of[0] = -10; of[1] = 10; of[2] = -17; of[3] = 7;
            glUniform4f(p->uC1, 0, 1, 0, 0); glUniform4f(p->uC2, 0, 0, -5, 0);
        }
        glUniform4fv(p->uSc, 1, sc);
        if (p->uOff >= 0) glUniform4fv(p->uOff, 1, of);
        if (p->uScv >= 0) glUniform4fv(p->uScv, 1, sc);
        if (p->uOffv >= 0) {                         /* (the vertex shader's: offset x Time, less whole repeats) */
            float ov[4];
            for (int k = 0; k < 4; k++) { float x = of[k] * t; ov[k] = x - floorf(x); }
            glUniform4fv(p->uOffv, 1, ov);
        }
    }
}

typedef struct { float d; int i; int prog; } drawitem;
static drawitem items[8192];
/* by program, then by place in the buffers (the parts' order there is by material): order doesn't change the picture
 * (the solid pass writes depth and the GPU sorts out what is hidden tile by tile; the glow adds up), and parts next to
 * each other in a buffer then draw at once */
static int by_prog_buf(const void *a, const void *b)
{
    const drawitem *x = a, *y = b;
    if (x->prog != y->prog) return x->prog - y->prog;
    return x->i - y->i;
}

static void draw_at(const gchunk *c, uint32_t ioff, uint32_t ni)   /* indices ioff.. of c's buffer */
{
    glBindBuffer(GL_ARRAY_BUFFER, c->vbo);
    vertex_layout(NULL);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, c->ibo);
    glDrawElements(GL_TRIANGLES, ni, GL_UNSIGNED_SHORT, (void *)((uintptr_t)ioff * 2));
    gfx_draws++; gfx_tris += ni / 3;
#ifdef NR_PC
    if (lt_cur >= 0) lt_tris[lt_cur] += ni / 3;
#endif
}
static void draw_chunk(const gchunk *c) { draw_at(c, c->ioff, c->ni); }
/* the ranges of a buffer to draw, those next to each other joined (one draw) */
static const gchunk *rg_c; static uint32_t rg_off, rg_n;
static void rg_add(const gchunk *c, uint32_t ioff, uint32_t ni)
{
    if (rg_n && rg_c->ibo == c->ibo && rg_off + rg_n == ioff) { rg_n += ni; return; }
    if (rg_n) draw_at(rg_c, rg_off, rg_n);
    rg_c = c; rg_off = ioff; rg_n = ni;
}
static void rg_flush(void) { if (rg_n) draw_at(rg_c, rg_off, rg_n); rg_n = 0; }
/* a part: whole, or its runs in view (not when it is all in view: then whole, without looking at them) */
static void rg_chunk(const level_rt *lv, const gchunk *c)
{
    if (!c->nrun || GSW.no_runs || aabb_inside(c->bmin, c->bmax)) { rg_add(c, c->ioff, c->ni); return; }
    for (uint32_t r = 0; r < c->nrun; r++) {
        const grun *R = &lv->runs[c->run0 + r];
        if (aabb_visible(R->bmin, R->bmax)) rg_add(c, R->ioff, R->ni);
    }
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
        /* the parts after it in the same buffer, also in view: one draw */
        uint32_t ni = c->ni;
        FD_LABEL("m%d %s", c->mat, lv->d.mats[c->mat].diffuse >= 0 && lv->d.texnames ? lv->d.texnames[lv->d.mats[c->mat].diffuse] : "-");
        rg_chunk(lv, c);
        while (k + 1 < n && items[k + 1].prog == items[k].prog) {
            const gchunk *c2 = &lv->ch[items[k + 1].i];
            if (c2->ibo != c->ibo || c2->ioff != c->ioff + ni || c2->mat != c->mat) break;
            ni += c2->ni; k++;
            rg_chunk(lv, c2);
        }
        rg_flush();
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
        items[n].d = c->kind == NR_KIND_BACKDROP ? 1e9f : d; items[n].prog = prog_for(m, 0);
        if ((gfx_dbg & 128) && items[n].prog == P_WSCROLL) items[n].prog = P_WADD;    /* (the bench: the scrolling glow as plain glow) */
        items[n++].i = i;
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

/* the four level lights that light a point the most (tubes: their nearest point), as the object shaders get them
 * (uLA: where, near; uLB: colour, far; uLamb: their ambient shares). In the order of the level's lights, so that
 * objects lit by the same four get the same uniforms (and may be drawn together) */
typedef struct { float la[4][4], lb[4][4], amb[4], lk[4], lr[4]; int n; } obj_lights_t;   /* (n: how many, in the first slots) */
static void light_pick(v3 at, obj_lights_t *out)
{
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
    /* the chosen four in the level's order (the shader adds them up: the order only matters to the uniforms) */
    for (int k = 1; k < 4; k++) for (int j = k; j > 0 && bi[j] >= 0 && (bi[j - 1] < 0 || bi[j - 1] > bi[j]); j--) {
        int ti = bi[j]; bi[j] = bi[j - 1]; bi[j - 1] = ti; v3 tp = bp[j]; bp[j] = bp[j - 1]; bp[j - 1] = tp;
    }
    memset(out, 0, sizeof *out);
    for (int k = 0; k < 4; k++) {
        if (bi[k] < 0) { out->la[k][2] = -1e5f; out->la[k][3] = 1; out->lb[k][3] = 2; continue; }
        out->n++;
        const gfx_light *L = &lv->lights[bi[k]];
        out->la[k][0] = bp[k].x; out->la[k][1] = bp[k].y; out->la[k][2] = bp[k].z; out->la[k][3] = L->near_;
        out->lb[k][0] = L->col[0]; out->lb[k][1] = L->col[1]; out->lb[k][2] = L->col[2]; out->lb[k][3] = L->far_;
        out->amb[k] = L->amb;
    }
    for (int k = 0; k < 4; k++) {                        /* (the shader's form: K = near / (far - near), far) */
        float n_ = out->la[k][3], f_ = out->lb[k][3], df = f_ - n_;
        out->lk[k] = df != 0 ? n_ / df : (n_ >= 0 ? 1e30f : -1e30f); out->lr[k] = f_;
    }
}
static void light_apply(prog_t *p, const obj_lights_t *l)
{
    if (p->uLA[0] < 0) return;                           /* (a program without lights, or for none) */
    for (int k = 0; k < 4 && p->uLA[k] >= 0; k++) { glUniform4fv(p->uLA[k], 1, l->la[k]); glUniform4fv(p->uLB[k], 1, l->lb[k]); }
    glUniform4fv(p->uLamb, 1, l->amb);
    if (p->uLK >= 0) glUniform4fv(p->uLK, 1, l->lk);
    if (p->uLR >= 0) glUniform4fv(p->uLR, 1, l->lr);
}
/* the program for an object lit by n lights: the one for just those (not in the cinematics, whose own variants work
 * out all four; nor with gfx_dbg 512, the bench's "objects: four lights each") */
static prog_t *prog_lights(prog_t *p, int pi, int n, int inst)
{
    if (p->lit != 5) return p;
#ifdef NR_PC
    lt_cur = n;
    if (getenv("ALLLIGHTS")) return p;
#endif
    if (n >= NLV || (gfx_dbg & 512) || gfx_redblue.on) return p;
    prog_t *v = inst ? &PILV[n][pi] : &PLV[n][pi];
    return v->prog ? v : p;
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

/* the simpler shapes: lod_k is this frame's pixels per unit at depth 1; an object's factor (its scale, over the depth
 * of its nearest point) times a shape's error is how far the shape strays on screen, in pixels: up to LOD_PX, drawn */
static float lod_k;
static float LOD_PX = 0.25f;                       /* (a quarter of a pixel: thin far parts - posts, spikes - don't blink out) */
/* (x this while the dynamic resolution has the 3D scene under full size: half a pixel then, a third fewer of the moving
 * objects' triangles in the heaviest views - moto's end: 54000 to 33000 - where the scene's size alone gains little) */
float gfx_lod_bias = 1;
static float obj_lod_f(const gfx_obj *o)
{
    const level_rt *m = o->model;
    if ((gfx_dbg & 1024) && gh_is_model(m)) return 0;   /* (the bench: the ghost cars in their simplest shapes) */
    if (lod_k <= 0 || GSW.no_lod || !(m->bmax[0] >= m->bmin[0])) return 1e30f;
#ifdef NR_PC
    { static int once; if (!once) { once = 1; if (getenv("LODPX")) LOD_PX = (float)atof(getenv("LODPX")); } }
#endif
    float lc[3], h[3], c[3], r2 = 0, s2 = 0;
    for (int k = 0; k < 3; k++) { lc[k] = 0.5f * (m->bmin[k] + m->bmax[k]); h[k] = 0.5f * (m->bmax[k] - m->bmin[k]); }
    for (int r = 0; r < 3; r++) c[r] = o->m[r][0] * lc[0] + o->m[r][1] * lc[1] + o->m[r][2] * lc[2] + o->m[r][3];
    for (int k = 0; k < 3; k++) {
        float l2 = o->m[0][k] * o->m[0][k] + o->m[1][k] * o->m[1][k] + o->m[2][k] * o->m[2][k];
        r2 += l2 * h[k] * h[k]; if (l2 > s2) s2 = l2;
    }
    float w = VP[3][0] * c[0] + VP[3][1] * c[1] + VP[3][2] * c[2] + VP[3][3] - sqrtf(r2);    /* (its nearest depth) */
    return w > 0.5f ? sqrtf(s2) * lod_k / w : 1e30f;
}
static int chunk_lod(const gchunk *c, float f)          /* -> 0 the full shape, 1 or 2 a simpler one */
{
    for (int k = 1; k >= 0; k--) if (c->lni[k] && c->lerr[k] * f <= LOD_PX * gfx_lod_bias) return k + 1;
    return 0;
}
static void draw_chunk_lod(const gchunk *c, int lod)
{
    if (!lod) { draw_chunk(c); return; }
    glBindBuffer(GL_ARRAY_BUFFER, c->vbo);
    vertex_layout(NULL);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, c->ibo);
    glDrawElements(GL_TRIANGLES, c->lni[lod - 1], GL_UNSIGNED_SHORT, (void *)((uintptr_t)c->lioff[lod - 1] * 2));
    gfx_draws++; gfx_tris += c->lni[lod - 1] / 3;
#ifdef NR_PC
    if (lt_cur >= 0) lt_tris[lt_cur] += c->lni[lod - 1] / 3;
#endif
}

/* a moving object's big part drawn whole: its runs in view (their boxes moved to world space; not while it warps in or
 * out: its triangles leave their places) */
static void obj_box(const gfx_obj *o, const float *mn, const float *mx, float wmn[3], float wmx[3])
{
    for (int r = 0; r < 3; r++) {
        float c = o->m[r][3], e = 0;
        for (int k = 0; k < 3; k++) { c += o->m[r][k] * 0.5f * (mn[k] + mx[k]); e += fabsf(o->m[r][k]) * 0.5f * (mx[k] - mn[k]); }
        wmn[r] = c - e; wmx[r] = c + e;
    }
}
static void draw_obj_runs(const level_rt *m, const gchunk *c, const gfx_obj *o)
{
    float wmn[3], wmx[3];
    obj_box(o, c->bmin, c->bmax, wmn, wmx);
    if (aabb_inside(wmn, wmx)) { draw_chunk(c); return; }
    for (uint32_t r = 0; r < c->nrun; r++) {
        const grun *R = &m->runs[c->run0 + r];
        obj_box(o, R->bmin, R->bmax, wmn, wmx);
        if (aabb_visible(wmn, wmx)) rg_add(c, R->ioff, R->ni);
    }
    rg_flush();
}
static void draw_obj(const gfx_obj *o, int pass)    /* 0 solid, 1 additive */
{
    const level_rt *m = o->model;
    if (!m || !m->nch) return;
    prog_t *lastp = NULL;
    float lf = obj_lod_f(o);
    obj_lights_t L; int lit = 0;
    for (uint32_t i = 0; i < m->nch; i++) {
        const gchunk *c = &m->ch[i];
        const nr_mat *mt = &m->d.mats[c->mat];
        int pi = prog_for(mt, 1);
        if ((gfx_dbg & 128) && pi == P_OBJSCROLL) pi = P_OBJADD;   /* (the bench: the scrolling glow as plain glow) */
        if (prog_additive(pi) != (pass == 1)) continue;
        /* no reflection map and no tint: the plain lit shader (no sphere map work, one read less) */
        if (pi == P_OBJ && !(mt->reflect >= 0 && m->tex[mt->reflect]) && o->tint[3] <= 0 && !(mt->flags & (NR_MAT_SPHERE_N | NR_MAT_REFSPEC)))
            pi = P_OBJLIT;
        prog_t *p = prog_get(pi);
        if (p->lit || p->uLA[0] >= 0) {                  /* (the lights at its origin: those its program works out) */
            if (!lit) { light_pick(v3make(o->m[0][3], o->m[1][3], o->m[2][3]), &L); lit = 1; }
            p = prog_lights(p, pi, L.n, 0);
        }
        if (p != lastp) {
            lastp = p;
            use_prog(p);
            if (p->uOFlag >= 0) glUniform4f(p->uOFlag, o->use_col ? 1.0f : 0.0f, 0, 1.0f + o->phase, 0);
            if (o->use_col && p->uOCol >= 0) glUniform4fv(p->uOCol, 1, o->col);
            for (int r = 0; r < 3; r++) glUniform4f(p->uM[r], o->m[r][0], o->m[r][1], o->m[r][2], o->m[r][3]);
            if (p->uIntro >= 0) glUniform4f(p->uIntro, 1.25f - 2.7f * o->intro_p, 0, 0, 0);
            if (p->uPortal >= 0) glUniform4fv(p->uPortal, 1, o->portal);
            if (lit) light_apply(p, &L);
        }
        cur_phase = o->phase;
        bind_material(m, mt, p, o->glow > 0 ? o->glow : 1.0f, o->use_col ? 1.0f : (m->d.glow_scale > 0 ? m->d.glow_scale / 8.0f : 1.0f));
        if (p->uTint >= 0) glUniform4fv(p->uTint, 1, o->tint);
        FD_LABEL("%s m%d %s", m->name, c->mat, mt->diffuse >= 0 && m->d.texnames ? m->d.texnames[mt->diffuse] : "-");
        int lod = chunk_lod(c, lf);
        if (!lod && c->nrun && !GSW.no_runs && !o->no_cull && pi != P_OBJINTRO) draw_obj_runs(m, c, o);
        else draw_chunk_lod(c, lod);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

/* the objects of one pass. With instancing: the visible objects of one-part models are grouped by everything their
 * draw sets apart from the model matrix (program, model, colour, glow, tint, phase, lights); a group of two or more
 * is one draw, the rest are drawn one by one as before. (Draw order within a pass doesn't change the picture: the
 * solid pass writes depth, the glow pass adds up) */
typedef struct {
    const level_rt *model; int prog, use_col, lod;
    float glow, phase, tint[4], col[4];
    obj_lights_t L;
} inst_key;
typedef struct { uint32_t h; int obj; inst_key k; } inst_rec;
static inst_rec *IR; static int ir_cap;
static int (*GR)[2], gr_cap;                       /* (the groups: first record, how many) */
static int grp_cmp(const void *a, const void *b) { return IR[((const int *)a)[0]].obj - IR[((const int *)b)[0]].obj; }
/* (the records sorted so that those with the same key lie together: by the key's hash, the model by its name - not by
 * where it is in memory, which differs from run to run on the PC - then the object) */
static int inst_cmp(const void *a, const void *b)
{
    const inst_rec *x = a, *y = b;
    if (x->h != y->h) return x->h < y->h ? -1 : 1;
    if (x->k.model != y->k.model) { int c = strcmp(x->k.model->name, y->k.model->name); if (c) return c; }
    return x->obj - y->obj;
}
static int obj_prog(const level_rt *m, const nr_mat *mt, const gfx_obj *o)
{
    int pi = prog_for(mt, 1);
    if ((gfx_dbg & 128) && pi == P_OBJSCROLL) pi = P_OBJADD;
    if (pi == P_OBJ && !(mt->reflect >= 0 && m->tex[mt->reflect]) && o->tint[3] <= 0 && !(mt->flags & (NR_MAT_SPHERE_N | NR_MAT_REFSPEC)))
        pi = P_OBJLIT;
    return pi;
}
static void draw_inst_group(const gfx_obj *objs, const inst_rec *r, int n)
{
    const inst_key *k = &r[0].k;
    const level_rt *m = k->model; const gchunk *c = &m->ch[0]; const nr_mat *mt = &m->d.mats[c->mat];
    prog_t *p = prog_lights(&PI[k->prog], k->prog, k->L.n, 1);
    use_prog(p);
    if (p->uOFlag >= 0) glUniform4f(p->uOFlag, k->use_col ? 1.0f : 0.0f, 0, 1.0f + k->phase, 0);
    if (k->use_col && p->uOCol >= 0) glUniform4fv(p->uOCol, 1, k->col);
    if (p->uIntro >= 0) glUniform4f(p->uIntro, 1.25f, 0, 0, 0);
    light_apply(p, &k->L);
    cur_phase = k->phase;
    bind_material(m, mt, p, k->glow > 0 ? k->glow : 1.0f, k->use_col ? 1.0f : (m->d.glow_scale > 0 ? m->d.glow_scale / 8.0f : 1.0f));
    if (p->uTint >= 0) glUniform4fv(p->uTint, 1, k->tint);
    while (n > 0) {
        if (inst_n >= INST_MAX) {                          /* (the ring's part for this frame is full: one by one) */
            for (int i = 0; i < n; i++) draw_obj(&objs[r[i].obj], prog_additive(k->prog));
            return;
        }
        int m_ = n < INST_MAX - inst_n ? n : INST_MAX - inst_n;
        float (*rows)[12] = inst_ring + (size_t)inst_seg * INST_MAX + inst_n;
        for (int i = 0; i < m_; i++) memcpy(rows[i], objs[r[i].obj].m, 48);   /* (the matrix's three rows) */
        inst_n += m_;
        glBindBuffer(GL_ARRAY_BUFFER, c->vbo);
        vertex_layout(NULL);
        inst_attribs((const float (*)[12])rows);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, c->ibo);
        FD_LABEL("%s m%d %s", m->name, c->mat, mt->diffuse >= 0 && m->d.texnames ? m->d.texnames[mt->diffuse] : "-");
        uint32_t dni = k->lod ? c->lni[k->lod - 1] : c->ni, dio = k->lod ? c->lioff[k->lod - 1] : c->ioff;
        glDrawElementsInstanced(GL_TRIANGLES, dni, GL_UNSIGNED_SHORT, (void *)((uintptr_t)dio * 2), m_);
        inst_attribs_off();
        gfx_draws++; gfx_tris += dni / 3 * m_; gfx_inst_draws++; gfx_inst_objs += m_;
#ifdef NR_PC
        if (lt_cur >= 0) lt_tris[lt_cur] += dni / 3 * m_;
#endif
        r += m_; n -= m_;
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}
static void draw_objs(const gfx_obj *objs, int nobj, const unsigned char *ovis, int pass)
{
    if (!inst_ok || gfx_redblue.on || (gfx_dbg & 32)) { for (int i = 0; i < nobj; i++) if (ovis[i]) draw_obj(&objs[i], pass); return; }   /* (gfx_dbg 32: the bench) */
    if (ir_cap < nobj) { free(IR); ir_cap = nobj + 256; IR = malloc(sizeof(inst_rec) * ir_cap); if (!IR) { ir_cap = 0; return; } }
    int nr = 0;
    for (int i = 0; i < nobj; i++) {
        if (!ovis[i]) continue;
        const gfx_obj *o = &objs[i]; const level_rt *m = o->model;
        if (m->nch != 1) { draw_obj(o, pass); continue; }
        const nr_mat *mt = &m->d.mats[m->ch[0].mat];
        int pi = obj_prog(m, mt, o);
        if (prog_additive(pi) != (pass == 1)) continue;       /* (nothing of it in this pass) */
        if (!PI[pi].prog) { draw_obj(o, pass); continue; }
        inst_rec *r = &IR[nr++];
        memset(&r->k, 0, sizeof r->k);
        r->obj = i; r->k.model = m; r->k.prog = pi; r->k.use_col = o->use_col; r->k.glow = o->glow;
        r->k.lod = chunk_lod(&m->ch[0], obj_lod_f(o));
        r->k.phase = PI[pi].uSc >= 0 ? o->phase : 0;          /* (the phase: only the scrolling programs read it) */
        memcpy(r->k.tint, o->tint, 16);
        if (o->use_col) memcpy(r->k.col, o->col, 16);
        if (PI[pi].uLA[0] >= 0) light_pick(v3make(o->m[0][3], o->m[1][3], o->m[2][3]), &r->k.L);
        inst_key kk = r->k; kk.model = NULL;
        const uint8_t *b = (const uint8_t *)&kk; uint32_t h = 2166136261u;
        for (const char *q = m->name; *q; q++) h = (h ^ (uint8_t)*q) * 16777619u;
        for (size_t q = 0; q < sizeof kk; q++) h = (h ^ b[q]) * 16777619u;
        r->h = h;
    }
    if (!nr) return;
    qsort(IR, nr, sizeof *IR, inst_cmp);
#ifdef NR_PC
    if (getenv("INSTDBG")) {   /* (the test machine: what keeps a model's copies apart, once every 5 s) */
        static int fr; if (++fr % 300 == 0) {
            for (int a = 0; a < nr; a++) {
                int same_model_before = 0; for (int q = 0; q < a; q++) if (IR[q].k.model == IR[a].k.model) { same_model_before = 1; break; }
                if (same_model_before) continue;
                int n = 0, dl = 0, dg = 0, dp = 0, dt = 0, dc = 0, dpr = 0; const inst_key *k0 = &IR[a].k;
                for (int q = a; q < nr; q++) if (IR[q].k.model == k0->model) {
                    const inst_key *k = &IR[q].k; n++;
                    dl += memcmp(&k->L, &k0->L, sizeof k->L) != 0; dg += k->glow != k0->glow; dp += k->phase != k0->phase;
                    dt += memcmp(k->tint, k0->tint, 16) != 0; dc += k->use_col != k0->use_col || memcmp(k->col, k0->col, 16) != 0; dpr += k->prog != k0->prog;
                }
                if (n > 2) fprintf(stderr, "INSTDBG pass %d %s x%d: lights differ %d, glow %d, phase %d, tint %d, colour %d, program %d\n", pass, k0->model->name, n, dl, dg, dp, dt, dc, dpr);
            }
        }
    }
#endif
    /* the groups drawn in the order of their first objects (as one by one), so that which of two objects' faces in the
     * same place shows doesn't change with what sets the groups apart (their lights, their simpler shapes) */
    if (gr_cap < nr) { free(GR); gr_cap = nr + 256; GR = malloc(sizeof *GR * gr_cap); if (!GR) gr_cap = 0; }
    int ng = 0;
    for (int a = 0; a < nr; ) {
        int e = a + 1;
        while (e < nr && IR[e].h == IR[a].h && !memcmp(&IR[e].k, &IR[a].k, sizeof IR[a].k)) e++;
        if (GR) { GR[ng][0] = a; GR[ng][1] = e - a; ng++; }
        else if (e - a == 1) draw_obj(&objs[IR[a].obj], pass);
        else draw_inst_group(objs, &IR[a], e - a);
        a = e;
    }
    if (!GR) return;
    qsort(GR, ng, sizeof *GR, grp_cmp);
    for (int g = 0; g < ng; g++) {
        if (GR[g][1] == 1) draw_obj(&objs[IR[GR[g][0]].obj], pass);
        else draw_inst_group(objs, &IR[GR[g][0]], GR[g][1]);
    }
}

/* camera-facing quads, additive */
static void draw_sprites(const gfx_sprite *s, int n, v3 fwd, v3 upv)
{
    if (n <= 0) return;
    static uint8_t buf[64 * 6 * NR_VERTEX_SIZE];
    v3 r = norm(cross(fwd, upv)), u = cross(r, fwd);
    prog_t *p = P[P_SPRITE].prog ? &P[P_SPRITE] : &P[P_WADD];
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
    FD_LABEL("sprites");
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
    FD_LABEL("quads%s", subtract ? " (subtract)" : "");
    glDrawElements(GL_TRIANGLES, nquads * 6, GL_UNSIGNED_SHORT, idx);
    if (subtract) glBlendEquation(GL_FUNC_ADD);
    gfx_draws++; gfx_tris += nquads * 2;
}

/* ---- the GPU particles (particles.c): rings of corners in memory the GPU reads as it is (no copies), a static
 * index buffer of quads */
#define PART_QMAX 16384               /* quads a draw (16-bit indices) */
static void *gfx_pring_alloc_bytes(size_t bytes, unsigned *buf)
{
#ifdef NR_PC
    *buf = 0;
    return calloc(1, bytes);
#else
    /* the rings are rewritten every frame by the CPU and read by the GPU, so their memory must be uncached: what the CPU
     * writes into cached memory can stay in its cache, the GPU reading what was there before - part of a quad old and
     * part new, a wedge across the screen. vitaGL's pools are all cached but its video memory (vitaGL.h: its RAM and
     * physically contiguous pools are USER_RW and PHYCONT_USER_RW): the C library's heap first drew warp_speed's wing
     * trails as a white wedge at 79 ms a frame, and the physically contiguous pool later gave the particles' hitches -
     * 100-500 ms frames whose quads were half from an earlier level's particles in the same slots (the GPU drew the
     * slots of no size just as slowly, and as much of the screen, while without them the frame took 5.6 ms). So a block
     * of the system's uncached memory, mapped for the GPU; video memory (uncached to the CPU) if there is none */
    const char *pool = "uncached main memory";
    bytes = (bytes + 4096 + 4095) & ~(size_t)4095;   /* (a page more than the ring: the GPU may read past its last slot) */
    void *m = NULL;
    {   SceUID uid = sceKernelAllocMemBlock("nr_gpu_ring", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, (SceSize)bytes, NULL);
        if (uid >= 0 && sceKernelGetMemBlockBase(uid, &m) >= 0 && m && sceGxmMapMemory(m, (SceSize)bytes, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE) >= 0) {}
        else { if (uid >= 0) sceKernelFreeMemBlock(uid); m = NULL; logm("  (uncached memory for a GPU ring: not given, 0x%08x)", (unsigned)uid); }
    }
    if (!m) { m = vglAlloc((uint32_t)bytes, VGL_MEM_VRAM); pool = "video memory"; }
    if (!m) { m = vglAlloc((uint32_t)bytes, VGL_MEM_PHYCONT); pool = "physically contiguous memory (cached: stale data possible)"; }
    logm("  a %.2f MB ring for the GPU in %s", bytes / 1048576.0f, pool);
    if (!m) { *buf = 0; return NULL; }
    memset(m, 0, bytes);
    GLuint b = 0; glGenBuffers(1, &b);
    glBindBuffer(GL_ARRAY_BUFFER, b);
    vglBufferData(GL_ARRAY_BUFFER, m);            /* the buffer IS that memory */
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    *buf = b;
    return m;
#endif
}
/* (the hitch hunt, part_dbg 8192: each particle ring's memory let go and a new block given at each level, as the bench's
 * copy "from a ring made then" that drew the hitch's particles in 1.4 ms: the GPU idle first, the old block unmapped
 * and freed, the new one mapped and put under the same GL buffer). 0: renewed; -1: left as it was; -2: lost */
int gfx_pring_renew(gfx_pvert **ring, unsigned *buf, uint32_t nverts)
{
#ifdef NR_PC
    (void)ring; (void)buf; (void)nverts;
    return -1;
#else
    if (!*ring || !*buf) return -1;
    SceUID uid = sceKernelFindMemBlockByAddr(*ring, 0);
    if (uid < 0) return -1;                         /* (not a block of its own: video memory, a pool) */
    glFinish();                                     /* (nothing on the GPU reads it any more) */
    size_t bytes = ((size_t)nverts * sizeof(gfx_pvert) + 4096 + 4095) & ~(size_t)4095;
    sceGxmUnmapMemory(*ring); sceKernelFreeMemBlock(uid);
    void *m = NULL;
    SceUID nu = sceKernelAllocMemBlock("nr_gpu_ring", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, (SceSize)bytes, NULL);
    if (nu < 0 || sceKernelGetMemBlockBase(nu, &m) < 0 || !m || sceGxmMapMemory(m, (SceSize)bytes, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE) < 0) {
        if (nu >= 0) sceKernelFreeMemBlock(nu);
        logm("  a particles' ring: its new memory not given (0x%08x): no particles", (unsigned)nu);
        *ring = NULL; return -2;
    }
    memset(m, 0, bytes);
    glBindBuffer(GL_ARRAY_BUFFER, *buf); vglBufferData(GL_ARRAY_BUFFER, m); glBindBuffer(GL_ARRAY_BUFFER, 0);
    static int said;
    if (said++ < 6) logm("  a particles' ring renewed: %p -> %p (%.2f MB)", (void *)*ring, m, bytes / 1048576.0f);
    *ring = (gfx_pvert *)m;
    return 0;
#endif
}
int gfx_part_dbg;
/* (the hitch hunt: a buffer in video memory, whatever part_dbg says) */
gfx_pvert *gfx_pring_alloc_vram(uint32_t nverts, unsigned *buf)
{
#ifndef NR_PC
    size_t bytes = (size_t)nverts * sizeof(gfx_pvert) + 4096;
    void *m = vglAlloc((uint32_t)bytes, VGL_MEM_VRAM);
    if (m) {
        logm("  a %.2f MB copy for the GPU in video memory (the hitch bench)", bytes / 1048576.0f);
        memset(m, 0, bytes);
        GLuint b = 0; glGenBuffers(1, &b); glBindBuffer(GL_ARRAY_BUFFER, b); vglBufferData(GL_ARRAY_BUFFER, m); glBindBuffer(GL_ARRAY_BUFFER, 0);
        *buf = b; return m;
    }
#endif
    *buf = 0; return NULL;
}
gfx_pvert *gfx_pring_alloc(uint32_t nverts, unsigned *buf)
{
#ifndef NR_PC
    if (gfx_part_dbg & 64) {                       /* (the hitch hunt: the particles' rings in video memory) */
        size_t bytes = (size_t)nverts * sizeof(gfx_pvert) + 4096;
        void *m = vglAlloc((uint32_t)bytes, VGL_MEM_VRAM);
        if (m) {
            logm("  a %.2f MB ring for the GPU in video memory (part_dbg 64)", bytes / 1048576.0f);
            memset(m, 0, bytes);
            GLuint b = 0; glGenBuffers(1, &b); glBindBuffer(GL_ARRAY_BUFFER, b); vglBufferData(GL_ARRAY_BUFFER, m); glBindBuffer(GL_ARRAY_BUFFER, 0);
            *buf = b; return m;
        }
    }
#endif
    return gfx_pring_alloc_bytes((size_t)nverts * sizeof(gfx_pvert), buf);
}
GLuint gfx_texture_rgba(const void *rgba, int w, int h, int linear)
{
    GLuint id; glGenTextures(1, &id); glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return id;
}
static uint16_t *part_idx; static GLuint part_ibo;
static float part_k[4] = { -1e9f, 1, 1, 1 };
void gfx_particles_scale(float near0, float near1, float size, float inten)
{
    part_k[0] = near0; part_k[1] = near1 > near0 ? 1.0f / (near1 - near0) : 1; part_k[2] = size; part_k[3] = inten;
}
/* every: draw only the slots that are multiples of it (the smoke thinned out: particles.c) - the indices of every
 * every-th quad, made once for each step (2 to 8) */
static uint16_t *part_idx_n[9]; static GLuint part_ibo_n[9];
static void *part_ibo_mem, *part_ibo_n_mem[9];       /* (where each index buffer's indices were put: ibo_check) */
/* (an index buffer of the particles still where it was put - as ring_buf_check for the rings; made again if not) */
static void ibo_check(GLuint *ibo, void **mem, const void *src, size_t bytes, const char *what)
{
#ifndef NR_PC
    if (!*ibo || !src || *(void *const *)(uintptr_t)*ibo == *mem) return;
    static int said;
    if (said++ < 12) logm("  the GL buffer of %s no longer pointed at its indices (%p, not %p): made again", what, *(void *const *)(uintptr_t)*ibo, *mem);
    glGenBuffers(1, ibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, *ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizei)bytes, src, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    *mem = *(void *const *)(uintptr_t)*ibo;
#else
    (void)ibo; (void)mem; (void)src; (void)bytes; (void)what;
#endif
}
static const uint16_t *part_every(int m)
{
    if (!part_idx_n[m]) {
        uint16_t *ix = malloc((PART_QMAX / m + 1) * 6 * sizeof(uint16_t));
        if (!ix) return NULL;
        for (int q = 0; q <= PART_QMAX / m && q * m < PART_QMAX; q++) {
            static const int o[6] = { 0, 1, 2, 0, 2, 3 };
            for (int k = 0; k < 6; k++) ix[q * 6 + k] = (uint16_t)(q * m * 4 + o[k]);
        }
        part_idx_n[m] = ix;
#ifndef NR_PC
        glGenBuffers(1, &part_ibo_n[m]); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, part_ibo_n[m]);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (PART_QMAX / m + 1) * 6 * sizeof(uint16_t), ix, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        part_ibo_n_mem[m] = *(void *const *)(uintptr_t)part_ibo_n[m];
#endif
    }
    return part_idx_n[m];
}
void gfx_particles(const gfx_pvert *ring, unsigned *buf, uint32_t first, uint32_t n, GLuint atlas, GLuint pal, float now, v3 right, v3 up, int subtract, int one_slice, int every)
{
    if (!n || !P[P_PART].prog) return;
    if (buf) { ring_watch_add(ring, buf); ring_buf_check(ring, buf, "a particles' ring"); }   /* (buf NULL: from the heap, the hitch hunt) */
    if (every > 8) every = 8;
    if (every > 1) {                                   /* (from the first slot that is a multiple of it) */
        uint32_t f2 = (first + every - 1) / every * every;
        if (f2 >= first + n || n > PART_QMAX || !part_every(every)) { if (f2 >= first + n) return; every = 1; }
        else { n = (first + n - f2 + every - 1) / every; first = f2; }
    }
    if (!part_idx) {
        part_idx = malloc(PART_QMAX * 6 * sizeof(uint16_t));
        if (!part_idx) return;
        for (int q = 0; q < PART_QMAX; q++) {
            static const int o[6] = { 0, 1, 2, 0, 2, 3 };
            for (int k = 0; k < 6; k++) part_idx[q * 6 + k] = (uint16_t)(q * 4 + o[k]);
        }
#ifndef NR_PC
        glGenBuffers(1, &part_ibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, part_ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, PART_QMAX * 6 * sizeof(uint16_t), part_idx, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        part_ibo_mem = *(void *const *)(uintptr_t)part_ibo;
#endif
    }
    ibo_check(&part_ibo, &part_ibo_mem, part_idx, PART_QMAX * 6 * sizeof(uint16_t), "the particles' index buffer");
    if (every > 1) ibo_check(&part_ibo_n[every], &part_ibo_n_mem[every], part_idx_n[every], (PART_QMAX / every + 1) * 6 * sizeof(uint16_t), "a thinned particles' index buffer");
    prog_t *p = one_slice && P[P_PART1].prog ? &P[P_PART1] : &P[P_PART];
    frame_uniforms(p);
    if (p->uPN >= 0) glUniform4f(p->uPN, now, 0, 0, 0);
    if (p->uPR >= 0) glUniform3f(p->uPR, right.x, right.y, right.z);
    if (p->uPU >= 0) glUniform3f(p->uPU, up.x, up.y, up.z);
    if (p->uPK >= 0) { float k[4] = { part_k[0], part_k[1], gfx_part_dbg & 4 ? 0 : part_k[2], part_k[3] }; glUniform4fv(p->uPK, 1, k); }
    if ((gfx_part_dbg & 32) && every == 1 && n >= 4) { first += n - n / 4; n /= 4; }   /* (the hitch hunt: the newest quarter) */
    if ((gfx_part_dbg & 512) && every == 1 && n >= 4) n -= n / 4;                     /* (the oldest three quarters) */
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, pal);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, atlas);
    if (subtract) glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
    const GLsizei st = (GLsizei)sizeof(gfx_pvert);
    while (n > 0) {
        uint32_t m = n > PART_QMAX ? PART_QMAX : n;    /* (with every > 1: n is the quads drawn, all in one draw) */
        if ((gfx_part_dbg & 1) && every == 1 && m > 256) m = 256;   /* (the hitch hunt: draws of 256 quads) */
#ifdef NR_PC
        const uint8_t *b = (const uint8_t *)(ring + (size_t)first * 4);
        glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        const void *ix = every > 1 ? (const void *)part_idx_n[every] : (const void *)part_idx;
#else
        const uint8_t *b; const void *ix;
        if (!buf) {                                    /* (the hitch hunt: from the heap, vitaGL copying it at the draw) */
            b = (const uint8_t *)(ring + (size_t)first * 4);
            glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
            ix = every > 1 ? (const void *)part_idx_n[every] : (const void *)part_idx;
        } else {
            b = (const uint8_t *)((uintptr_t)first * 4 * st);   /* (offsets in the ring's buffer) */
            glBindBuffer(GL_ARRAY_BUFFER, *buf); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, every > 1 ? part_ibo_n[every] : part_ibo);
            ix = 0;
        }
#endif
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, st, b + offsetof(gfx_pvert, p));
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, st, b + offsetof(gfx_pvert, v));
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, st, b + offsetof(gfx_pvert, l));
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, st, b + offsetof(gfx_pvert, t));
        glVertexAttribPointer(4, 4, GL_BYTE, GL_TRUE, st, b + offsetof(gfx_pvert, c));
        FD_LABEL("particles%s", subtract ? " (subtract)" : "");
        glDrawElements(GL_TRIANGLES, (GLsizei)(m * 6), GL_UNSIGNED_SHORT, ix);
        gfx_draws++; gfx_tris += m * 2;
        first += m; n -= m;
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    if (subtract) glBlendEquation(GL_FUNC_ADD);
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
    FD_LABEL("tracks %s", pass ? "glow" : "dark");
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
/* (for the GPU's clock, gputime.c: when the frame's first scene - the 3D scene's target - went to the GPU, at the first
 * screen pass; and the size the 3D scene was really drawn at) */
uint64_t gfx_kick_us; float gfx_scale_drawn = 1;

/* ---- the new passes' drawing: a rectangle of the target (pixels x0..x1 of its width, all its height) with the
 * vertex coordinates u0..u1 across, v0..v1 down; and the same as a grid of gx x 9 quads (per-vertex work that isn't
 * linear: the speed blur's taps). The target is bound by the caller */
static void rect_pass(prog_t *p, const target_t *t, int x0, int x1, float u0, float u1, float v0, float v1)
{
    const float a = 2.0f * x0 / t->w - 1, b = 2.0f * x1 / t->w - 1;
    const float v[16] = { a, -1, u0, v0,  b, -1, u1, v0,  a, 1, u0, v1,  b, 1, u1, v1 };
    glUseProgram(p->prog);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, v);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
static void grid_rect(prog_t *p, const target_t *t, int x0, int x1, int gx, float u0, float u1)
{
    enum { GY = 9 };
    static float v[16 * GY * 6 * 4];
    if (gx > 16) gx = 16;
    int nv = 0;
    for (int y = 0; y < GY; y++) for (int x = 0; x < gx; x++) {
        static const int c[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 0, 1 }, { 1, 0 }, { 1, 1 } };
        for (int k = 0; k < 6; k++) {
            float fx = (float)(x + c[k][0]) / gx, fy = (float)(y + c[k][1]) / GY;
            float *o = v + nv * 4;
            o[0] = 2.0f * (x0 + (x1 - x0) * fx) / t->w - 1; o[1] = fy * 2 - 1; o[2] = u0 + (u1 - u0) * fx; o[3] = fy; nv++;
        }
    }
    glUseProgram(p->prog);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, v);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
    glDrawArrays(GL_TRIANGLES, 0, nv);
}
static void bind_target(const target_t *t)
{
    glBindFramebuffer(GL_FRAMEBUFFER, t ? t->fbo : 0);
    glViewport(0, 0, t ? t->w : SW, t ? t->h : SH);
}
/* the bright parts, the wide glow, the glow and the speed blur in four targets; the final picture */
int gfx_blur_drawn;                                    /* (the last frame's screen passes had the speed blur: the GPU's clock) */
static void post_passes2(const gfx_post *post)
{
    int bloom = !GSW.no_bloom, speed = post && post->blur > 0.01f;
    gfx_blur_drawn = speed;
    glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    if (bloom || speed) {
        bind_target(&T_Q);
        tex_unit(0, T_SCENE.tex);
        if (bloom) {
            prog_t *p = &P[P_DOWN4]; glUseProgram(p->prog);
            glUniform4f(p->uTap, 1.0f / SW, 1.0f / SH, 0, 0); glUniform4f(p->uStep, 0, 0, gfx_bloom_thr, 0);
            rect_pass(p, &T_Q, 0, QW, 0, scene_us, 0, scene_vs);
            const float ue = (QW - 0.5f) / QW * scene_us;                  /* (the edge column again) */
            rect_pass(p, &T_Q, QW, QW + 8, ue, ue, 0, scene_vs);
        }
        if (speed) {
            prog_t *p = &P[P_COPY4]; glUseProgram(p->prog);
            glUniform4f(p->uTap, 1.0f / SW, 1.0f / SH, 0, 0);
            rect_pass(p, &T_Q, QC, TQW, 0, scene_us, 0, scene_vs);
            const float ue = 0.5f / QW * scene_us;
            rect_pass(p, &T_Q, QW + 8, QC, ue, ue, 0, scene_vs);
        }
        gt_mark_scene3d();                 /* (the first screen pass is under way: the 3D scene has been handed to the GPU) */
    }
    const float bq = (float)QW / TQW;                                      /* (the bright parts' share of T_Q's width) */
    if (bloom && !GSW.no_wide) {
        bind_target(&T_W);
        tex_unit(0, T_Q.tex);
        prog_t *p = &P[P_WIDE9]; glUseProgram(p->prog);
        glUniform4f(p->uTap, 1.0f / TQW, 1.0f / QH, 0, 0);
        rect_pass(p, &T_W, 0, QW / 4, 0, bq, 0, 1);
        const float ue = (QW / 4 - 0.5f) / (QW / 4) * bq;
        rect_pass(p, &T_W, QW / 4, QW / 4 + 2, ue, ue, 0, 1);
    }
    if (bloom || speed) {
        bind_target(&T_BB);
        if (bloom) {
            tex_unit(0, T_Q.tex); tex_unit(2, GSW.no_wide ? tex_black : T_W.tex);
            prog_t *p = &P[P_BLUR9]; glUseProgram(p->prog);
            glUniform4f(p->uTap, 1.0f / TQW, 1.0f / QH, 0, 0); glUniform4f(p->uStep, 0, 0, gfx_bloom_tune[0], gfx_bloom_tune[1]);
            rect_pass(p, &T_BB, 0, QW, 0, bq, 0, 1);
            const float ue = (QW - 0.5f) / TQW;
            rect_pass(p, &T_BB, QW, QW + 4, ue, ue, 0, 1);
            tex_unit(2, 0); glActiveTexture(GL_TEXTURE0);
        }
        if (speed) {
            tex_unit(0, T_Q.tex);
            prog_t *p = &P[P_RADIAL2]; glUseProgram(p->prog);
            glUniform4f(p->uStep, post->blur, 0, 0, 0);
            glUniform4f(p->uMap, bq, 1, (float)QC / TQW, 0);
            grid_rect(p, &T_BB, BR, TBW, 16, 0, 1);
            const float ue = 0.5f / QW;
            grid_rect(p, &T_BB, QW + 4, BR, 1, ue, ue);
        }
    }
    prog_t *p = &P[speed ? P_COMP_SPEED2 : P_COMP2];
    glUseProgram(p->prog);
    tex_unit(0, T_SCENE.tex);
    tex_unit(1, bloom ? T_BB.tex : tex_black);
    if (speed) tex_unit(3, T_BB.tex);
    float bs = post && post->bloom > 0 ? post->bloom : 1.0f;
    glUniform4f(p->uBloom, gfx_bloom_tune[0], gfx_bloom_tune[1], 0, 8.0f * gfx_bloom_tune[3] * bs);
    if (post) glUniform4fv(p->uEdge, 1, post->edge); else glUniform4f(p->uEdge, 0, 0, 0, 0);
    glUniform4f(p->uFx, post ? post->blur : 0, post ? post->flash : 0, post ? post->gain : 0, 0);
    if (p->uStep >= 0) glUniform4f(p->uStep, post ? post->blur : 0, 0, 0, 0);
    if (p->uScale >= 0) glUniform4f(p->uScale, scene_us, scene_vs, 0, 0);
    glUniform4f(p->uMap, (float)QW / TBW, 1, 0, 0);
    if (p->uMapR >= 0) glUniform4f(p->uMapR, (float)QW / TBW, 1, (float)BR / TBW, 0);
    grid_pass(p, NULL);                    /* (a grid: the edge tint is worked out per vertex) */
    gt_mark_offscreen();                   /* (the final pass is under way: every scene before it has been handed to the GPU) */
    if (speed) tex_unit(3, 0);
    tex_unit(1, 0); glActiveTexture(GL_TEXTURE0);
}
static void post_passes(const gfx_post *post)
{
    glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
    for (int i = 2; i < 5; i++) glDisableVertexAttribArray(i);
    if (!(gfx_dbg & 64)) {                 /* (gfx_dbg 64: the bench's old screen passes, to compare) */
        post_passes2(post);
#ifdef NR_PC
        if (fd_dump_post) { fd_dump_post = 0; fd_save(&T_Q, "q"); fd_save(&T_W, "w"); fd_save(&T_BB, "bb"); glBindFramebuffer(GL_FRAMEBUFFER, 0); }
#endif
        for (int i = 2; i < 5; i++) glEnableVertexAttribArray(i);
        glDepthMask(GL_TRUE);
        return;
    }
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
    gt_mark_offscreen();                   /* (the final pass is under way: every scene before it has been handed to the GPU) */
    if (speed) tex_unit(3, 0);
    tex_unit(1, 0); glActiveTexture(GL_TEXTURE0);
#ifdef NR_PC
    if (fd_dump_post) { fd_dump_post = 0; fd_save(&T_B[0], "b0"); fd_save(&T_B[1], "b1"); fd_save(&T_B[2], "b2"); fd_save(&T_B[3], "b3"); glBindFramebuffer(GL_FRAMEBUFFER, 0); }
#endif
    for (int i = 2; i < 5; i++) glEnableVertexAttribArray(i);
    glDepthMask(GL_TRUE);
}

void gfx_draw_frame(const level_rt *lv, v3 eye, v3 fwd, v3 upv, float fov_deg,
                    const gfx_obj *objs, int nobj, const gfx_sprite *spr, int nspr, const gfx_post *post)
{
    gfx_draws = gfx_tris = 0; gfx_inst_draws = gfx_inst_objs = 0; lod_k = 0;
    inst_seg = (inst_seg + 1) % INST_SEGS; inst_n = 0;      /* (this frame's part of the instances' ring) */
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
        gfx_scale_drawn = sc;
    } else gfx_scale_drawn = 1;
    cur_lv = lv; cur_eye = eye;
    if (lv) lighting_from(lv);
    glClearColor(lv ? fog_col[0] : 0, lv ? fog_col[1] : 0, lv ? fog_col[2] : 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#ifdef NR_PC
    if (gfx_fd_on && pp) { glClearStencil(0); glStencilMask(0xff); glClear(GL_STENCIL_BUFFER_BIT); }
#endif
    uint64_t tpc = gnow(), tps = tpc;
    if (lv && lv->nch) {
        /* the PC's projection (0x605590): the FOV is a mean of the two axes, fovY = 2 atan(tan(FOV/2) /
         * sqrt(aspect)); near 0.1, far SkyFar + 0.01 */
        const float fov = 2.0f * atanf(tanf(fov_deg * 3.14159265f / 360.0f) / sqrtf((float)SW / SH));
        const float zfar = (lv->d.light.sky_far > 50 ? lv->d.light.sky_far : 1600.0f) + 0.01f;
        /* the sky: a unit cube around the camera */
        build_vp(eye, fwd, upv, 1, fov, 0.05f, 100.0f);
        glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
        FD_PHASE("sky");
        draw_items(lv, collect(lv, eye, 0), 1);
        tps = gnow();
        /* the level, near to far */
        build_vp(eye, fwd, upv, 0, fov, 0.1f, zfar);
        build_planes();
        lod_k = sqrtf(VP[1][0] * VP[1][0] + VP[1][1] * VP[1][1] + VP[1][2] * VP[1][2]) * (SH * 0.5f);   /* (screen pixels) */
        /* the objects outside the view are left out (the caller only drops those behind or far off) */
        gfx_objs_drawn = gfx_objs_culled = 0;
        if (nobj > (int)sizeof ovis) nobj = (int)sizeof ovis;
        for (int i = 0; i < nobj; i++) { ovis[i] = (unsigned char)obj_in_view(&objs[i]); if (ovis[i]) gfx_objs_drawn++; else gfx_objs_culled++; }
#ifdef NR_PC
        if (gfx_fd_on == 2) {   /* (the sweep: the visible objects' triangles by their size on screen) */
            fd_small[0] = fd_small[1] = fd_small[2] = 0; memset(lt_tris, 0, sizeof lt_tris);
            float f = sqrtf(VP[1][0] * VP[1][0] + VP[1][1] * VP[1][1] + VP[1][2] * VP[1][2]);
            for (int i = 0; i < nobj; i++) if (ovis[i]) {
                const gfx_obj *o = &objs[i]; const level_rt *m = o->model;
                long t = 0; for (uint32_t c = 0; c < m->nch; c++) t += m->ch[c].ni / 3;
                float lc[3], h[3], c[3], r2 = 0;
                for (int k = 0; k < 3; k++) { lc[k] = 0.5f * (m->bmin[k] + m->bmax[k]); h[k] = 0.5f * (m->bmax[k] - m->bmin[k]); }
                for (int r = 0; r < 3; r++) c[r] = o->m[r][0] * lc[0] + o->m[r][1] * lc[1] + o->m[r][2] * lc[2] + o->m[r][3];
                for (int k = 0; k < 3; k++) r2 += (o->m[0][k] * o->m[0][k] + o->m[1][k] * o->m[1][k] + o->m[2][k] * o->m[2][k]) * h[k] * h[k];
                float w = VP[3][0] * c[0] + VP[3][1] * c[1] + VP[3][2] * c[2] + VP[3][3];
                float px = w > 0.1f ? sqrtf(r2) * f * (SH * 0.5f) / w : 1e9f;
                fd_small[px < 12 ? 0 : px < 40 ? 1 : 2] += t;
            }
        }
#endif
#ifdef NR_PC
        if (getenv("NOOBJCULL")) for (int i = 0; i < nobj; i++) ovis[i] = objs[i].model && objs[i].model->nch;   /* (the PC test machine: to compare) */
        if (getenv("OBJHIST")) {   /* (the PC test machine: the models drawn, once every 5 s) */
            static int fr; if (++fr % 300 == 0) {
                const level_rt *mk[64]; int mc[64], nm = 0, chunks = 0;
                for (int i = 0; i < nobj; i++) if (ovis[i]) {
                    int k = 0; while (k < nm && mk[k] != objs[i].model) k++;
                    if (k == nm && nm < 64) { mk[nm] = objs[i].model; mc[nm++] = 0; }
                    if (k < nm) mc[k]++;
                    chunks += objs[i].model->nch;
                }
                fprintf(stderr, "OBJHIST %d objects, %d chunks:", gfx_objs_drawn, chunks);
                for (int k = 0; k < nm; k++) if (mc[k] > 2) fprintf(stderr, " %s x%d (%u ch, %u tris)", mk[k]->name, mc[k], mk[k]->nch, mk[k]->nch ? mk[k]->ch[0].ni / 3 : 0);
                fprintf(stderr, "\n");
            }
        }
#endif
        tp1 = gnow();
        glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE);
        int n = collect(lv, eye, 1);
        qsort(items, n, sizeof(drawitem), by_prog_buf);
        if (!GSW.no_cull) glEnable(GL_CULL_FACE);
        FD_PHASE("level");
        if (!(gfx_dbg & 4)) draw_items(lv, n, 1);          /* (gfx_dbg 4: the bench leaves the level out) */
        tp2 = gnow(); SEC(1);
        FD_PHASE("objects");
        if (!(gfx_dbg & 16)) draw_objs(objs, nobj, ovis, 0);   /* (gfx_dbg 16 / 8: the bench) */
        glDisable(GL_CULL_FACE);
        if (gfx_hook_decal) {
            FD_PHASE("decals");
            gfx_hook_decal(eye, fwd, upv);
            glDisable(GL_BLEND); glDepthMask(GL_TRUE);
        }
        tp3 = gnow(); SEC(2);
        /* glowing things on top */
        glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE); glDepthMask(GL_FALSE);
        n = collect(lv, eye, 2);
        FD_PHASE("lvlglow");
        if (!(gfx_dbg & 1)) draw_items(lv, n, 1);
        tp4 = gnow(); SEC(3);
        FD_PHASE("objglow");
        if (!(gfx_dbg & 8) && (gfx_dbg & (2048 | 4096))) {
            /* (the bench: the ghost cars drawn after the rest, solid - no blending, depth written: the cost of their layers
             * over each other - or with their back faces left out) */
            static unsigned char *gv; static int gvn;
            if (gvn < nobj) { free(gv); gvn = nobj + 64; gv = malloc(gvn); }
            if (gv) {
                for (int i = 0; i < nobj; i++) gv[i] = ovis[i] && !gh_is_model(objs[i].model);
                draw_objs(objs, nobj, gv, 1);
                for (int i = 0; i < nobj; i++) gv[i] = ovis[i] && gh_is_model(objs[i].model);
                if (gfx_dbg & 2048) { glDisable(GL_BLEND); glDepthMask(GL_TRUE); } else glEnable(GL_CULL_FACE);
                draw_objs(objs, nobj, gv, 1);
                glEnable(GL_BLEND); glDepthMask(GL_FALSE); glDisable(GL_CULL_FACE);
            }
        } else if (!(gfx_dbg & 8)) draw_objs(objs, nobj, ovis, 1);
        tp5 = gnow(); SEC(4);
        FD_PHASE("sprites");
        draw_sprites(spr, nspr, fwd, upv);
        FD_PHASE("effects");
        if (gfx_hook_additive) gfx_hook_additive(eye, fwd, upv);
        glDisable(GL_BLEND); glDepthMask(GL_TRUE);
    }
    tp6 = gnow(); SEC(5);
#ifdef NR_PC
    if (gfx_fd_on && pp) fd_report();
#endif
    if (pp) { if (!gfx_kick_us) gfx_kick_us = gnow(); post_passes(post); }
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
        if (p >= P_DOWN && p <= P_COMP_SPEED2) continue;          /* (the screen passes: used every frame from the start) */
        for (int vr = 0; vr < 2 + NLV; vr++) {
        GLuint pg = vr >= 2 ? PLV[vr - 2][p].prog : vr ? PRB[p].prog : P[p].prog;
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
    glBlendEquation(GL_FUNC_ADD);
    if (inst_ok) {   /* the instanced programs the same way, drawn as they are in a race (the instances' rows from their ring) */
        static GLuint vb, ib; static const uint16_t ix3[3] = { 0, 1, 2 };
        if (!vb) {
            glGenBuffers(1, &vb); glGenBuffers(1, &ib);
            glBindBuffer(GL_ARRAY_BUFFER, vb); glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib); glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof ix3, ix3, GL_STATIC_DRAW);
        }
        for (int q = 0; q < P_COUNT * (1 + NLV); q++) {
            int p = q % P_COUNT; GLuint pg = q < P_COUNT ? PI[p].prog : PILV[q / P_COUNT - 1][p].prog;
            if (!pg) continue;
            glUseProgram(pg);
            for (int b = 0; b < 2; b++) {
                if (b == 0) glDisable(GL_BLEND); else { glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE); }
                glBindBuffer(GL_ARRAY_BUFFER, vb); vertex_layout(NULL);
                inst_attribs((const float (*)[12])inst_ring);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib);
                glDrawElementsInstanced(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, (void *)0, 1);
                inst_attribs_off();
                n++;
            }
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glBlendFunc(GL_ONE, GL_ONE);
    }
    glDisable(GL_BLEND); glDepthMask(GL_TRUE);
    for (int i = 2; i < 5; i++) glDisableVertexAttribArray(i);
    if (pp) {   /* the speed blur's passes too (only used once the car is fast: their first use was a 100 ms hitch) */
        gfx_post fake; memset(&fake, 0, sizeof fake); fake.blur = 0.5f;
        if (scene_us <= 0) scene_us = scene_vs = 1;
        glViewport(0, 0, SW, SH);
        post_passes(&fake);
        fake.blur = 0; post_passes(&fake);           /* (and without the speed blur) */
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, SW, SH);
    (void)n;
}
