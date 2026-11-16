// Physical power-button mechanism; no Zephyr or hardware access.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define BUTTON_DEBOUNCE_MS 30
#define BUTTON_PULSE_MS 32
#define BUTTON_HOLD_MS 8000
#define BUTTON_ASSERT 1
#define BUTTON_RELEASE 2
#define BUTTON_OFF 4
struct buttonctl {
    bool enabled, ready, pressed, start_pending, host_low, forwarded, held_off;
    int8_t candidate;
    int64_t candidate_since, pressed_since, release_due;
    uint32_t presses, holds, errors;
};
void buttonctl_arm(struct buttonctl *b, bool enabled, int64_t now);
// raw_level: active-low GPD2, negative on read error. All power decisions
// remain with powerctl/the supervised Python policy; this returns actions.
int buttonctl_tick(struct buttonctl *b, int raw_level, bool can_start,
    bool host_ready, bool powered, int64_t now);
bool buttonctl_poll(struct buttonctl *b);
