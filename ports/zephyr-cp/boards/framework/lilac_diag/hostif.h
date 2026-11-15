// Host-interface protocol logic for the Lilac diagnostic target, free of
// Zephyr so it can be compiled and tested on the host. espi.c owns the
// hardware, locking and timing; this file owns packet and byte semantics,
// mirroring stock ec-lilac common/host_command_task.c and common/acpi.c.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Protocol 3 packets over the LPC/eSPI host command window.
#define HOSTIF_PACKET_SIZE 256  // EC_LPC_HOST_PACKET_SIZE
#define HOSTIF_HEADER_SIZE 8    // ec_host_request and ec_host_response
#define HOSTIF_DATA_MAX (HOSTIF_PACKET_SIZE - HOSTIF_HEADER_SIZE)

// ec_status values used here (ec_commands.h).
#define HOSTIF_RES_SUCCESS 0
#define HOSTIF_RES_INVALID_COMMAND 1
#define HOSTIF_RES_ERROR 2
#define HOSTIF_RES_INVALID_VERSION 6
#define HOSTIF_RES_INVALID_CHECKSUM 7
#define HOSTIF_RES_TIMEOUT 10
#define HOSTIF_RES_INVALID_HEADER 12
#define HOSTIF_RES_REQUEST_TRUNCATED 13
#define HOSTIF_RES_RESPONSE_TOO_BIG 14
#define HOSTIF_RES_BUSY 16

#define HOSTIF_CMD_GET_PROTOCOL_INFO 0x000b

struct hostif_request {
    uint16_t command;
    uint8_t version;
    uint16_t data_len;
    uint8_t data[HOSTIF_DATA_MAX];
};

// Validate a request as stock host_packet_receive() does (size, header
// version, data length, checksum) and copy it out. Returns HOSTIF_RES_SUCCESS
// or the error result to send.
int hostif_parse_request(const uint8_t *buf, size_t size, struct hostif_request *req);

// Build a response as stock host_packet_respond() does into buf (max bytes,
// at least the header): error results carry no data, oversized data turns
// into RESPONSE_TOO_BIG. Returns the packet length; *driver_result is the
// byte stock writes to the data port.
size_t hostif_build_response(uint8_t *buf, size_t max, uint16_t result,
    const uint8_t *data, size_t len, uint8_t *driver_result);

// One outstanding request at a time, owned by Python until answered or timed
// out. The caller serializes all slot calls.
enum hostif_action {
    HOSTIF_QUEUED,   // request stored for Python; arm the timeout
    HOSTIF_RESPOND,  // response built in out; send it now
    HOSTIF_NONE,     // nothing to send (stale seq or no request pending)
};

struct hostif_slot {
    bool pending;
    uint32_t seq;
    struct hostif_request req;
    uint32_t requests, answered, native, bad, busy, timeouts, late, aborted;
};

// A request arrived. Malformed packets and protocol-level commands are
// answered natively; a request while one is pending is answered BUSY.
enum hostif_action hostif_slot_receive(struct hostif_slot *slot, const uint8_t *buf, size_t size,
    uint8_t *out, size_t *out_len, uint8_t *driver_result);
// Python's answer for `seq`. HOSTIF_NONE (and `late` counted) when the
// request already timed out or was aborted.
enum hostif_action hostif_slot_complete(struct hostif_slot *slot, uint32_t seq, uint16_t result,
    const uint8_t *data, size_t len, uint8_t *out, size_t *out_len, uint8_t *driver_result);
// The deadline for `seq` passed: answer TIMEOUT unless already answered.
enum hostif_action hostif_slot_timeout(struct hostif_slot *slot, uint32_t seq,
    uint8_t *out, size_t *out_len, uint8_t *driver_result);
// eSPI or platform reset: drop a pending request without answering.
void hostif_slot_abort(struct hostif_slot *slot);

// ACPI EC byte protocol (ports 0x62/0x66), as stock acpi_ap_to_ec() for the
// generic addresses. Addresses with board side effects (fan duty, keyboard
// backlight, charge limit, features, USB power, strings FIFO) are not served
// yet: reads return 0xff and writes are ignored, both counted.
#define HOSTIF_ACPI_OUTPUT 0x01     // *result must be written to the host
#define HOSTIF_ACPI_BURST_ON 0x02   // set BURST status, arm the 1 s disable
#define HOSTIF_ACPI_BURST_OFF 0x04  // clear BURST status

struct hostif_acpi {
    uint8_t cmd;
    uint8_t addr;
    int data_count;
    uint8_t test;
    uint32_t bytes, queries, ignored_reads, ignored_writes;
};

// memmap: the 0xe0-byte window behind ACPI addresses 0x20..0xff (may be NULL).
int hostif_acpi_byte(struct hostif_acpi *acpi, bool is_cmd, uint8_t value,
    const uint8_t *memmap, uint8_t *result);

// Port 80 codes, oldest first; overflow drops the newest and counts it.
#define HOSTIF_PORT80_DEPTH 64
struct hostif_port80 {
    uint32_t codes[HOSTIF_PORT80_DEPTH];
    uint16_t head, count;
    uint32_t total, dropped, last;
};
void hostif_port80_push(struct hostif_port80 *p, uint32_t code);
// Moves up to max codes into out; returns how many.
size_t hostif_port80_drain(struct hostif_port80 *p, uint32_t *out, size_t max);

// Shared memory (ACPI SHM) access check for Python (image 9): 0, -EINVAL if
// [offset, offset + len) leaves the `size`-byte window, -EPERM for a write
// touching a native-owned byte (EC_MEMMAP_HOST_CMD_FLAGS, 0x27).
#define HOSTIF_MEMMAP_HOST_CMD_FLAGS 0x27
int hostif_memmap_check(size_t offset, size_t len, size_t size, bool write);
