/* The race camera: see camera.h. Function names and addresses are the PC's (docs/re/re_K.md). */
#include "camera.h"
#include <math.h>

static float kf(float k, float dt) { return 1.0f - powf(1.0f - k, 60.0f * dt); }   /* a per-frame lerp at 60 fps -> dt */
static v3 col(const nr_car *car, int i) { return v3make(car->rot[0][i], car->rot[1][i], car->rot[2][i]); }

/* LookAt (0x678120): z = from the target, x = up x z, y = z x x */
static void lookat(nr_cam *c, v3 pos, v3 target, v3 hint)
{
    c->pos = pos;
    v3 z = sub(pos, target); c->z = len(z) > 1e-6f ? norm(z) : v3make(0, -1, 0);
    v3 x = cross(hint, c->z); c->x = len(x) > 1e-6f ? norm(x) : v3make(1, 0, 0);
    c->y = cross(c->z, c->x);
}
/* the segment from the car to where the camera wants to be, against the level's static triangles
 * that face the car (0x63ba80; not moving objects); the camera sits 0.07 off a wall it hits */
static v3 cam_ray(nr_coll *coll, v3 P, v3 off)
{
    float L = len(off);
    if (L < 1e-4f || !coll || !coll->ntri) return add(P, off);
    v3 d = mul(off, 1.0f / L);
    float o[3] = { P.x, P.y, P.z }, dd[3] = { d.x, d.y, d.z }, t, n[3];
    coll->no_dyn++;
    int hit = nr_coll_ray(coll, o, dd, L, &t, n, NULL);
    coll->no_dyn--;
    return hit ? add(add(P, mul(d, t)), mul(v3make(n[0], n[1], n[2]), 0.07f)) : add(P, off);
}

/* Reset + snap (0x6f0b50 / 0x6efd60): behind the car along its up, looking at it */
void cam_reset_to(nr_cam *c, const nr_car *car, nr_coll *coll)
{
    v3 P = v3make(car->pos[0], car->pos[1], car->pos[2]), Y = col(car, 1), Z = col(car, 2);
    c->T = c->W = c->s = 0;
    c->up = Z;
    c->has_base = 0; c->lyaw = c->lpitch = c->lyaw_v = c->lpitch_v = 0; c->ldist = 1;
    float leash = 6 + c->added + 6 * c->lb, h = 1.2f + 0.7f * c->lb;
    v3 pos = cam_ray(coll, P, add(mul(Y, -leash), mul(Z, h)));
    lookat(c, pos, add(P, mul(Z, 1.2f)), Z);
}

/* the death and finish cameras (0x6f00c0 / 0x6ef740): where it is, turning to the target and
 * levelling its roll at 3/s, the FOV closing in on the target's spread */
void cam_track(nr_cam *c, v3 M, float spread, float dt)
{
    c->has_base = 0;
    v3 f = mul(c->z, -1), to = sub(M, c->pos);
    float dist = len(to);
    if (dist < 0.01f) return;
    float k = fminf(1, 3 * dt);
    v3 nf = norm(lerp3(f, mul(to, 1.0f / dist), k));
    v3 up = norm(lerp3(c->y, v3make(0, 0, 1), k));
    lookat(c, c->pos, add(c->pos, nf), up);
    float want = 1.5f * atanf((spread + 2) / dist) * 57.2958f;
    want = want < 10 ? 10 : want > 120 ? 120 : want;
    c->fov += (want - c->fov) * k;
}

/* the countdown's camera (BaseLevel state 1, 0x712dc3): round the warp-in stand-in at 6 + Added Camera Leash Distance
 * and 1.2 up, at the angle 0.75 x the clock (-6 .. 0: from its right past its front and left to behind it), in its
 * frame; pulled in to 0.99 of the way to a wall between; looking at it 1.2 above, world up. At 0 it is where the
 * chase camera's snap puts it */
void cam_intro(nr_cam *c, v3 P, const float R[3][3], float T, nr_coll *coll)
{
    float r = 6 + c->added, a = 0.75f * T;
    float l[3] = { r * sinf(a), -r * cosf(a), 1.2f };
    v3 off = v3make(R[0][0] * l[0] + R[0][1] * l[1] + R[0][2] * l[2], R[1][0] * l[0] + R[1][1] * l[1] + R[1][2] * l[2],
                    R[2][0] * l[0] + R[2][1] * l[1] + R[2][2] * l[2]);
    float L = len(off);
    if (L > 1e-4f && coll && coll->ntri) {
        float o[3] = { P.x, P.y, P.z }, d[3] = { off.x / L, off.y / L, off.z / L }, t, n[3];
        coll->no_dyn++;
        if (nr_coll_ray(coll, o, d, L, &t, n, NULL)) off = mul(off, t / L * 0.99f);
        coll->no_dyn--;
    }
    c->T = c->W = 0; c->up = v3make(0, 0, 1);
    c->has_base = 0; c->lyaw = c->lpitch = c->lyaw_v = c->lpitch_v = 0; c->ldist = 1;
    lookat(c, add(P, off), add(P, v3make(0, 0, 1.2f)), v3make(0, 0, 1));
}

/* SwitchCarCam (0x6eef50): Normal -> Far -> Bumper -> Normal */
void cam_next_view(nr_cam *c)
{
    if (c->bumper) { c->bumper = 0; c->far_view = 0; }
    else if (c->far_view) { c->far_view = 0; c->bumper = 1; }
    else c->far_view = 1;
}

/* ChaseCamera VUpdate, mode 1: Chase (0x6f0c10), the view blends, SpeedFov (0x6f09f0) */
static v3 rot_about(v3 v, v3 axis, float a)     /* right-handed Rodrigues (0x676f10) */
{
    float l = len(axis);
    if (l < 1e-6f || a == 0) return v;
    v3 k = mul(axis, 1.0f / l);
    float cs = cosf(a), sn = sinf(a);
    return add(add(mul(v, cs), mul(cross(k, v), sn)), mul(k, dot(k, v) * (1 - cs)));
}
/* a value eased towards a target as a critically damped spring (smooth both ways, at any frame rate): the time it
 * takes about st */
static float smooth_to(float x, float target, float *vel, float st, float dt)
{
    float w = 2.0f / st, e = w * dt, f = 1.0f / (1.0f + e + 0.48f * e * e + 0.235f * e * e * e);
    float d = x - target, t = (*vel + w * d) * dt;
    *vel = (*vel - w * t) * f;
    float o = target + (d + t) * f;
    if ((target - x > 0) == (o > target)) { o = target; *vel = 0; }     /* (no overshoot) */
    return o;
}
void cam_stick_shape(float rx, float ry, float *tx, float *ty)
{
    const float DZ = 0.15f;
    float m = sqrtf(rx * rx + ry * ry);
    if (m <= DZ) { *tx = *ty = 0; return; }
    float k = (m > 1 ? 1 : m) - DZ; k /= 1 - DZ;
    k = k * k * (1.5f - 0.5f * k);                       /* (a gentle start, full at the edge: 0.5 -> 0.31) */
    *tx = rx / m * k; *ty = -ry / m * k;
}
static void free_look(nr_cam *c, nr_coll *coll, v3 P, float tx, float ty, float dt);
void cam_chase_step(nr_cam *c, const nr_car *car, nr_coll *coll, float dt, float pitch_in, float deploy, int no_speed, float tx, float ty)
{
    v3 P0 = v3make(car->pos[0], car->pos[1], car->pos[2]), X = col(car, 0), Y = col(car, 1), Z = col(car, 2);
    /* the chase camera goes on from where it was itself (not where the free look turned the view to), and without the
     * stick: the PC's CamTurn moved it along the car's own axes (it swung about as the car rolled in the air) and by a
     * share of the frame (not of the time), and turned it one way on the ground and the other in the air */
    if (c->has_base) { c->pos = c->bpos; c->x = c->bx; c->y = c->by; c->z = c->bz; }
    c->tgt = add(P0, mul(Z, 1.2f));
    float r = 2.0f * dt;                                        /* Far and Bumper blend over 0.5 s */
    c->lb = c->far_view ? fminf(1, c->lb + r) : fmaxf(0, c->lb - r);
    c->bb = c->bumper ? fminf(1, c->bb + r) : fmaxf(0, c->bb - r);
    if (c->bb < 0.9f) {
        v3 P = add(P0, mul(Y, 5 * c->bb)), C = c->pos;
        float leash = 6 + c->added + 6 * c->lb, h = 1.2f + 0.7f * c->lb, fly = 10 + c->added + 6 * c->lb;
        v3 v = v3make(car->vel[0], car->vel[1], car->vel[2]), w = v3make(car->angvel[0], car->angvel[1], car->angvel[2]);
        float fwdV = dot(Y, v), latV = fabsf(dot(X, v)), upV = fabsf(dot(Z, v)), w2 = dot(w, w);
        /* the tumble factor: 0 on the ground, 1 (at once) when the car is slow, sliding or spinning in the air */
        if (car->grounded >= 3) c->T -= dt;
        else if (fwdV < 25 || latV > 20 || upV > 20 || w2 > 35) c->T = 1;
        else if (fwdV > 28 && latV < 10 && upV < 10 && w2 < 10) c->T -= dt;
        c->T = c->T < 0 ? 0 : c->T > 1 ? 1 : c->T;
        float k10 = kf(0.1f, dt);
        v3 CP = sub(C, P); float dist = len(CP);
        v3 D = dist > 1e-4f ? mul(CP, 1.0f / dist) : mul(Y, -1);
        float d = dist > fly ? fly : dist + (fly - dist) * k10;
        v3 D2 = sub(mul(D, 1 - k10), mul(Y, k10));            /* the fly camera swings in behind */
        float kp = fmaxf(0, fminf(1, 2 - 2 * fabsf(pitch_in)));
        v3 Uf = lerp3(c->y, v3make(0, 0, 1), kf(0.1f * kp, dt));
        c->W += ((fwdV < 2 ? 0 : deploy) - c->W) * k10;
        if (c->T < 0.9f) c->up = add(c->up, mul(sub(Z, c->up), (1 - c->T) * k10));
        else c->up = add(c->up, mul(sub(v3make(0, 0, 1), c->up), kf(0.03f, dt)));
        v3 up = c->up;
        v3 v2 = sub(P, C);
        float hgt = h;
        v3 flat = sub(v2, mul(up, dot(up, v2)));
        if (dot(v, v) > 1 && len(flat) > 1e-4f) {              /* swing round when the car drives at the camera */
            v3 nv = sub(v, mul(up, dot(up, v)));
            if (len(nv) > 1e-4f) {
                nv = norm(nv); v3 a = mul(norm(flat), -1);
                float dd = dot(a, nv);
                if (dd > 0.75f) { v3 cc = cross(a, up); float s = (dd - 0.75f) * dt * 10; flat = add(flat, dot(cc, nv) > 0 ? mul(cc, s) : mul(cc, -s)); }
            }
        }
        float FL = len(flat);
        v3 fN = FL > 1e-4f ? mul(flat, 1.0f / FL) : Y;
        float FL2 = FL < leash ? FL + (leash - FL) * k10 : leash, ratio = FL2 / leash;   /* a hard leash, eased out from below */
        float Lv = len(v2), L2 = Lv > leash ? Lv + (leash - Lv) * kf(0.3f, dt) : Lv + (leash - Lv) * k10;
        v3 n = Lv > 1e-4f ? mul(v2, 1.0f / Lv) : Y;
        float hl = sqrtf(n.x * n.x + n.y * n.y);
        if (fabsf(n.z) > 5 * hl)                                 /* no steeper than about 79 degrees */
            n = hl > 1e-6f ? norm(v3make(n.x / hl, n.y / hl, n.z > 0 ? 5.0f : -5.0f)) : norm(v3make(Y.x, Y.y, n.z > 0 ? 5.0f : -5.0f));
        v3 ground = add(mul(fN, -FL2), mul(up, hgt * ratio));
        v3 tumble = mul(n, -L2);
        v3 off = add(mul(add(mul(ground, 1 - c->T), mul(tumble, c->T)), 1 - c->W), mul(D2, c->W * d));
        v3 pos = cam_ray(coll, P, off);
        c->tgt = add(P, mul(up, 1.2f * ratio * (1 - c->W)));
        lookat(c, pos, c->tgt, add(mul(up, 1 - c->W), mul(Uf, c->W)));
    }
    if (c->bb > 0) {                                           /* Bumper: on the car's nose, turned as the car */
        v3 H = add(P0, add(mul(X, c->bump[0]), add(mul(Y, c->bump[1]), mul(Z, c->bump[2]))));
        c->pos = lerp3(c->pos, H, c->bb);
        v3 f = norm(lerp3(mul(c->z, -1), Y, c->bb)), u = norm(lerp3(c->y, Z, c->bb));
        lookat(c, c->pos, add(c->pos, f), u);
        c->tgt = lerp3(c->tgt, c->pos, c->bb);                   /* (the bumper view turns where it is) */
    }
    c->bpos = c->pos; c->bx = c->x; c->by = c->y; c->bz = c->z; c->has_base = 1;
    free_look(c, coll, add(P0, mul(Y, 5 * c->bb)), tx, ty, dt);
    /* SpeedFov: speed^2 from 4000 to 7000, in over 0.5 s, out over 0.25 s; FOV 60..75 (also the speed blur) */
    float q = car->vel[0] * car->vel[0] + car->vel[1] * car->vel[1] + car->vel[2] * car->vel[2];
    float t = no_speed ? 0 : q <= 4000 ? 0 : q >= 7000 ? 1 : (q - 4000) / 3000;
    float a = dt / (t > c->s ? 0.5f : 0.25f); if (a > 1) a = 1;
    c->s += (t - c->s) * a;
    c->fov = 60 + 15 * c->s;
}

/* the free look: the stick's direction is where the view turns to (all the way: 120 degrees round; the camera 40
 * degrees up, and down 12 on the ground, 35 flying), reached as a damped spring in about 0.15 s and eased back behind the car a little slower
 * (0.22 s) when the stick is let go. The chase camera's own frame is turned as one round what it looks at (about its
 * up, then its right), so it never rolls or flips however the car turns; a wall between what it looks at and the
 * camera pulls it in at once, and it eases back out */
static void free_look(nr_cam *c, nr_coll *coll, v3 P, float tx, float ty, float dt)
{
    (void)P;
    const float YAW = 2.094f, UP = 0.698f, DOWN = 0.21f + 0.40f * c->W;
    float ty_ = ty * (ty > 0 ? UP : DOWN), tyaw = tx * YAW;
    c->lyaw = smooth_to(c->lyaw, tyaw, &c->lyaw_v, fabsf(tyaw) < fabsf(c->lyaw) ? 0.22f : 0.15f, dt);
    c->lpitch = smooth_to(c->lpitch, ty_, &c->lpitch_v, fabsf(ty_) < fabsf(c->lpitch) ? 0.22f : 0.15f, dt);
    if (fabsf(c->lyaw) < 1e-4f && fabsf(c->lpitch) < 1e-4f) { c->lyaw = c->lpitch = c->lyaw_v = c->lpitch_v = 0; c->ldist = 1; return; }
    v3 pv = c->tgt, o = sub(c->pos, pv);
    v3 x = c->x, y = c->y, z = c->z;
    /* down first (about the camera's right: the view tips down as the camera rises), then round (about its up) */
    o = rot_about(o, x, -c->lpitch); y = rot_about(y, x, -c->lpitch); z = rot_about(z, x, -c->lpitch);
    o = rot_about(o, c->y, -c->lyaw); x = rot_about(x, c->y, -c->lyaw); y = rot_about(y, c->y, -c->lyaw); z = rot_about(z, c->y, -c->lyaw);
    float L = len(o), f = 1;
    if (L > 0.3f && coll && coll->ntri) {                /* a wall between what it looks at and the turned camera */
        v3 d = mul(o, 1.0f / L);
        float org[3] = { pv.x, pv.y, pv.z }, dd[3] = { d.x, d.y, d.z }, t, n[3];
        coll->no_dyn++;
        if (nr_coll_ray(coll, org, dd, L, &t, n, NULL)) f = fmaxf(0, t - 0.15f) / L;
        coll->no_dyn--;
    }
    c->ldist = f < c->ldist ? f : c->ldist + (f - c->ldist) * fminf(1, 6 * dt);   /* (in at once, out over about 0.3 s) */
    v3 pos = add(pv, mul(o, c->ldist));
    if (L * c->ldist > 0.5f) lookat(c, pos, pv, y);
    else { c->pos = pos; c->x = x; c->y = y; c->z = z; }    /* (the bumper view: turned where it is) */
}
