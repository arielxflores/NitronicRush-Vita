/* The car, ported from NitronicRush.exe:
 *   CarLogic::VUpdate (per frame)   0x709f90: inputs 0x708e70, leaning 0x700ee0, jump 0x707450 / 0x707140,
 *                                   wings 0x706880, boost 0x703af0, steering 0x7001f0, gas/brake 0x704510
 *   ControllerCar (per step)        0x62f6b0: wheel rays 0x62f250, suspension, friction 0x62d9a0,
 *                                   friction impulses 0x62ef10
 *   ControllerWings (per step)      0x62c240
 *   BodyRigid integration           0x639560 (inertia 0x639120, from the hull box 0x6304d0)
 * The body's collision with the level stands in for the game's convex hull with a few spheres. */
#include "nr_car.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross(const float *a, const float *b, float *o)
{
    float t[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
    memcpy(o, t, 12);
}
static float len3(const float *a) { return sqrtf(dot(a, a)); }
static void norm3(float *a) { float l = len3(a); if (l > 1e-9f) { a[0] /= l; a[1] /= l; a[2] /= l; } }
static void madd(float *o, const float *a, float s) { o[0] += a[0] * s; o[1] += a[1] * s; o[2] += a[2] * s; }
static void col(const nr_car *c, int j, float *o) { o[0] = c->rot[0][j]; o[1] = c->rot[1][j]; o[2] = c->rot[2][j]; }
static void to_world(const nr_car *c, const float *l, float *o)   /* car-space point -> world */
{
    for (int i = 0; i < 3; i++) o[i] = c->pos[i] + c->rot[i][0] * l[0] + c->rot[i][1] * l[1] + c->rot[i][2] * l[2];
}
static void dir_world(const nr_car *c, const float *l, float *o)
{
    for (int i = 0; i < 3; i++) o[i] = c->rot[i][0] * l[0] + c->rot[i][1] * l[1] + c->rot[i][2] * l[2];
}
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

/* ---- quaternions (x y z w) */
static void q_to_rot(const float *q, float r[3][3])
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    r[0][0] = 1 - 2 * (y * y + z * z); r[0][1] = 2 * (x * y - z * w);     r[0][2] = 2 * (x * z + y * w);
    r[1][0] = 2 * (x * y + z * w);     r[1][1] = 1 - 2 * (x * x + z * z); r[1][2] = 2 * (y * z - x * w);
    r[2][0] = 2 * (x * z - y * w);     r[2][1] = 2 * (y * z + x * w);     r[2][2] = 1 - 2 * (x * x + y * y);
}
static void rot_to_q(float r[3][3], float *q)
{
    float tr = r[0][0] + r[1][1] + r[2][2];
    if (tr > 0) {
        float s = sqrtf(tr + 1.0f) * 2;
        q[3] = 0.25f * s; q[0] = (r[2][1] - r[1][2]) / s; q[1] = (r[0][2] - r[2][0]) / s; q[2] = (r[1][0] - r[0][1]) / s;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        float s = sqrtf(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2;
        q[3] = (r[2][1] - r[1][2]) / s; q[0] = 0.25f * s; q[1] = (r[0][1] + r[1][0]) / s; q[2] = (r[0][2] + r[2][0]) / s;
    } else if (r[1][1] > r[2][2]) {
        float s = sqrtf(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2;
        q[3] = (r[0][2] - r[2][0]) / s; q[0] = (r[0][1] + r[1][0]) / s; q[1] = 0.25f * s; q[2] = (r[1][2] + r[2][1]) / s;
    } else {
        float s = sqrtf(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2;
        q[3] = (r[1][0] - r[0][1]) / s; q[0] = (r[0][2] + r[2][0]) / s; q[1] = (r[1][2] + r[2][1]) / s; q[2] = 0.25f * s;
    }
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; i++) q[i] /= l;
}
static void q_slerp(const float *a, const float *b0, float t, float *o)
{
    float b[4] = { b0[0], b0[1], b0[2], b0[3] };
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0) { d = -d; for (int i = 0; i < 4; i++) b[i] = -b[i]; }
    float ka, kb;
    if (d > 0.9995f) { ka = 1 - t; kb = t; }
    else { float th = acosf(d), s = sinf(th); ka = sinf((1 - t) * th) / s; kb = sinf(t * th) / s; }
    float l = 0;
    for (int i = 0; i < 4; i++) { o[i] = a[i] * ka + b[i] * kb; l += o[i] * o[i]; }
    l = sqrtf(l); for (int i = 0; i < 4; i++) o[i] /= l;
}
static void set_q(nr_car *c, const float *q) { memcpy(c->q, q, 16); q_to_rot(c->q, c->rot); }

/* ---- the triangles around each vehicle, gathered once per frame (the player's car and the CORE
 * drones each keep their own set) */
#define NEAR_SLOTS 12
static struct { const nr_car *car; const nr_coll *w; nr_coll_near nb; unsigned used; } NS[NEAR_SLOTS];
static unsigned NS_clock;
static int near_slot(const nr_car *c)
{
    int best = 0;
    for (int i = 0; i < NEAR_SLOTS; i++) {
        if (NS[i].car == c) { NS[i].used = ++NS_clock; return i; }
        if (NS[i].used < NS[best].used) best = i;
    }
    NS[best].car = c; NS[best].w = NULL; NS[best].used = ++NS_clock;
    return best;
}
static void gather_near(const nr_car *c, nr_coll *w, float margin)
{
    int k = near_slot(c);
    float r = 3.0f + margin, lo[3], hi[3];
    for (int i = 0; i < 3; i++) { lo[i] = c->pos[i] - r; hi[i] = c->pos[i] + r; }
    nr_coll_gather(w, lo, hi, &NS[k].nb); NS[k].w = w;
}
static void ensure_near(const nr_car *c, nr_coll *w)
{
    int k = near_slot(c);
    int ok = NS[k].w == w;
    for (int i = 0; i < 3 && ok; i++) if (c->pos[i] - 2.8f < NS[k].nb.lo[i] || c->pos[i] + 2.8f > NS[k].nb.hi[i]) ok = 0;
    if (!ok) gather_near(c, w, 0.5f + len3(c->vel) * 0.02f);
}

/* ---- parameters */
void nr_car_set_hull(nr_car_params *p, const float mn[3], const float mx[3])
{
    float h[3], ctr[3];
    for (int i = 0; i < 3; i++) { h[i] = (mx[i] - mn[i]) * 0.5f; ctr[i] = (mx[i] + mn[i]) * 0.5f; p->hull_min[i] = mn[i]; p->hull_max[i] = mx[i]; }
    /* ColliderConvexMesh: inverse inertia of the hull's box, 3 / (a^2 + b^2) with half extents */
    p->inv_inertia[0] = 3.0f / (h[1] * h[1] + h[2] * h[2]);
    p->inv_inertia[1] = 3.0f / (h[0] * h[0] + h[2] * h[2]);
    p->inv_inertia[2] = 3.0f / (h[0] * h[0] + h[1] * h[1]);
    /* spheres as tall as the hull, two abreast if it is wide, three along it */
    float r = fminf(h[2], h[0]);
    int nx = h[0] > r * 1.2f ? 2 : 1;
    p->nhull = 0;
    for (int iy = 0; iy < 3; iy++)
        for (int ix = 0; ix < nx; ix++) {
            float *s = p->hull[p->nhull++];
            s[0] = nx == 1 ? ctr[0] : (ix ? 1 : -1) * (h[0] - r) + ctr[0];
            s[1] = mn[1] + r + (mx[1] - mn[1] - 2 * r) * iy * 0.5f;
            s[2] = ctr[2];
            s[3] = r;
        }
}

void nr_car_default_params(nr_car_params *p)
{
    memset(p, 0, sizeof *p);
    static const float wp[4][3] = { { -0.718f, 1.027f, -0.243f }, { 0.718f, 1.027f, -0.243f },
                                    { -0.679f, -1.173f, -0.243f }, { 0.679f, -1.173f, -0.243f } };
    p->nwheels = 4; memcpy(p->wheel_pos, wp, sizeof wp);
    for (int i = 0; i < 4; i++) {
        p->wheel_steer[i] = i < 2 ? 0.45f : 0;
        p->wheel_drive[i] = i >= 2;
        p->wheel_brake[i] = i < 2 ? 0.004f : 0.016f;
        p->wheel_brake_max[i] = i < 2 ? 0.1f : 0.4f;
        p->fric_fs[i] = 2.5f; p->fric_fk[i] = 2.0f; p->fric_ss[i] = 3.5f; p->fric_sk[i] = 3.3f;
    }
    p->radius = 0.265f; p->spring_min = -0.08f; p->spring_max = 0.2f; p->spring_rest = 0.3f; p->spring_k = 30;
    p->max_spring_force = 6000; p->damp_comp = 5.3f; p->damp_relax = 3.3f;
    p->fwd_force = 20; p->rev_force = 15; p->side_roll = 0.1f; p->front_roll = 0.38f;
    p->drive_rest = -0.5f; p->roll_rest = 0.2f; p->drive_fric = 0; p->roll_fric = 0.7f;
    p->drag[0] = 1; p->drag[1] = 0.02f; p->drag[2] = 0.005f;
    p->rot_factor = 0.52f; p->inv_mass = 1; p->com[2] = -0.15f;
    p->jump_impulse = 15; p->brake_time = 0.2f; p->steer_red_start = 22; p->steer_red_factor = 0.2f; p->boost_force = 30;
    p->wing_pitch = 10; p->wing_roll = 10; p->wing_yaw = 8;
    p->gravity = 25;
    static const float hmn[3] = { -0.836f, -1.783f, -0.383f }, hmx[3] = { 0.836f, 1.770f, 0.483f };   /* CarHull.mdc */
    nr_car_set_hull(p, hmn, hmx);
}

int nr_car_load_params(nr_car_params *p, const char *cfg)
{
    int n = 0;
    const char *s = cfg;
    while (s && *s) {
        char key[96]; float v[48]; int nv = 0;
        const char *e = strchr(s, '\n'); if (!e) e = s + strlen(s);
        const char *sp = s; while (sp < e && *sp != ' ') sp++;
        int kl = (int)(sp - s); if (kl > 95) kl = 95;
        memcpy(key, s, kl); key[kl] = 0;
        const char *q = sp;
        while (q < e && nv < 48) { char *end; float f = strtof(q, &end); if (end == q) break; v[nv++] = f; q = end; }
        #define K(name, field) if (!strcmp(key, name) && nv >= 1) { p->field = v[0]; n++; }
        K("ControllerCar.Driving_Restitution", drive_rest) K("ControllerCar.Rolling_Restitution", roll_rest)
        K("ControllerCar.Driving_Friction", drive_fric) K("ControllerCar.Rolling_Friction", roll_fric)
        K("ControllerCar.Wheel_Friction_Side_Roll_Coefficient", side_roll)
        K("ControllerCar.Wheel_Friction_Front_Roll_Coefficient", front_roll)
        K("ControllerCar.Wheel_Radius", radius) K("ControllerCar.Spring_Minimum_Length", spring_min)
        K("ControllerCar.Spring_Maximum_Length", spring_max) K("ControllerCar.Spring_Rest_Length", spring_rest)
        K("ControllerCar.Spring_Strength", spring_k) K("ControllerCar.Max_Spring_Force", max_spring_force)
        K("ControllerCar.Spring_Damping_Compression", damp_comp) K("ControllerCar.Spring_Damping_Relaxation", damp_relax)
        K("ControllerCar.Forward_Force_Factor", fwd_force) K("ControllerCar.Reverse_Force_Factor", rev_force)
        K("BodyRigid.Rotational_Factor", rot_factor) K("BodyRigid.Inverse_Mass", inv_mass)
        K("CarLogic.Jump_Impulse", jump_impulse) K("CarLogic.Brake_Time", brake_time) K("CarLogic.Boost_Force", boost_force)
        K("CarLogic.Steer_Reduction_Start_Velocity", steer_red_start) K("CarLogic.Steer_Reduction_Factor", steer_red_factor)
        K("ControllerWings.Wing_Pitch_Speed", wing_pitch) K("ControllerWings.Wing_Roll_Speed", wing_roll)
        K("ControllerWings.Wing_Yaw_Speed", wing_yaw)
        #undef K
        if (!strcmp(key, "BodyRigid.Drag") && nv >= 3) { memcpy(p->drag, v, 12); n++; }
        if (!strcmp(key, "BodyRigid.Center_Of_Mass") && nv >= 3) { memcpy(p->com, v, 12); n++; }
        if (!strcmp(key, "ControllerCar.Wheels") && nv >= 10) {
            /* per wheel: steer factor, position, brake strength, max brake strength, front static and
             * kinetic friction, side static and kinetic friction. The drive wheels are the ones that
             * don't steer (true for every vehicle in the game) */
            p->nwheels = nv / 10 > 4 ? 4 : nv / 10;
            for (int w = 0; w < p->nwheels; w++) {
                const float *x = v + w * 10;
                p->wheel_steer[w] = x[0];
                memcpy(p->wheel_pos[w], x + 1, 12);
                p->wheel_brake[w] = x[4]; p->wheel_brake_max[w] = x[5];
                p->fric_fs[w] = x[6]; p->fric_fk[w] = x[7]; p->fric_ss[w] = x[8]; p->fric_sk[w] = x[9];
                p->wheel_drive[w] = x[0] == 0;
            }
            {   /* every wheel steers (the CORE drone's GunHumvee): all four drive (re_J 1) */
                int any = 0; for (int w = 0; w < p->nwheels; w++) any |= p->wheel_drive[w];
                if (!any) for (int w = 0; w < p->nwheels; w++) p->wheel_drive[w] = 1;
            }
            n++;
        }
        s = *e ? e + 1 : e;
    }
    return n;
}

void nr_car_reset(nr_car *c, const float pos[3], const float fwd_in[3])
{
    memset(c, 0, sizeof *c);
    float f[3] = { fwd_in[0], fwd_in[1], 0 }; norm3(f);
    if (len3(f) < 0.5f) { f[0] = 0; f[1] = 1; }
    float up[3] = { 0, 0, 1 }, r[3];
    cross(f, up, r); norm3(r);
    for (int i = 0; i < 3; i++) { c->rot[i][0] = r[i]; c->rot[i][1] = f[i]; c->rot[i][2] = up[i]; }
    rot_to_q(c->rot, c->q); q_to_rot(c->q, c->rot);
    memcpy(c->pos, pos, 12);
    c->pos[2] += 1.0f;
    for (int w = 0; w < 4; w++) { c->wheel_len[w] = 0.2f; c->wheel_slip[w] = 1; }
    c->mode_fwd = c->mode_rev = 1;
    c->cam_up[2] = 1; memcpy(c->cam_right, r, 12);
    { int k = near_slot(c); NS[k].w = NULL; }
}

void nr_car_reset_rot(nr_car *c, const float pos[3], const float R[3][3])
{
    float f[3] = { R[0][1], R[1][1], R[2][1] }, u[3] = { R[0][2], R[1][2], R[2][2] }, r[3];
    nr_car_reset(c, pos, f);
    norm3(u); if (len3(u) < 0.5f) { u[0] = 0; u[1] = 0; u[2] = 1; }
    madd(f, u, -dot(f, u)); norm3(f);
    if (len3(f) < 0.5f) return;
    cross(f, u, r); norm3(r);
    for (int i = 0; i < 3; i++) { c->rot[i][0] = r[i]; c->rot[i][1] = f[i]; c->rot[i][2] = u[i]; }
    rot_to_q(c->rot, c->q); q_to_rot(c->q, c->rot);
    memcpy(c->pos, pos, 12);
    memcpy(c->cam_up, u, 12); memcpy(c->cam_right, r, 12);
}

float nr_car_speed(const nr_car *c) { return len3(c->vel); }
float nr_car_forward_speed(const nr_car *c) { float f[3]; col(c, 1, f); return dot(c->vel, f); }
float nr_car_mph(const nr_car *c) { return fabsf(c->grounded >= 2 ? nr_car_forward_speed(c) : len3(c->vel)) * 2.33386f; }

/* ---- BodyRigid */
static void iinv_mul(const nr_car *c, const nr_car_params *p, const float v[3], float o[3])
{   /* world inverse inertia: R diag(hull) R^T * inverse mass * Rotational Factor */
    float l[3], k = p->inv_mass * p->rot_factor;
    for (int j = 0; j < 3; j++) l[j] = (c->rot[0][j] * v[0] + c->rot[1][j] * v[1] + c->rot[2][j] * v[2]) * p->inv_inertia[j] * k;
    dir_world(c, l, o);
}
static void apply_impulse(nr_car *c, const nr_car_params *p, const float J[3], const float r[3])
{
    madd(c->vel, J, p->inv_mass);
    float t[3], w[3]; cross(r, J, t); iinv_mul(c, p, t, w);
    c->angvel[0] += w[0]; c->angvel[1] += w[1]; c->angvel[2] += w[2];
}
static void apply_ang(nr_car *c, const nr_car_params *p, const float L[3])
{
    float w[3]; iinv_mul(c, p, L, w);
    c->angvel[0] += w[0]; c->angvel[1] += w[1]; c->angvel[2] += w[2];
}
static float impulse_denom(const nr_car *c, const nr_car_params *p, const float r[3], const float a[3])
{
    float rx[3], t[3]; cross(r, a, rx); iinv_mul(c, p, rx, t);
    return p->inv_mass + dot(rx, t);
}
static void point_vel(const nr_car *c, const float r[3], float o[3])
{
    cross(c->angvel, r, o); o[0] += c->vel[0]; o[1] += c->vel[1]; o[2] += c->vel[2];
}

/* ---- CarLogic, once per frame */
void nr_car_frame(nr_car *c, const nr_car_params *p, nr_coll *w, const nr_car_input *in, float dt)
{
    float right[3], fwd[3], up[3];
    col(c, 0, right); col(c, 1, fwd); col(c, 2, up);
    c->jumped = 0; c->body_touch = 0; c->ncontact = 0;
    if (dt <= 0) return;

    /* the stick as lean / wing input (0x708e70): the right stick rolls and flips the car; the left
     * stick leans it a little (and fully steers the wings) */
    float dep = clampf(c->deploy, 0, 1), lr, lp, ly;
    if (fabsf(in->roll_x) > 0.01f || fabsf(in->roll_y) > 0.01f) { lr = in->roll_x; lp = -in->roll_y; ly = 0; }
    else { lr = -in->steer * (1 - dep) * 0.223f; lp = in->pitch * ((1 - 0.316f) * dep + 0.316f); ly = in->steer; }
    c->lean_roll = lr; c->lean_pitch = lp; c->lean_yaw = ly;

    /* leaning without the wings (0x700ee0): rolls and flips, pushing the car onto its wheels */
    if (!in->wings) {
        float x = lr * lr * dt;
        if (lr < -0.05f || lr > 0.05f) { madd(c->vel, up, -25.0f * x); madd(c->angvel, fwd, (lr > 0 ? 25.225f : -25.225f) * x); }
        x = lp * lp * dt;
        if (lp > 0.05f || lp < -0.05f) { madd(c->vel, up, -50.0f * x); madd(c->angvel, right, (lp > 0 ? 13.475f : -13.475f) * x); }
    }

    /* jump (0x707450): off two wheels or more, or off a wall right in front of the nose (0x707140) */
    if (in->jump) {
        if (c->grounded >= 2) { madd(c->vel, up, p->jump_impulse * p->inv_mass); c->jumped = 1; }
        else if (w) {
            float t, n[3];
            if (nr_coll_ray(w, c->pos, fwd, 2.1f, &t, n, NULL)) { madd(c->vel, fwd, -p->jump_impulse * p->inv_mass); c->jumped = 1; }
        }
    }

    /* wings (0x706880): out in a third of a second; while they open the car turns to face where the
     * camera looks; open, it flies where its nose points */
    if (in->wings) {
        c->deploy = fminf(c->deploy + 3 * dt, 1);
        if (!c->wings_were_on) {
            float cu[3] = { c->cam_up[0], c->cam_up[1], c->cam_up[2] }, cr[3] = { c->cam_right[0], c->cam_right[1], c->cam_right[2] }, cf[3];
            norm3(cu); madd(cr, cu, -dot(cr, cu)); norm3(cr);
            cross(cu, cr, cf);
            float m[3][3];
            for (int i = 0; i < 3; i++) { m[i][0] = cr[i]; m[i][1] = cf[i]; m[i][2] = cu[i]; }
            if (len3(cr) > 0.5f) rot_to_q(m, c->wing_q); else memcpy(c->wing_q, c->q, 16);
        }
    } else c->deploy = fmaxf(c->deploy - 3 * dt, 0);
    c->wings_were_on = in->wings;
    if (in->wings && c->deploy > 0 && c->deploy < 1) {
        float q[4]; q_slerp(c->q, c->wing_q, c->deploy * 0.2f, q); set_q(c, q);
        float k = 0.9f * (1 - dt);
        for (int i = 0; i < 3; i++) c->angvel[i] *= k;
        col(c, 0, right); col(c, 1, fwd); col(c, 2, up);
    }
    if (c->deploy > 0.9f) { float s = len3(c->vel); for (int i = 0; i < 3; i++) c->vel[i] = fwd[i] * s; }

    /* boost (0x703af0): the jets run whenever the wings are out */
    float gas = clampf(in->throttle, -1, 1), brk = in->brake;
    c->boosting = in->wings || in->boost;
    if (c->boosting) {
        c->boost_ramp = fminf(c->boost_ramp + 2 * dt, 1);
        if (c->boost_ramp > 0.8f) madd(c->vel, fwd, p->boost_force * c->boost_ramp * dt * p->inv_mass);
        if (c->deploy < 0.5f) gas = 1;
    } else c->boost_ramp = fmaxf(c->boost_ramp - 2 * dt, 0);

    /* steering (0x7001f0): less lock with speed, turned at 5 per second */
    float vf = dot(c->vel, fwd);
    float red = fmaxf(vf - p->steer_red_start, 0) * p->steer_red_factor;
    float target = in->steer / (1 + red), d = target - c->steer, step = 5 * dt;
    c->steer = clampf(d > step ? c->steer + step : d < -step ? c->steer - step : target, -1, 1);

    /* gas, brake and reverse (0x704510): the brake reverses once the car has been nearly still for
     * Brake Time */
    c->throttle = 0; c->brake = 0;
    if (c->mode_fwd) {
        c->throttle = clampf(gas, -1, 1); c->brake = clampf(brk, 0, 1);
        if (gas > 0.2f) c->mode_rev = 0;
    }
    if (c->mode_rev) {
        c->throttle = clampf(-brk, -1, 1); c->brake = clampf(gas, 0, 1);
        if (brk > 0.2f) c->mode_fwd = 0;
    }
    if (dot(c->vel, c->vel) < 36) {
        c->rev_timer -= dt;
        if (c->rev_timer <= 0) { c->rev_timer = 0; c->mode_fwd = c->mode_rev = 1; }
    } else {
        c->rev_timer = p->brake_time;
        if (vf > 0.1f) { c->mode_fwd = 1; c->mode_rev = 0; }
        else if (vf < -0.1f) { c->mode_rev = 1; c->mode_fwd = 0; }
    }

    if (c->grounded == 0) c->air_time += dt; else c->air_time = 0;
    if (w) gather_near(c, w, 0.5f + len3(c->vel) * (dt > 0.05f ? dt : 0.05f));
}

/* ---- ControllerWings, every step */
static float WING_U[3], WING_K = 1; static int WING_N;
static void wings_step(nr_car *c, const nr_car_params *p, nr_coll *w, float dt)
{
    float dep = clampf(c->deploy, 0, 1);
    if (dep <= 0) { WING_N = 0; return; }
    float right[3], fwd[3], up[3];
    col(c, 0, right); col(c, 1, fwd); col(c, 2, up);
    for (int i = 0; i < 3; i++) c->angvel[i] *= 0.99f;
    /* level out against the camera's up, or against whatever is ahead within 25 m (so the car
     * lands on its wheels); the ray is cast at 50 Hz */
    if ((WING_N++ & 3) == 0) {
        float s = len3(c->vel), D[3];
        if (s < 30) memcpy(D, fwd, 12); else for (int i = 0; i < 3; i++) D[i] = c->vel[i] / s;
        memcpy(WING_U, c->cam_up, 12); WING_K = 1;
        if (len3(WING_U) < 0.5f) { WING_U[0] = WING_U[1] = 0; WING_U[2] = 1; }
        float t, n[3];
        if (w && nr_coll_ray(w, c->pos, D, 25.0f, &t, n, NULL)) { memcpy(WING_U, n, 12); WING_K = 1 + 0.2f * (25 - t); }
    }
    float U[3] = { WING_U[0], WING_U[1], WING_U[2] };
    madd(U, right, -c->lean_yaw * 0.5f); norm3(U);
    float ang = atan2f(-dot(right, U), dot(up, U));
    float L[3] = { 0, 0, 0 };
    madd(L, fwd, ang * (1 - fabsf(c->lean_roll)) * WING_K * dt * -5.0f); apply_ang(c, p, L);
    float pw = c->lean_pitch * fabsf(c->lean_pitch) * 0.5f;
    float cr[3] = { c->cam_right[0], c->cam_right[1], c->cam_right[2] }, cu[3] = { c->cam_up[0], c->cam_up[1], c->cam_up[2] };
    if (len3(cr) < 0.5f) memcpy(cr, right, 12);
    if (len3(cu) < 0.5f) memcpy(cu, up, 12);
    L[0] = L[1] = L[2] = 0; madd(L, cr, pw * p->wing_pitch * dt * dep * 2); apply_ang(c, p, L);
    L[0] = L[1] = L[2] = 0; madd(L, fwd, c->lean_roll * p->wing_roll * dt * dep); apply_ang(c, p, L);
    L[0] = L[1] = L[2] = 0; madd(L, cu, c->lean_yaw * p->wing_yaw * dt * dep * 2); apply_ang(c, p, L);
}

/* the level's surfaces for the body: CollisionMaterials.xml (friction, restitution) */
/* CollisionMaterials.xml: Default, Road, Building, Streetlight, Obsticle, Metal, Lava, spike */
static const float MAT_F[8] = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1.0f, 0.95f }, MAT_E[8] = { 0.5f, 0.3f, 0.5f, 0.5f, 0.5f, 0.5f, 0.1f, 0.05f };

void nr_car_step(nr_car *c, const nr_car_params *p, nr_coll *w, float dt)
{
    ensure_near(c, w);
    const nr_coll_near *NB = &NS[near_slot(c)].nb;
    float right[3], fwd[3], up[3], down[3], com[3];
    col(c, 0, right); col(c, 1, fwd); col(c, 2, up);
    down[0] = -up[0]; down[1] = -up[1]; down[2] = -up[2];
    to_world(c, p->com, com);

    /* ---- ControllerCar: wheel rays (0x62f250) */
    int contacts = 0, nw = p->nwheels;
    float cp[4][3], cn[4][3], rv[4], fs[4], sv[4][3];   /* sv: the ground's own velocity (moving objects) */
    for (int i = 0; i < nw; i++) {
        float a[3]; to_world(c, p->wheel_pos[i], a);
        float t, n[3]; uint32_t mat;
        if (nr_coll_ray_near(w, NB, a, down, p->spring_max + p->radius, &t, n, &mat)) {
            c->wheel_len[i] = clampf(t - p->radius, p->spring_min, p->spring_max);
            for (int k = 0; k < 3; k++) { cp[i][k] = a[k] + down[k] * t; cn[i][k] = n[k]; c->wheel_n[i][k] = n[k]; c->wheel_cp[i][k] = cp[i][k]; }
            float r[3] = { cp[i][0] - com[0], cp[i][1] - com[1], cp[i][2] - com[2] }, vp[3];
            point_vel(c, r, vp);
            nr_coll_point_vel(w, mat, cp[i], sv[i]);
            for (int k = 0; k < 3; k++) vp[k] -= sv[i][k];
            float den = dot(n, up);
            rv[i] = den <= 0.1f ? 0 : dot(n, vp) / den;
            c->wheel_contact[i] = 1; contacts++;
        } else {
            c->wheel_len[i] = fminf(c->wheel_len[i] + p->spring_max * dt * 5, p->spring_max);
            c->wheel_contact[i] = 0;
            if (p->wheel_drive[i]) c->wheel_spin[i] = (c->wheel_spin[i] - c->throttle * dt * 500) * 0.99f;
            else c->wheel_spin[i] *= 0.996f;
            c->wheel_spin[i] -= c->brake * c->wheel_spin[i] * dt * 0.1f;
            c->wheel_force[i] = 0; c->wheel_skid[i] = 0;
        }
    }
    c->grounded = contacts;

    /* suspension */
    for (int i = 0; i < nw; i++) {
        if (!c->wheel_contact[i]) continue;
        float f = (p->spring_rest - c->wheel_len[i]) * p->spring_k - (rv[i] < 0 ? p->damp_comp : p->damp_relax) * rv[i];
        fs[i] = c->wheel_force[i] = clampf(f, 0, p->max_spring_force);
    }
    for (int i = 0; i < nw; i++) {
        if (!c->wheel_contact[i]) continue;
        float J[3] = { cn[i][0] * fs[i] * dt, cn[i][1] * fs[i] * dt, cn[i][2] * fs[i] * dt };
        float r[3] = { cp[i][0] - com[0], cp[i][1] - com[1], cp[i][2] - com[2] };
        apply_impulse(c, p, J, r);
    }

    /* tyres (0x62d9a0): sideways grip, drive, brakes, and a friction ellipse that lets go into a
     * skid and catches again below the kinetic limit */
    float fI[4], sI[4], fdir[4][3], sdir[4][3];
    for (int i = 0; i < nw; i++) {
        if (!c->wheel_contact[i]) continue;
        float *n = cn[i];
        float sa = c->steer * p->wheel_steer[i];
        float lf[3] = { -sinf(sa), cosf(sa), 0 }, wf[3];
        dir_world(c, lf, wf);
        madd(wf, n, -dot(wf, n)); norm3(wf);
        float ws[3]; cross(wf, n, ws);
        memcpy(fdir[i], wf, 12); memcpy(sdir[i], ws, 12);
        float r[3] = { cp[i][0] - com[0], cp[i][1] - com[1], cp[i][2] - com[2] }, vp[3];
        point_vel(c, r, vp);
        for (int k = 0; k < 3; k++) vp[k] -= sv[i][k];
        float side = -dot(ws, vp) / impulse_denom(c, p, r, ws) * 0.2f;
        float jf = impulse_denom(c, p, r, wf), vlong = dot(wf, vp);
        c->wheel_spin[i] = -vlong / p->radius;
        float drive = 0;
        if (p->wheel_drive[i] && c->throttle != 0) drive = (c->throttle > 0 ? p->fwd_force : p->rev_force) * c->throttle * dt;
        if (c->brake != 0) {
            float x = -(p->wheel_brake[i] / jf) * vlong, k = c->brake;
            if (fabsf(x) > p->wheel_brake_max[i]) { float s = p->wheel_brake_max[i] / fabsf(x); x *= s; k = s * c->brake; }
            drive *= 1 - c->brake;
            if (fabsf(x) > fabsf(drive)) c->wheel_spin[i] *= 1 - k;
            drive += x;
        }
        float maxi = clampf(fs[i] * 2, 0, 10), slip = 1;
        if (maxi <= 0) { drive = side = 0; }
        else if (c->wheel_skid[i]) {
            float a = side / (p->fric_sk[i] * maxi * dt), b = drive / (p->fric_fk[i] * maxi * dt), q = a * a + b * b;
            if (q > 1 && q >= 1e-7f) { slip = 1 / sqrtf(q); side *= slip; drive *= slip; }
            else c->wheel_skid[i] = 0;
            c->wheel_spin[i] *= slip;
        } else {
            float a = side / (p->fric_ss[i] * maxi * dt), b = drive / (p->fric_fs[i] * maxi * dt), q = a * a + b * b;
            if (q > 1 && q >= 1e-7f) {
                float a2 = side / (p->fric_sk[i] * maxi * dt), b2 = drive / (p->fric_fk[i] * maxi * dt);
                slip = 1 / sqrtf(a2 * a2 + b2 * b2); side *= slip; drive *= slip;
                c->wheel_skid[i] = 1;
            }
        }
        c->wheel_slip[i] = slip;
        fI[i] = drive; sI[i] = side;
    }
    /* ... applied nearer the centre of mass's height (Front / Side Roll Coefficient); a front wheel
     * only grips while the rear wheel on its side is down too (0x62ef10) */
    for (int i = 0; i < nw; i++) {
        if (!c->wheel_contact[i]) continue;
        if (i < nw - 2 && !c->wheel_contact[i + 2]) continue;
        float r[3] = { cp[i][0] - com[0], cp[i][1] - com[1], cp[i][2] - com[2] };
        float h = dot(cn[i], r), rr[3], J[3];
        if (fI[i] != 0) {
            memcpy(rr, r, 12); madd(rr, cn[i], h * (p->front_roll - 1));
            for (int k = 0; k < 3; k++) J[k] = fdir[i][k] * fI[i];
            apply_impulse(c, p, J, rr);
        }
        if (sI[i] != 0) {
            memcpy(rr, r, 12); madd(rr, cn[i], h * (p->side_roll - 1));
            for (int k = 0; k < 3; k++) J[k] = sdir[i][k] * sI[i];
            apply_impulse(c, p, J, rr);
        }
    }
    if (contacts > 0) for (int i = 0; i < 3; i++) c->angvel[i] *= 0.985f;

    /* ---- ControllerWings */
    wings_step(c, p, w, dt);

    /* ---- BodyRigid integration (0x639560): move, turn about the centre of mass, gravity, drag */
    float c0[3], c1[3]; dir_world(c, p->com, c0);
    madd(c->pos, c->vel, dt);
    {
        float *q = c->q, wx = c->angvel[0] * dt * 0.5f, wy = c->angvel[1] * dt * 0.5f, wz = c->angvel[2] * dt * 0.5f;
        float nq[4] = { q[0] + wx * q[3] + wy * q[2] - wz * q[1],
                        q[1] + wy * q[3] + wz * q[0] - wx * q[2],
                        q[2] + wz * q[3] + wx * q[1] - wy * q[0],
                        q[3] - wx * q[0] - wy * q[1] - wz * q[2] };
        float l = sqrtf(nq[0] * nq[0] + nq[1] * nq[1] + nq[2] * nq[2] + nq[3] * nq[3]);
        for (int i = 0; i < 4; i++) nq[i] /= l;
        set_q(c, nq);
    }
    dir_world(c, p->com, c1);
    for (int i = 0; i < 3; i++) c->pos[i] += c0[i] - c1[i];
    c->vel[2] -= p->gravity * p->inv_mass * dt;
    {
        float s = len3(c->vel);
        if (s > 1e-6f) {
            float ns = s - (p->drag[0] + p->drag[1] * s + p->drag[2] * s * s) * dt;
            if (ns < 0) ns = 0;
            for (int i = 0; i < 3; i++) c->vel[i] *= ns / s;
        }
    }
    for (int i = 0; i < 3; i++) c->angvel[i] *= 0.995f;

    /* ---- the body against the level: the collider's material is Driving Restitution / Friction
     * while a wheel touches, Rolling Restitution / Friction in the air (combined with the surface's) */
    c->last_hit = 0;
    ensure_near(c, w);
    for (int h = 0; h < p->nhull; h++) {
        float cw[3], push[3], n[3]; uint32_t mat = 0;
        to_world(c, p->hull[h], cw);
        if (!nr_coll_sphere_near(w, NB, cw, p->hull[h][3], push, n, &mat)) continue;
        c->body_touch = 1;
        madd(c->pos, push, 1.0f);
        float comw[3]; to_world(c, p->com, comw);
        float r[3] = { cw[0] - n[0] * p->hull[h][3] - comw[0], cw[1] - n[1] * p->hull[h][3] - comw[1], cw[2] - n[2] * p->hull[h][3] - comw[2] };
        float vp[3], sv2[3], cpt[3] = { cw[0] - n[0] * p->hull[h][3], cw[1] - n[1] * p->hull[h][3], cw[2] - n[2] * p->hull[h][3] };
        point_vel(c, r, vp);
        nr_coll_point_vel(w, mat, cpt, sv2);
        for (int k = 0; k < 3; k++) vp[k] -= sv2[k];           /* relative to a moving object */
        float vn = dot(vp, n);
        {   /* remember the contact for the game's rules (one per surface, the hardest) */
            float vt[3] = { vp[0], vp[1], vp[2] }; madd(vt, n, -vn);
            int k = 0;
            while (k < c->ncontact && c->contact[k].mat != mat) k++;
            if (k == c->ncontact && k < 16) { c->ncontact++; c->contact[k].d = -1e9f; }
            if (k < 16 && -vn > c->contact[k].d) {
                c->contact[k].d = -vn; c->contact[k].slide = len3(vt); c->contact[k].mat = mat;
                memcpy(c->contact[k].n, n, 12); memcpy(c->contact[k].p, cpt, 12);
            }
        }
        if (vn >= 0) continue;
        int m = mat & 7;
        float e = (contacts ? p->drive_rest : p->roll_rest) * MAT_E[m]; if (e < 0) e = 0;
        float mu = (contacts ? p->drive_fric : p->roll_fric) * MAT_F[m];
        float jn = -(1 + e) * vn / impulse_denom(c, p, r, n), J[3] = { n[0] * jn, n[1] * jn, n[2] * jn };
        apply_impulse(c, p, J, r);
        if (mu > 0) {
            point_vel(c, r, vp);
            for (int k = 0; k < 3; k++) vp[k] -= sv2[k];
            float vt[3] = { vp[0], vp[1], vp[2] }; madd(vt, n, -dot(vp, n));
            float lt = len3(vt);
            if (lt > 1e-4f) {
                float t[3] = { vt[0] / lt, vt[1] / lt, vt[2] / lt };
                float jt = lt / impulse_denom(c, p, r, t);
                if (jt > mu * jn) jt = mu * jn;
                float Jt[3] = { -t[0] * jt, -t[1] * jt, -t[2] * jt };
                apply_impulse(c, p, Jt, r);
            }
        }
        if (-vn > c->last_hit) { c->last_hit = -vn; c->last_hull = h; c->last_mat = mat; memcpy(c->last_n, n, 12); }
    }
}

/* ---- automatic driver and respawning (tests and attract mode) */
void nr_car_place_on_route(nr_car *c, nr_ai *ai, const float (*path)[3], const float *plen, uint32_t n, int at)
{
    if (n < 2) return;
    uint32_t j = at >= 0 ? (uint32_t)at : nr_route_nearest(path, n, c->pos, 0, n);
    if (j + 1 >= n) j = n - 2;
    uint32_t k = j; while (k + 1 < n && plen[k] - plen[j] < 4) k++;
    float f[3] = { path[k][0] - path[j][0], path[k][1] - path[j][1], path[k][2] - path[j][2] };
    nr_car_reset(c, path[j], f);
    ai->idx = j;
}

uint32_t nr_route_nearest(const float (*path)[3], uint32_t n, const float p[3], uint32_t from, uint32_t window)
{
    uint32_t best = from; float bd = 1e30f;
    uint32_t end = from + window < n ? from + window : n;
    uint32_t beg = from > 20 ? from - 20 : 0;
    for (uint32_t i = beg; i < end; i++) {
        float d[3] = { path[i][0] - p[0], path[i][1] - p[1], path[i][2] - p[2] };
        float dd = dot(d, d);
        if (dd < bd) { bd = dd; best = i; }
    }
    return best;
}

void nr_ai_drive_world(nr_ai *ai, const nr_car *c, nr_coll *w, const float (*path)[3], const float *plen, uint32_t n, nr_car_input *out)
{
    nr_ai_drive(ai, c, path, plen, n, out);
    if (!w || c->grounded < 2) return;
    /* jump when a wall is coming up (the game marks these with yellow chevrons) */
    float fwd[3]; col(c, 1, fwd);
    float h[3] = { fwd[0], fwd[1], 0 }; norm3(h);
    float speed = len3(c->vel), reach = 6.0f + speed * 0.35f;
    float o[3] = { c->pos[0], c->pos[1], c->pos[2] + 0.4f }, t, nrm[3]; uint32_t mat;
    if (nr_coll_ray(w, o, h, reach, &t, nrm, &mat) && nrm[2] < 0.5f && nrm[0] * h[0] + nrm[1] * h[1] < -0.75f) out->jump = 1;   /* a wall facing us, not a bend's side wall */
    /* a gap ahead (no road under a point a little ahead): jump and boost across */
    float ahead[3] = { c->pos[0] + h[0] * (4 + speed * 0.25f), c->pos[1] + h[1] * (4 + speed * 0.25f), c->pos[2] + 2.0f };
    float dn[3] = { 0, 0, -1 };
    ai->boosting = 0;
    if (speed > 15 && !nr_coll_ray(w, ahead, dn, 12.0f, &t, nrm, &mat)) { out->jump = 1; out->boost = 1; ai->boosting = 1; }
}

void nr_ai_drive(nr_ai *ai, const nr_car *c, const float (*path)[3], const float *plen, uint32_t n, nr_car_input *out)
{
    memset(out, 0, sizeof *out);
    if (n < 2) return;
    ai->idx = nr_route_nearest(path, n, c->pos, ai->idx, 60);
    float speed = len3(c->vel);
    float look = 10.0f + speed * 0.5f;
    uint32_t t = ai->idx;
    while (t + 1 < n && plen[t] - plen[ai->idx] < look) t++;
    float to[3] = { path[t][0] - c->pos[0], path[t][1] - c->pos[1], path[t][2] - c->pos[2] };
    float right[3], fwd[3];
    col(c, 0, right); col(c, 1, fwd);
    float x = dot(to, right), y = dot(to, fwd);
    float ang = atan2f(x, y);                        /* > 0: target is to the right */
    out->steer = clampf(-ang * 2.5f, -1, 1);         /* steer > 0 turns left */
    /* slow for sharp bends ahead */
    float bend = 0;
    uint32_t a = ai->idx, b = t, cc = t;
    while (cc + 1 < n && plen[cc] - plen[t] < look) cc++;
    if (b > a && cc > b) {
        float d1[3] = { path[b][0] - path[a][0], path[b][1] - path[a][1], 0 }, d2[3] = { path[cc][0] - path[b][0], path[cc][1] - path[b][1], 0 };
        norm3(d1); norm3(d2); bend = 1 - dot(d1, d2);
    }
    float want = 60.0f - bend * 120.0f; if (want < 18) want = 18;
    if (fabsf(ang) > 0.6f) want = fminf(want, 20);
    out->throttle = speed < want ? 1.0f : 0.0f;
    out->brake = speed > want + 8 ? 1.0f : 0.0f;
    if (c->grounded == 0 && c->air_time > 0.1f && c->air_time < 1.5f && ai->boosting) out->boost = 1;
    /* in the air: keep the car level with the right stick */
    if (c->grounded == 0) {
        out->roll_x = clampf(right[2] * 3.0f, -1, 1);
        out->roll_y = clampf(fwd[2] * 2.0f, -1, 1);
    }
    if (plen[ai->idx] > ai->best) { ai->best = plen[ai->idx]; }
}

int nr_car_watchdog(nr_car *c, nr_ai *ai, const float (*path)[3], const float *plen, uint32_t n,
                    float minz, float dt, float *bad_time, const char **why, int strict)
{
    if (n < 2) return 0;
    uint32_t i = nr_route_nearest(path, n, c->pos, ai->idx, 80);
    float d[3] = { path[i][0] - c->pos[0], path[i][1] - c->pos[1], path[i][2] - c->pos[2] };
    float up[3]; col(c, 2, up);
    const char *bad = NULL;
    if (c->pos[2] < minz - 30) bad = "fell off the level";
    else if (dot(d, d) > 120.0f * 120.0f) bad = "left the track";
    else if (up[2] < 0.1f && c->grounded == 0 && len3(c->vel) < 5) bad = "upside down";
    else if (strict && len3(c->vel) < 1.5f) bad = "stuck";
    if (!bad) { *bad_time = 0; return 0; }
    *bad_time += dt;
    float limit = (bad[0] == 'f' || bad[0] == 'l') ? 0.0f : strict ? 2.0f : 3.0f;
    if (*bad_time <= limit) return 0;
    /* back on the route a little further along */
    uint32_t j = i;
    while (j + 1 < n && plen[j] - plen[i] < 15) j++;
    uint32_t k = j; while (k + 1 < n && plen[k] - plen[j] < 4) k++;
    float f[3] = { path[k][0] - path[j][0], path[k][1] - path[j][1], path[k][2] - path[j][2] };
    nr_car_reset(c, path[j], f);
    float fl[3]; col(c, 1, fl); madd(c->vel, fl, 20);
    ai->idx = j; ai->respawns++; *bad_time = 0;
    if (why) *why = bad;
    return 1;
}
