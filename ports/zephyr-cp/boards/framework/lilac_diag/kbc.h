// Lilac BIOS keyboard controller; pure state machine, no hardware accesses.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define KBC_QUEUE 128
struct kbc {
    uint8_t ram[32], pending, ram_addr, scan_set, leds, rate, output, last;
    bool enabled;
    uint16_t head, count, queue[KBC_QUEUE];
    uint32_t received, sent, dropped, keys, unknown;
};
void kbc_init(struct kbc *k);
void kbc_receive(struct kbc *k, bool command, uint8_t data);
void kbc_key(struct kbc *k, uint16_t code, bool pressed);
bool kbc_pop(struct kbc *k, uint16_t *value);
int deck_board_id(int mv);
bool deck_present(int mv, bool powered);
