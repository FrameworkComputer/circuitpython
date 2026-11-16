// Lilac power-rail interlocks and protection; see powerctl.h.
#include <errno.h>
#include <string.h>

#include "powerctl.h"

static const char *const out_names[PWR_OUT_COUNT] = {
    [PWR_SYS_PWRGD] = "sys_pwrgd_ec",
    [PWR_VR_ON] = "vr_on",
    [PWR_SSD] = "ssd_pwr_en",
    [PWR_SUSP] = "susp_l",
    [PWR_0P75VS] = "0p75vs_pwr_en",
    [PWR_SYSON] = "syson",
    [PWR_RSMRST] = "ec_soc_rsmrst_l",
    [PWR_PBTN] = "pbtn_out_l",
    [PWR_APU_AUD] = "apu_aud_pwr_en",
    [PWR_PCH] = "pch_pwr_en",
    [PWR_0P75_1P8VALW] = "0p75_1p8valw_pwren",
};

static const char *const fault_names[PWR_FAULT_COUNT] = {
    [PWR_FAULT_NONE] = "none",
    [PWR_FAULT_ALW_LOST] = "alw_lost",
    [PWR_FAULT_VR_TIMEOUT] = "vr_timeout",
    [PWR_FAULT_VR_LOST] = "vr_lost",
    [PWR_FAULT_THERMAL] = "thermal",
    [PWR_FAULT_LEASE] = "lease",
    [PWR_FAULT_HANG] = "hang",
    [PWR_FAULT_REQUEST] = "request",
    [PWR_FAULT_RELOAD] = "reload",
};

#define NONE (-1)

// Asserting an output needs every listed predecessor high for at least its
// delay and the listed input high for at least its delay (stock
// power_handle_state() order and k_msleep()s). 3VALW power good is required
// for every assertion (stock meant to wait for it but passes mask 0).
struct pre { int8_t out; uint16_t ms; };
struct rule { struct pre pre[2]; int8_t in; uint16_t in_ms; };
static const struct rule rules[PWR_OUT_COUNT] = {
    // G3S5
    [PWR_0P75_1P8VALW] = { { { NONE, 0 }, { NONE, 0 } }, PWR_IN_ALW_PG, 0 },
    [PWR_APU_AUD] = { { { PWR_0P75_1P8VALW, 10 }, { NONE, 0 } }, NONE, 0 },
    [PWR_PCH] = { { { PWR_APU_AUD, 10 }, { NONE, 0 } }, NONE, 0 },
    [PWR_PBTN] = { { { PWR_PCH, 10 }, { NONE, 0 } }, NONE, 0 },
    [PWR_RSMRST] = { { { PWR_PBTN, 10 }, { NONE, 0 } }, NONE, 0 },
    // S3S0: 10 ms after SLP_S3 high, SYSON; 20 ms; SUSP + 0.75VS; 20 ms;
    // VR_ON + SSD; VR power good; 10 ms; SYS_PWRGD.
    [PWR_SYSON] = { { { PWR_RSMRST, 0 }, { NONE, 0 } }, PWR_IN_SLP_S3, 10 },
    [PWR_SUSP] = { { { PWR_SYSON, 20 }, { NONE, 0 } }, PWR_IN_SLP_S3, 0 },
    [PWR_0P75VS] = { { { PWR_SYSON, 20 }, { NONE, 0 } }, PWR_IN_SLP_S3, 0 },
    [PWR_VR_ON] = { { { PWR_SUSP, 20 }, { PWR_0P75VS, 20 } }, PWR_IN_SLP_S3, 0 },
    [PWR_SSD] = { { { PWR_SUSP, 20 }, { PWR_0P75VS, 20 } }, PWR_IN_SLP_S3, 0 },
    [PWR_SYS_PWRGD] = { { { PWR_VR_ON, 0 }, { NONE, 0 } }, PWR_IN_VR_PG, 10 },
};

static const uint8_t s0_rails[] = { PWR_SYS_PWRGD, PWR_VR_ON, PWR_SSD, PWR_SUSP, PWR_0P75VS };

const char *powerctl_out_name(int out) {
    return (out >= 0 && out < PWR_OUT_COUNT) ? out_names[out] : NULL;
}

const char *powerctl_fault_name(int fault) {
    return (fault >= 0 && fault < PWR_FAULT_COUNT) ? fault_names[fault] : NULL;
}

int powerctl_out_index(const char *name) {
    for (int i = 0; i < PWR_OUT_COUNT; i++) {
        if (strcmp(name, out_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

void powerctl_init(struct powerctl *pc, bool armed, int64_t now) {
    memset(pc, 0, sizeof(*pc));
    pc->armed = armed;
    for (int i = 0; i < PWR_OUT_COUNT; i++) {
        pc->out_since[i] = now;
    }
    for (int i = 0; i < PWR_IN_COUNT; i++) {
        pc->in_since[i] = now;
    }
    pc->pbtn_release_ms = -1;
    pc->s0s3_due = -1;
    pc->s0s3_rail_due = -1;
    pc->s3s5_due = -1;
    pc->hang_due = -1;
}

bool powerctl_s0_peripherals(const struct powerctl *pc) {
    return pc->out[PWR_SYS_PWRGD] && pc->in[PWR_IN_SLP_S3];
}

bool powerctl_any_on(const struct powerctl *pc) {
    for (int i = 0; i < PWR_OUT_COUNT; i++) {
        if (pc->out[i]) {
            return true;
        }
    }
    return false;
}

static void put(struct powerctl *pc, int out, int level, int64_t now,
    struct powerctl_write *w, size_t *n) {
    if (pc->out[out] != level) {
        pc->out[out] = (uint8_t)level;
        pc->out_since[out] = now;
        if (out == PWR_VR_ON && level) {
            pc->vr_on_ms = now;
            pc->vr_seen = false;
        }
    }
    w[*n].out = (uint8_t)out;
    w[*n].level = (uint8_t)level;
    (*n)++;
}

void powerctl_inputs(struct powerctl *pc, const uint8_t in[PWR_IN_COUNT], int64_t now) {
    for (int i = 0; i < PWR_IN_COUNT; i++) {
        uint8_t level = in[i] ? 1 : 0;
        if (level == pc->in[i]) {
            continue;
        }
        pc->in[i] = level;
        pc->in_since[i] = now;
        if (i == PWR_IN_SLP_S5) {
            // Stock arms system_hang_detect once per S5->S3 transition.
            pc->hang_due = (level && pc->out[PWR_RSMRST]) ? now + PWR_HANG_MS : -1;
        }
    }
}

static bool held(const struct powerctl *pc, int out, int ms, int64_t now) {
    return pc->out[out] && now - pc->out_since[out] >= ms;
}

static bool input_held(const struct powerctl *pc, int in, int ms, int64_t now) {
    return pc->in[in] && now - pc->in_since[in] >= ms;
}

static bool powering_down(const struct powerctl *pc) {
    return pc->s0s3_due >= 0 || pc->s0s3_rail_due >= 0 || pc->s3s5_due >= 0;
}

int powerctl_request(struct powerctl *pc, int out, int level, int64_t now,
    struct powerctl_write *w, size_t *n) {
    *n = 0;
    if (out < 0 || out >= PWR_OUT_COUNT || (level != 0 && level != 1)) {
        return -EINVAL;
    }
    if (level) {
        if (!pc->armed) {
            return -EPERM;
        }
        if (pc->fault != PWR_FAULT_NONE) {
            return -EBUSY;
        }
        if (powering_down(pc)) {
            return -EBUSY;
        }
        const struct rule *r = &rules[out];
        if (!pc->in[PWR_IN_ALW_PG]) {
            return -EAGAIN;
        }
        for (int i = 0; i < 2; i++) {
            if (r->pre[i].out != NONE && !held(pc, r->pre[i].out, r->pre[i].ms, now)) {
                return -EAGAIN;
            }
        }
        if (r->in != NONE && !input_held(pc, r->in, r->in_ms, now)) {
            return -EAGAIN;
        }
        if (out == PWR_SYSON && !pc->in[PWR_IN_SLP_S5]) {
            return -EAGAIN;
        }
        if (out == PWR_SYS_PWRGD && !pc->vr_seen) {
            return -EAGAIN;  // VR power good must have been observed by tick()
        }
    }
    put(pc, out, level, now, w, n);
    if (out == PWR_PBTN) {
        pc->pbtn_release_ms = -1;
    }
    pc->lease_until = now + PWR_LEASE_MS;
    return 0;
}

int powerctl_pbtn_pulse(struct powerctl *pc, int64_t now,
    struct powerctl_write *w, size_t *n) {
    *n = 0;
    if (!pc->armed) {
        return -EPERM;
    }
    if (pc->fault != PWR_FAULT_NONE || powering_down(pc)) {
        return -EBUSY;
    }
    // Stock: RSMRST released, 90 ms, PBTN low for 20 ms, PBTN high.
    if (!held(pc, PWR_RSMRST, 90, now) || !pc->out[PWR_PBTN] || pc->pbtn_release_ms >= 0) {
        return -EAGAIN;
    }
    put(pc, PWR_PBTN, 0, now, w, n);
    pc->pbtn_release_ms = now + PWR_PBTN_PULSE_MS;
    pc->lease_until = now + PWR_LEASE_MS;
    return 0;
}

void powerctl_heartbeat(struct powerctl *pc, int64_t now) {
    pc->lease_until = now + PWR_LEASE_MS;
}

size_t powerctl_power_off(struct powerctl *pc, int64_t now, struct powerctl_write *w) {
    size_t n = 0;
    for (int i = 0; i < PWR_OUT_COUNT; i++) {
        put(pc, i, 0, now, w, &n);
    }
    pc->pbtn_release_ms = -1;
    pc->s0s3_due = -1;
    pc->s0s3_rail_due = -1;
    pc->s3s5_due = -1;
    pc->hang_due = -1;
    return n;
}

size_t powerctl_emergency(struct powerctl *pc, enum powerctl_fault reason, int64_t now,
    struct powerctl_write *w) {
    bool was_on = powerctl_any_on(pc);
    size_t n = powerctl_power_off(pc, now, w);
    if (was_on && pc->fault == PWR_FAULT_NONE) {
        pc->fault = reason;
        pc->fault_ms = now;
        pc->faults++;
    }
    return n;
}

int powerctl_clear_fault(struct powerctl *pc) {
    if (powerctl_any_on(pc)) {
        return -EBUSY;
    }
    pc->fault = PWR_FAULT_NONE;
    return 0;
}

static bool s0_rail_on(const struct powerctl *pc) {
    for (size_t i = 0; i < sizeof(s0_rails); i++) {
        if (pc->out[s0_rails[i]]) {
            return true;
        }
    }
    return false;
}

size_t powerctl_tick(struct powerctl *pc, int64_t now, struct powerctl_write *w) {
    size_t n = 0;
    if (!powerctl_any_on(pc)) {
        return 0;
    }

    // Protection: any of these shuts everything off at once and latches.
    enum powerctl_fault fault = PWR_FAULT_NONE;
    if (pc->out[PWR_VR_ON] && pc->in[PWR_IN_VR_PG]) {
        pc->vr_seen = true;
    }
    if (!pc->in[PWR_IN_ALW_PG]) {
        fault = PWR_FAULT_ALW_LOST;
    } else if (pc->thermal_halt) {
        fault = PWR_FAULT_THERMAL;
    } else if (now > pc->lease_until) {
        fault = PWR_FAULT_LEASE;
    } else if (pc->out[PWR_VR_ON] && !pc->vr_seen && now - pc->vr_on_ms >= PWR_VR_TIMEOUT_MS) {
        fault = PWR_FAULT_VR_TIMEOUT;
    } else if (pc->out[PWR_VR_ON] && pc->vr_seen && !pc->in[PWR_IN_VR_PG] && pc->in[PWR_IN_SLP_S3]) {
        fault = PWR_FAULT_VR_LOST;
    } else if (pc->hang_due >= 0 && now >= pc->hang_due) {
        pc->hang_due = -1;
        if (!pc->vw_ready && pc->in[PWR_IN_SLP_S5] && pc->out[PWR_RSMRST]) {
            fault = PWR_FAULT_HANG;  // stock would force off and reboot (<= 4x); no retry here
        }
    }
    if (fault != PWR_FAULT_NONE) {
        return powerctl_emergency(pc, fault, now, w);
    }

    // Power-button pulse release.
    if (pc->pbtn_release_ms >= 0 && now >= pc->pbtn_release_ms) {
        pc->pbtn_release_ms = -1;
        put(pc, PWR_PBTN, 1, now, w, &n);
    }

    // Follow the SoC down (stock POWER_S0 -> S0S3 and POWER_S3 -> S3S5).
    if (!pc->in[PWR_IN_SLP_S3] && s0_rail_on(pc) && pc->s0s3_due < 0 && pc->s0s3_rail_due < 0) {
        pc->s0s3_due = now + PWR_S0S3_DEBOUNCE_MS;
    }
    if (pc->s0s3_due >= 0 && now >= pc->s0s3_due) {
        pc->s0s3_due = -1;
        put(pc, PWR_SYS_PWRGD, 0, now, w, &n);
        put(pc, PWR_VR_ON, 0, now, w, &n);
        pc->s0s3_rail_due = now + PWR_S0S3_RAIL_MS;
    }
    if (pc->s0s3_rail_due >= 0 && now >= pc->s0s3_rail_due) {
        pc->s0s3_rail_due = -1;
        put(pc, PWR_SUSP, 0, now, w, &n);
        put(pc, PWR_0P75VS, 0, now, w, &n);
        put(pc, PWR_SSD, 0, now, w, &n);
    }
    if (!pc->in[PWR_IN_SLP_S5] && pc->out[PWR_SYSON] && !s0_rail_on(pc) &&
        pc->s0s3_due < 0 && pc->s0s3_rail_due < 0 && pc->s3s5_due < 0) {
        pc->s3s5_due = now + PWR_S3S5_DEBOUNCE_MS;
    }
    if (pc->s3s5_due >= 0 && now >= pc->s3s5_due) {
        pc->s3s5_due = -1;
        put(pc, PWR_SYSON, 0, now, w, &n);
    }
    return n;
}
