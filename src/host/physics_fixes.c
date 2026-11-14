/*
 * Two places where the lifted physics must keep the retail arithmetic: the human pelvis's crouch-height clamps, which
 * the x87 evaluated at the 24-bit precision the game selects for its physics, and the wave model's step, which must
 * not shrink with the frame time.
 */
#include "host.h"

#include "cheats.h"
#include "water_kernels.h"

#define PELVIC_IDOF_VA 0x004F3FB0u
#define WATER_INTEGRATE_VA 0x0060FC10u
#define PELVIS_COUNT 8u
#define PELVIS_USAGE 0x006CE290u
#define PELVIS_HUMAN 1u
#define PELVIS_STATE 0x006CF198u
#define PELVIS_STATE_STRIDE 0x300u
#define PELVIS_MOUTH (21u * 12u)
#define PELVIS_DATA 0x006CEAB8u
#define PELVIS_DATA_STRIDE 0xDCu
#define PELVIS_HEIGHT (14u * 4u)
#define PELVIS_ALIVE (49u * 4u)
#define PELVIS_BIOTAG 0x0067B0C0u
#define PELVIS_BIOTAG_STRIDE 0x100u
#define BIOTAG_BODY_Z (2u * 4u)
#define KNEE_OFFSET_VA 0x00647F58u
#define LOW_FACTOR_VA 0x0064D970u
#define HIGH_FACTOR_VA 0x0064B9B0u
#define FPU_PRECISION_MASK 0x0300u
#define WATER_ROOT 0x50u
#define WATER_REQUIRED 0x9Cu
#define WATER_RUNNING 0xA0u
#define WATER_RUNNING_RENDERED 2u
#define WAVE_STEP_MAX 0x1Cu

static recomp_func_t s_idof_lifted, s_integrate_lifted;

static float rdf(uint32_t va) { return *(const float*)ADDR(va); }
static double rdd(uint32_t va) { return *(const double*)ADDR(va); }
static void wrf(uint32_t va, float v) { *(float*)ADDR(va) = v; }

/* Pelvic_IDOF(me, dt, inputs), cdecl. The lifted body evaluates every x87 operation in double precision whatever
 * the control word, which leaves a height clamped to 0.09 * (h - 0.2) and stored as a float below that bound: the
 * clamp then fired on every 10 ms step and zeroed the rising speed, holding the player crouched. */
static void pelvic_idof(void) {
    uint32_t me = MEM32(g_esp + 4), mouth, data;
    float pos, vel;
    pelvis_mouth_t fixed;
    if (g_alias_entry || me >= PELVIS_COUNT || MEM32(PELVIS_USAGE + 4u * me) != PELVIS_HUMAN) {
        s_idof_lifted();
        return;
    }
    cheats_before_pelvis();
    mouth = PELVIS_STATE + PELVIS_STATE_STRIDE * me + PELVIS_MOUTH;
    data = PELVIS_DATA + PELVIS_DATA_STRIDE * me;
    pos = rdf(mouth);
    vel = rdf(mouth + 4);
    s_idof_lifted();
    if ((g_fpu_cw & FPU_PRECISION_MASK) || !(rdf(data + PELVIS_ALIVE) > 0.0f)) return;
    fixed = pelvis_mouth_clamps(pos, vel, rdf(data + PELVIS_HEIGHT),
                                rdf(PELVIS_BIOTAG + PELVIS_BIOTAG_STRIDE * me + BIOTAG_BODY_Z), rdd(KNEE_OFFSET_VA),
                                rdd(LOW_FACTOR_VA), rdd(HIGH_FACTOR_VA));
    wrf(mouth, fixed.pos);
    wrf(mouth + 4, fixed.vel);
}

/* CEntityWater::CPriv::Integrate (thiscall, no arguments): pWater2->Step(sRequireStep), then running = 2. */
static void water_integrate(void) {
    uint32_t water = g_ecx, root, sim;
    water_cadence_t c;
    if (g_alias_entry) {
        s_integrate_lifted();
        return;
    }
    root = MEM32(water + WATER_ROOT);
    sim = root ? MEM32(root) : 0;
    c = water_cadence(rdf(water + WATER_REQUIRED), sim ? rdf(sim + WAVE_STEP_MAX) : 0.0f);
    if (c.steps < 0) {
        s_integrate_lifted();
        return;
    }
    if (!c.steps) {
        wrf(water + WATER_REQUIRED, c.carry);
        MEM32(water + WATER_RUNNING) = WATER_RUNNING_RENDERED;
        g_esp += 4;
        return;
    }
    wrf(water + WATER_REQUIRED, (float)c.steps * c.step);
    s_integrate_lifted();
    wrf(water + WATER_REQUIRED, c.carry);
}

void physics_fixes_install(void) {
    if (!guest_override_install(PELVIC_IDOF_VA, pelvic_idof, &s_idof_lifted))
        host_log("host", "warning: the pelvis height clamps could not be hooked");
    if (!guest_override_install(WATER_INTEGRATE_VA, water_integrate, &s_integrate_lifted))
        host_log("host", "warning: the water's step pacing could not be hooked");
}
