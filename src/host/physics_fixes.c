/*
 * Two places where the lifted physics must keep the retail arithmetic: the human pelvis's crouch-height clamps, which
 * the x87 evaluated at the 24-bit precision the game selects for its physics, and the wave model's step, which must
 * not shrink with the frame time. Above 1x Movement Speed the right hand follows Anne as at 1x (host_policy.h). Noclip
 * keeps only the vertical part of the contact responses Pelvic_IDOF reads (host_policy.h).
 */
#include "guest_va.h"
#include "host.h"

#include "cheats.h"
#include "perf_sections.h"
#include "physics_watch_policy.h"
#include "water_kernels.h"

#include <math.h>
#include <string.h>

#define PELVIC_IDOF_VA GVA(0x004F3FB0)
#define WATER_INTEGRATE_VA GVA(0x0060FC10)
#define PELVIS_COUNT 8u
#define PELVIS_USAGE GVA(0x006CE290)
#define PELVIS_HUMAN 1u
#define PELVIS_STATE GVA(0x006CF198)
#define PELVIS_STATE_STRIDE 0x300u
#define PELVIS_MOUTH (21u * 12u)
#define PELVIS_BODY 0u
#define PELVIS_RIGHT_HAND (25u * 12u)
#define DOF_BYTES 12u
#define DOF_VELOCITY 4u
#define PELVIS_DATA GVA(0x006CEAB8)
#define PELVIS_DATA_STRIDE 0xDCu
#define PELVIS_HEIGHT (14u * 4u)
#define PLAYER GVA(0x0066EF64)
#define PLAYER_HAND_PHASE 0xE28u
#define PLAYER_GRAB 0xE30u
#define CONTROL_ARM_URGENCY GVA(0x006D0A80)
#define PELVIS_HIP (13u * 4u)
#define PELVIS_SHOULDER (37u * 4u)
#define PELVIS_ARM (40u * 4u)
#define PELVIS_ALIVE (49u * 4u)
#define PELVIS_BIOTAG GVA(0x0067B0C0)
#define PELVIS_BIOTAG_STRIDE 0x100u
#define DOF_ACCELERATION 8u
#define NOCLIP_LOG_LIMIT 16u
#define BIOTAG_BODY_Z (2u * 4u)
#define KNEE_OFFSET_VA GVA(0x00647F58)
#define LOW_FACTOR_VA GVA(0x0064D970)
#define HIGH_FACTOR_VA GVA(0x0064B9B0)
#define FPU_PRECISION_MASK 0x0300u
#define WATER_ROOT 0x50u
#define WATER_REQUIRED 0x9Cu
#define WATER_RUNNING 0xA0u
#define WATER_RUNNING_RENDERED 2u
#define WAVE_STEP_MAX 0x1Cu

static recomp_func_t s_idof_lifted, s_integrate_lifted;
static hand_follow_t s_follow[PELVIS_COUNT];
static hand_carrier_t s_carrier[PELVIS_COUNT];

/* The hand's last offset from the body within reach, and the last steps' distances for the log of an escape. */
#define HAND_HISTORY 24u
#define HAND_ESCAPE_M 2.5f
#define HAND_REARM_M 1.8f
#define HAND_LOG_LIMIT 8u
static struct {
    float offset[3];
    int have_offset, escaping;
    float dist[HAND_HISTORY], hand_speed[HAND_HISTORY], body_speed[HAND_HISTORY], rise[HAND_HISTORY];
    unsigned next;
} s_hand[PELVIS_COUNT];
static unsigned s_escapes_logged, s_recoveries_logged;
static unsigned long long s_recoveries, s_passive_steps, s_active_steps;
static nc_memory_t s_noclip[PELVIS_COUNT];
static unsigned s_noclip_logged;

static float rdf(uint32_t va) { return *(const float*)ADDR(va); }

/* Three consecutive DOFs (x, y, z) of the pelvis state: value or velocity. */
static void read3(uint32_t dof, uint32_t part, float out[3]) {
    for (uint32_t i = 0; i < 3; i++) out[i] = rdf(dof + i * DOF_BYTES + part);
}

static void write3(uint32_t dof, uint32_t part, const float v[3]) {
    for (uint32_t i = 0; i < 3; i++) *(float*)ADDR(dof + i * DOF_BYTES + part) = v[i];
}
static double rdd(uint32_t va) { return *(const double*)ADDR(va); }
static void wrf(uint32_t va, float v) { *(float*)ADDR(va) = v; }

static float length3(const float a[3], const float b[3]) {
    float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

static float speed3(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

/* -1 for each when CPlayer cannot be read. */
static void player_hand(int* phase, int* grab) {
    uint32_t player = MEM32(PLAYER);
    *phase = *grab = -1;
    if (!player || !guest_readable(player + PLAYER_HAND_PHASE, PLAYER_GRAB + 4u - PLAYER_HAND_PHASE)) return;
    *phase = (int)MEM32(player + PLAYER_HAND_PHASE);
    *grab = (int)MEM32(player + PLAYER_GRAB);
}

static float hand_limit_of(uint32_t data) {
    return pw_hand_limit(rdf(data + PELVIS_HIP), rdf(data + PELVIS_SHOULDER), rdf(data + PELVIS_SHOULDER + 4u),
                         rdf(data + PELVIS_SHOULDER + 8u), rdf(data + PELVIS_ARM));
}

/* The steps before the hand went past HAND_ESCAPE_M, oldest first. */
static void log_escape(uint32_t me, float limit) {
    char line[900];
    int n = 0;
    for (unsigned i = 0; i < HAND_HISTORY && n >= 0 && (size_t)n + 48 < sizeof line; i++) {
        unsigned k = (s_hand[me].next + i) % HAND_HISTORY;
        n += _snprintf(line + n, sizeof line - 1 - n, "%s%.2f/%.1f/%.1f/%.1f", i ? " " : "", s_hand[me].dist[k],
                       s_hand[me].hand_speed[k], s_hand[me].body_speed[k], s_hand[me].rise[k]);
    }
    line[sizeof line - 1] = 0;
    host_log("perf-detail", "hand-escape: pelvis %u, hand limit %.3f m, Movement Speed %.1fx; last %u steps "
             "as distance m / hand speed / body speed m/s / hand height over the body m: %s", me, limit,
             cheats_speed_factor(), HAND_HISTORY, line);
}

/* After Pelvic_IDOF: a hand beyond any reach goes back beside the body with the body's speed (hand_recovery_needed). */
static void watch_hand(uint32_t me, uint32_t state, uint32_t data) {
    float body_pos[3], body_vel[3], hand_pos[3], hand_vel[3], stored_vel[3], before[3], dist, limit = hand_limit_of(data);
    unsigned k = s_hand[me].next;
    int phase, grab;
    read3(state + PELVIS_BODY, 0, body_pos);
    read3(state + PELVIS_BODY, DOF_VELOCITY, body_vel);
    read3(state + PELVIS_RIGHT_HAND, 0, hand_pos);
    read3(state + PELVIS_RIGHT_HAND, DOF_VELOCITY, stored_vel);
    hand_follow_world_velocity(&s_follow[me], stored_vel, hand_vel);
    dist = length3(hand_pos, body_pos);
    s_hand[me].dist[k] = dist;
    s_hand[me].hand_speed[k] = speed3(hand_vel);
    s_hand[me].body_speed[k] = speed3(body_vel);
    s_hand[me].rise[k] = hand_pos[2] - body_pos[2];
    s_hand[me].next = (k + 1) % HAND_HISTORY;
    if (dist <= limit) {
        for (int i = 0; i < 3; i++) s_hand[me].offset[i] = hand_pos[i] - body_pos[i];
        s_hand[me].have_offset = 1;
    }
    if (dist < HAND_REARM_M) s_hand[me].escaping = 0;
    if (dist > HAND_ESCAPE_M && !s_hand[me].escaping) {
        s_hand[me].escaping = 1;
        if (s_escapes_logged < HAND_LOG_LIMIT) {
            s_escapes_logged++;
            log_escape(me, limit);
        }
    }
    if (!hand_recovery_needed(dist, limit)) return;
    memcpy(before, hand_pos, sizeof before);
    hand_rest_position(body_pos, s_hand[me].offset, s_hand[me].have_offset, hand_pos);
    write3(state + PELVIS_RIGHT_HAND, 0, hand_pos);
    hand_follow_rebase(&s_follow[me], body_pos, body_vel, stored_vel);
    s_carrier[me].valid = 0;
    write3(state + PELVIS_RIGHT_HAND, DOF_VELOCITY, stored_vel);
    for (uint32_t i = 0; i < 3; i++) wrf(state + PELVIS_RIGHT_HAND + i * DOF_BYTES + DOF_ACCELERATION, 0.0f);
    s_recoveries++;
    if (s_recoveries_logged >= HAND_LOG_LIMIT) return;
    s_recoveries_logged++;
    player_hand(&phase, &grab);
    host_log("physics-watch", "hand-recovery: distance %.3f m (hand limit %.3f), body %.2f %.2f %.2f, hand "
             "%.2f %.2f %.2f, hand velocity %.2f %.2f %.2f, phase %d, grab %d, arm urgency %.2f, %s, reason %s; hand "
             "now %.2f %.2f %.2f", dist, limit, body_pos[0], body_pos[1], body_pos[2], before[0], before[1], before[2],
             hand_vel[0], hand_vel[1], hand_vel[2], phase, grab, rdf(CONTROL_ARM_URGENCY),
             s_follow[me].passive ? "passive (riding the pelvis)" : "controlled",
             isfinite(dist) ? "beyond any reach" : "not finite", hand_pos[0], hand_pos[1], hand_pos[2]);
}

void physics_fixes_summary(void) {
    host_log("perf-detail", "hand: %llu passive steps (riding the pelvis), %llu controlled; %u escapes "
             "past %.1f m logged, %llu safety recoveries (expected 0)", s_passive_steps, s_active_steps,
             s_escapes_logged, HAND_ESCAPE_M, s_recoveries);
}

/* Before Pelvic_IDOF: each part's contact response of this step loses its horizontal part (nc_part_dof). */
static void noclip_before(uint32_t me, uint32_t state, int alive) {
    nc_memory_t* m = &s_noclip[me];
    uint32_t biotag = PELVIS_BIOTAG + PELVIS_BIOTAG_STRIDE * me;
    if (!cheats_noclip() || !alive) {
        m->valid = 0;
        s_noclip_logged = 0;
        return;
    }
    for (unsigned i = 0; i < NC_PARTS; i++) {
        uint32_t dof = state + nc_part_dof[i] * DOF_BYTES + DOF_VELOCITY, tag = biotag + nc_part_dof[i] * 4u;
        float response[3] = { rdf(tag), rdf(tag + 4), rdf(tag + 8) }, vx = rdf(dof), vy = rdf(dof + DOF_BYTES);
        float kept_x = m->valid ? nc_horizontal_velocity(m->vx[i], vx) : vx;
        float kept_y = m->valid ? nc_horizontal_velocity(m->vy[i], vy) : vy;
        if (s_noclip_logged < NOCLIP_LOG_LIMIT && (kept_x != vx || kept_y != vy || response[0] != 0.0f || response[1] != 0.0f)) {
            s_noclip_logged++;
            host_log("perf-detail", "noclip part DOF %u: contact xy %.3f %.3f z %.3f, speed %.3f %.3f -> %.3f %.3f",
                     nc_part_dof[i], response[0], response[1], response[2], vx, vy, kept_x, kept_y);
        }
        nc_vertical_response(response);
        wrf(tag, response[0]);
        wrf(tag + 4, response[1]);
        wrf(dof, kept_x);
        wrf(dof + DOF_BYTES, kept_y);
    }
}

/* After Pelvic_IDOF: the speed each part will start the next step with, if no contact touches it. */
static void noclip_after(uint32_t me, uint32_t state, float dt) {
    nc_memory_t* m = &s_noclip[me];
    if (!cheats_noclip()) return;
    for (unsigned i = 0; i < NC_PARTS; i++) {
        uint32_t dof = state + nc_part_dof[i] * DOF_BYTES + DOF_VELOCITY;
        m->vx[i] = nc_next_velocity(rdf(dof), rdf(dof + DOF_ACCELERATION - DOF_VELOCITY), dt);
        m->vy[i] = nc_next_velocity(rdf(dof + DOF_BYTES), rdf(dof + DOF_BYTES + DOF_ACCELERATION - DOF_VELOCITY), dt);
    }
    m->valid = 1;
}

/* Pelvic_IDOF(me, dt, inputs), cdecl. The lifted body evaluates every x87 operation in double precision whatever
 * the control word, which leaves a height clamped to 0.09 * (h - 0.2) and stored as a float below that bound: the
 * clamp then fired on every 10 ms step and zeroed the rising speed, holding the player crouched. */
static void pelvic_idof(void) {
    uint32_t me = MEM32(g_esp + 4), state, mouth, data;
    float pos, vel, k, dt = *(const float*)ADDR(g_esp + 8), body_pos[3], body_vel[3], hand_pos[3], hand_vel[3];
    pelvis_mouth_t fixed;
    int64_t begin;
    int phase, grab, passive;
    if (g_alias_entry || me >= PELVIS_COUNT || MEM32(PELVIS_USAGE + 4u * me) != PELVIS_HUMAN) {
        if (g_alias_entry || me >= PELVIS_COUNT) {
            s_idof_lifted();
            return;
        }
        s_follow[me].valid = s_follow[me].passive = 0;
        s_carrier[me].valid = 0;
        s_hand[me].have_offset = 0;
        begin = perf_section_begin();
        s_idof_lifted();
        perf_section_end(PS_PHYSICS_ANIMAL_IDOF, begin);
        return;
    }
    begin = perf_section_begin();
    cheats_before_pelvis();
    state = PELVIS_STATE + PELVIS_STATE_STRIDE * me;
    mouth = state + PELVIS_MOUTH;
    data = PELVIS_DATA + PELVIS_DATA_STRIDE * me;
    noclip_before(me, state, rdf(data + PELVIS_ALIVE) > 0.0f);
    pos = rdf(mouth);
    vel = rdf(mouth + 4);
    player_hand(&phase, &grab);
    passive = rdf(data + PELVIS_ALIVE) > 0.0f && hand_passive(phase, grab, rdf(CONTROL_ARM_URGENCY));
    if (passive) s_passive_steps++;
    else s_active_steps++;
    read3(state + PELVIS_BODY, 0, body_pos);
    read3(state + PELVIS_BODY, DOF_VELOCITY, body_vel);
    read3(state + PELVIS_RIGHT_HAND, 0, hand_pos);
    read3(state + PELVIS_RIGHT_HAND, DOF_VELOCITY, hand_vel);
    k = rdf(data + PELVIS_ALIVE) > 0.0f && !passive ? hand_frame_share(cheats_speed_factor()) : 0.0f;
    if (passive || s_follow[me].passive) hand_follow_enter(&s_follow[me], passive, body_pos, body_vel, hand_pos, hand_vel);
    hand_carrier_step(&s_carrier[me], k, dt, body_pos, body_vel, hand_pos, hand_vel);
    if (s_carrier[me].valid) hand_damping_shift(k, 1.0f, body_vel, hand_vel);
    write3(state + PELVIS_RIGHT_HAND, 0, hand_pos);
    write3(state + PELVIS_RIGHT_HAND, DOF_VELOCITY, hand_vel);
    s_idof_lifted();
    if (s_follow[me].passive || s_carrier[me].valid) {
        read3(state + PELVIS_RIGHT_HAND, DOF_VELOCITY, hand_vel);
        if (s_follow[me].passive) hand_follow_leave(&s_follow[me], hand_vel);
        else hand_damping_shift(k, -1.0f, body_vel, hand_vel);
        write3(state + PELVIS_RIGHT_HAND, DOF_VELOCITY, hand_vel);
    }
    noclip_after(me, state, dt);
    if (rdf(data + PELVIS_ALIVE) > 0.0f) watch_hand(me, state, data);
    if (!(g_fpu_cw & FPU_PRECISION_MASK) && rdf(data + PELVIS_ALIVE) > 0.0f) {
        fixed = pelvis_mouth_clamps(pos, vel, rdf(data + PELVIS_HEIGHT),
                                    rdf(PELVIS_BIOTAG + PELVIS_BIOTAG_STRIDE * me + BIOTAG_BODY_Z),
                                    rdd(KNEE_OFFSET_VA), rdd(LOW_FACTOR_VA), rdd(HIGH_FACTOR_VA));
        wrf(mouth, fixed.pos);
        wrf(mouth + 4, fixed.vel);
    }
    perf_section_end(PS_PHYSICS_HUMAN_IDOF, begin);
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
