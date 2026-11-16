#include "buttonctl.h"
#include <string.h>
void buttonctl_arm(struct buttonctl *b, bool enabled, int64_t now) {
    memset(b, 0, sizeof(*b));
    b->enabled = enabled;
    b->candidate = -1;
    b->candidate_since = now;
    b->release_due = -1;
}
bool buttonctl_poll(struct buttonctl *b) {
    bool pending = b->enabled && b->start_pending;
    b->start_pending = false;
    return pending;
}
int buttonctl_tick(struct buttonctl *b, int raw, bool can_start,
    bool host_ready, bool powered, int64_t now) {
    if (!b->enabled) { return 0; }
    int action = 0;
    if (raw < 0) {
        b->errors++;
        if (b->host_low) { action |= BUTTON_RELEASE; }
        b->host_low = b->ready = b->pressed = b->start_pending = false;
        b->candidate = -1;
        b->release_due = -1;
        return action;
    }
    raw = !!raw;
    if (raw != b->candidate) {
        b->candidate = raw;
        b->candidate_since = now;
    }
    if (!host_ready && b->host_low) {
        b->host_low = false;
        b->release_due = -1;
        action |= BUTTON_RELEASE;
    }
    if (b->release_due >= 0 && now >= b->release_due) {
        b->release_due = -1;
        b->host_low = false;
        action |= BUTTON_RELEASE;
    }
    if (now - b->candidate_since >= BUTTON_DEBOUNCE_MS) {
        if (raw) {
            b->ready = true; // boot/reload-held button must first be released
            b->pressed = false;
            b->forwarded = b->held_off = false;
        } else if (b->ready && !b->pressed) {
            b->pressed = true;
            b->pressed_since = now;
            b->presses++;
            b->forwarded = host_ready;
            if (can_start) { b->start_pending = true; }
            else if (host_ready) {
                b->host_low = true;
                b->release_due = now + BUTTON_PULSE_MS;
                action |= BUTTON_ASSERT;
            }
        }
    }
    if (b->pressed && !b->held_off) {
        int64_t held = now - b->pressed_since;
        // Stock pre-OS behavior: 32 ms initial pulse, reassert at 4 s,
        // force G3 at 8 s. An off->on press never leaks to the AP again.
        if (held >= BUTTON_HOLD_MS) {
            b->held_off = true;
            b->start_pending = false;
            b->holds++;
            if (powered) { action |= BUTTON_OFF; }
        } else if (b->forwarded && host_ready && held >= 4000 && !b->host_low) {
            b->host_low = true;
            action |= BUTTON_ASSERT;
        }
    }
    if (!b->pressed && b->host_low && b->release_due < 0) {
        b->host_low = false;
        action |= BUTTON_RELEASE;
    }
    if (!can_start) { b->start_pending = false; }
    return action;
}
