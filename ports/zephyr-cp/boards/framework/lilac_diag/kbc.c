// Copyright 2026. SPDX-License-Identifier: BSD-3-Clause
// Semantics from ec-lilac common/keyboard_8042.c. AUX controller loopback is
// supported, but there is no PS/2 mouse: Lilac's touchpad is host I2C.
#include "kbc.h"
#include <string.h>
static void put(struct kbc *k, uint16_t v) {
    if (k->count == KBC_QUEUE) { k->dropped++; return; }
    k->queue[(k->head + k->count++) % KBC_QUEUE] = v;
}
static void clear(struct kbc *k) { k->head = k->count = 0; }
static void defaults(struct kbc *k) { k->scan_set = 2; k->rate = 0x2b; }
void kbc_init(struct kbc *k) {
    memset(k, 0, sizeof(*k));
    k->ram[0] = 0x70; // stock: translation, keyboard/AUX disabled
    k->output = 3;
    defaults(k);
}
void kbc_receive(struct kbc *k, bool command, uint8_t d) {
    k->received++;
    if (command) {
        k->pending = 0;
        if (d >= 0x20 && d <= 0x3f) { put(k, k->ram[d - 0x20]); return; }
        if (d >= 0x60 && d <= 0x7f) { k->ram_addr = d - 0x60; k->pending = 0x60; return; }
        switch (d) {
            case 0xaa: clear(k); k->ram[0] |= 4; put(k, 0x55); break;
            case 0xab: case 0xa9: put(k, 0); break;
            case 0xad: k->ram[0] |= 0x10; break;
            case 0xae: k->ram[0] &= ~0x10; break;
            case 0xa7: k->ram[0] |= 0x20; break;
            case 0xa8: k->ram[0] &= ~0x20; break;
            case 0xc0: put(k, 0); break;
            case 0xd0: put(k, k->output); break;
            case 0xd1: case 0xd2: case 0xd3: case 0xd4: k->pending = d; break;
            case 0xdd: k->output &= ~2; break;
            case 0xdf: k->output |= 2; break;
            default: k->unknown++; break; // reset/pulse intentionally no AP reset
        }
        return;
    }
    uint8_t pending = k->pending;
    k->pending = 0;
    switch (pending) {
        case 0x60: k->ram[k->ram_addr] = d; return;
        case 0xd1: k->output = d; return;
        case 0xd2: put(k, d); return;
        case 0xd3: put(k, 0x100 | d); return;
        case 0xd4: return; // no PS/2 AUX device, as stock without CONFIG_8042_AUX
        case 0xed: k->leds = d & 7; put(k, 0xfa); return;
        case 0xf3: k->rate = d; put(k, 0xfa); return;
        case 0xf0:
            put(k, 0xfa);
            if (!d) { put(k, k->scan_set); }
            else if (d <= 2) { k->scan_set = d; }
            return;
        default: break;
    }
    switch (d) {
        case 0xed: case 0xf0: case 0xf3: put(k, 0xfa); k->pending = d; break;
        case 0xf2: put(k, 0xfa); put(k, 0xab); put(k, 0x83); break;
        case 0xee: put(k, 0xfa); put(k, 0xee); break;
        case 0xf4: clear(k); k->enabled = true; put(k, 0xfa); break;
        case 0xf5: clear(k); k->enabled = false; defaults(k); put(k, 0xfa); break;
        case 0xf6: clear(k); defaults(k); put(k, 0xfa); break;
        case 0xff: clear(k); defaults(k); put(k, 0xfa); put(k, 0xaa); break;
        case 0xfe: put(k, k->last); break;
        default: put(k, 0xfe); k->unknown++; break;
    }
}
static const uint8_t set1[128] = {
		  0xff, 0x43, 0x41, 0x3f, 0x3d, 0x3b, 0x3c, 0x58, 0x64, 0x44,
		  0x42, 0x40, 0x3e, 0x0f, 0x29, 0x59, 0x65, 0x38, 0x2a, 0x70,
		  0x1d, 0x10, 0x02, 0x5a, 0x66, 0x71, 0x2c, 0x1f, 0x1e, 0x11,
		  0x03, 0x5b, 0x67, 0x2e, 0x2d, 0x20, 0x12, 0x05, 0x04, 0x5c,
		  0x68, 0x39, 0x2f, 0x21, 0x14, 0x13, 0x06, 0x5d, 0x69, 0x31,
		  0x30, 0x23, 0x22, 0x15, 0x07, 0x5e, 0x6a, 0x72, 0x32, 0x24,
		  0x16, 0x08, 0x09, 0x5f, 0x6b, 0x33, 0x25, 0x17, 0x18, 0x0b,
		  0x0a, 0x60, 0x6c, 0x34, 0x35, 0x26, 0x27, 0x19, 0x0c, 0x61,
		  0x6d, 0x73, 0x28, 0x74, 0x1a, 0x0d, 0x62, 0x6e, 0x3a, 0x36,
		  0x1c, 0x1b, 0x75, 0x2b, 0x63, 0x76, 0x55, 0x56, 0x77, 0x78,
		  0x79, 0x7a, 0x0e, 0x7b, 0x7c, 0x4f, 0x7d, 0x4b, 0x47, 0x7e,
		  0x7f, 0x6f, 0x52, 0x53, 0x50, 0x4c, 0x4d, 0x48, 0x01, 0x45,
		  0x57, 0x4e, 0x51, 0x4a, 0x37, 0x49, 0x46, 0x54,
	  };
void kbc_key(struct kbc *k, uint16_t code, bool pressed) {
    if (!k->enabled || (k->ram[0] & 0x10) || !code || code == 0xff) { return; }
    uint8_t bytes[4], n = 0, b = code;
    if (code >> 8 == 0xe0) { bytes[n++] = 0xe0; }
    if ((k->ram[0] & 0x40) || k->scan_set == 1) {
        b = b == 0x83 ? 0x41 : (b < 128 ? set1[b] : b);
        bytes[n++] = pressed ? b : b | 0x80;
    } else {
        if (!pressed) { bytes[n++] = 0xf0; }
        bytes[n++] = b;
    }
    // Never insert a partial E0/F0 sequence.
    if (k->count + n > KBC_QUEUE) { k->dropped++; return; }
    for (uint8_t i = 0; i < n; i++) { put(k, bytes[i]); }
    k->keys++;
}
bool kbc_pop(struct kbc *k, uint16_t *value) {
    if (!k->count) { return false; }
    *value = k->queue[k->head];
    k->head = (k->head + 1) % KBC_QUEUE;
    k->count--; k->sent++; k->last = *value;
    return true;
}
int deck_board_id(int mv) {
    static const int limits[] = {85,233,360,492,649,844,965,1094,1380,1562,1710,2040,2197,2557,2766,2814};
    if (mv < 0) { return -1; }
    for (int i = 0; i < 16; i++) { if (mv < limits[i]) { return i; } }
    return -1;
}
bool deck_present(int mv, bool powered) {
    int id = deck_board_id(mv);
    // Stock's unknown (-1) passes the unpowered comparison; fail closed instead.
    return id >= 0 && (powered ? id >= 1 : id <= 10);
}
