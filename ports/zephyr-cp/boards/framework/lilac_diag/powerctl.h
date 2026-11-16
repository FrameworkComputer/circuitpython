// Lilac AP power-rail interlocks and protection, free of Zephyr so it can be
// compiled and tested on the host. power.c owns pins, timers and locking.
//
// Mechanism and hard limits only (PLAN.md section 1): Python sequences power
// up by requesting outputs; this code refuses any assertion whose stock
// predecessor, minimum delay or power-good is missing, follows the SoC's
// sleep signals down with stock ordering on its own, and shuts everything
// off (latched, no retry) on a fault. Reference: ec-lilac 6bcd05ac
// lilac/src/power_sequence.c.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Outputs, in stock chipset_force_g3() order (emergency shutdown order);
// SSD power, which force_g3 omits, after VR_ON. All active high; PBTN high
// is "released" (pbtn_out_l).
enum powerctl_out {
    PWR_SYS_PWRGD,
    PWR_VR_ON,
    PWR_SSD,
    PWR_SUSP,
    PWR_0P75VS,
    PWR_SYSON,
    PWR_RSMRST,
    PWR_PBTN,
    PWR_APU_AUD,
    PWR_PCH,
    PWR_0P75_1P8VALW,
    PWR_OUT_COUNT,
};

enum powerctl_in {
    PWR_IN_ALW_PG,   // pwr_3v5v_pg (3VALW power good)
    PWR_IN_VR_PG,    // apu_core_pwrgd
    PWR_IN_SLP_S3,   // slp_s3_l
    PWR_IN_SLP_S5,   // slp_s5_l
    PWR_IN_COUNT,
};

enum powerctl_fault {
    PWR_FAULT_NONE,
    PWR_FAULT_ALW_LOST,    // 3VALW power good low with rails up
    PWR_FAULT_VR_TIMEOUT,  // VR_ON high, VR power good not within 1 s
    PWR_FAULT_VR_LOST,     // VR power good lost after it was seen
    PWR_FAULT_THERMAL,     // native thermal halt latched
    PWR_FAULT_LEASE,       // Python stopped refreshing its lease
    PWR_FAULT_HANG,        // eSPI VW channel not ready 5 s after SLP_S5 high
    PWR_FAULT_REQUEST,     // Python requested an emergency shutdown
    PWR_FAULT_RELOAD,      // Python reloaded with rails up
    PWR_FAULT_COUNT,
};

// Stock delays and deadlines (ms).
#define PWR_VR_TIMEOUT_MS 1000   // power_wait_signals() DEFAULT_TIMEOUT
#define PWR_HANG_MS 5000         // system_hang_detect deferred by 5 s
#define PWR_S0S3_DEBOUNCE_MS 5   // POWER_S0: k_msleep(5) before S0S3
#define PWR_S0S3_RAIL_MS 85      // S0S3: VR_ON off, 85 ms, then SUSP etc.
#define PWR_S3S5_DEBOUNCE_MS 55  // POWER_S3: k_msleep(55) before S3S5
#define PWR_PBTN_PULSE_MS 20     // G3S5 power-button pulse
#define PWR_LEASE_MS 1000        // bring-up policy, not stock

struct powerctl {
    bool armed;
    uint8_t out[PWR_OUT_COUNT];
    int64_t out_since[PWR_OUT_COUNT];
    uint8_t in[PWR_IN_COUNT];
    int64_t in_since[PWR_IN_COUNT];
    bool vw_ready;
    bool thermal_halt;
    enum powerctl_fault fault;
    int64_t fault_ms;
    uint32_t faults;
    int64_t lease_until;
    int64_t vr_on_ms;       // when VR_ON was last asserted
    bool vr_seen;           // VR power good seen since VR_ON
    int64_t pbtn_release_ms;  // pulse end, or -1
    int64_t s0s3_due, s0s3_rail_due, s3s5_due;  // -1 when idle
    int64_t hang_due;       // one-shot VW-ready check, -1 when idle
};

// An output change for the caller to apply, in list order.
struct powerctl_write {
    uint8_t out;
    uint8_t level;
};
#define PWR_MAX_WRITES PWR_OUT_COUNT

void powerctl_init(struct powerctl *pc, bool armed, int64_t now);
// Feed input levels (0/1) and their observed change times.
void powerctl_inputs(struct powerctl *pc, const uint8_t in[PWR_IN_COUNT], int64_t now);

// Python asks for one output level. 0 and the write to apply, or:
// -EPERM disarmed (assertions only), -EBUSY fault latched, -EAGAIN a
// predecessor, delay or power good is missing (nothing written), -EINVAL.
// Lowering is always allowed. Every accepted request refreshes the lease.
int powerctl_request(struct powerctl *pc, int out, int level, int64_t now,
    struct powerctl_write *w, size_t *n);
// 20 ms power-button press (stock G3S5): RSMRST high for 90 ms, PBTN released.
int powerctl_pbtn_pulse(struct powerctl *pc, int64_t now,
    struct powerctl_write *w, size_t *n);
void powerctl_heartbeat(struct powerctl *pc, int64_t now);
// Immediate shutdown in force_g3 order (stock chipset_force_shutdown()),
// not latched. Returns the writes (all outputs low).
size_t powerctl_power_off(struct powerctl *pc, int64_t now, struct powerctl_write *w);
// Same writes, latched with `reason` if any output was high.
size_t powerctl_emergency(struct powerctl *pc, enum powerctl_fault reason, int64_t now,
    struct powerctl_write *w);
// Clear a latched fault; -EBUSY while any output is still high.
int powerctl_clear_fault(struct powerctl *pc);

// Evaluate protection and timed steps; returns the writes to apply, in order.
size_t powerctl_tick(struct powerctl *pc, int64_t now, struct powerctl_write *w);

bool powerctl_any_on(const struct powerctl *pc);

// S0 peripherals (image 12): true while SYS_PWRGD is asserted and SLP_S3#
// is high, i.e. from stock HOOK_CHIPSET_RESUME to HOOK_CHIPSET_SUSPEND
// (lilac/src/power_sequence.c peripheral_power_resume/suspend: the panel
// backlight enable ec_bkoff_l / gpio_sm_panel_bken_ec). Shutdown passes
// through suspend, so any power-down or fault turns them off.
bool powerctl_s0_peripherals(const struct powerctl *pc);
// Stock chipset startup/shutdown peripherals: on from S5->S3 until S3->S5,
// including S3 suspend. Native fault/off/reload also removes their power.
bool powerctl_startup_peripherals(const struct powerctl *pc);
const char *powerctl_out_name(int out);
const char *powerctl_fault_name(int fault);
int powerctl_out_index(const char *name);
