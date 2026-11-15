// Host-interface protocol logic; see hostif.h. No Zephyr dependencies.
#include <errno.h>
#include <string.h>

#include "hostif.h"

#define HOSTIF_REQUEST_VERSION 3   // EC_HOST_REQUEST_VERSION
#define HOSTIF_RESPONSE_VERSION 3  // EC_HOST_RESPONSE_VERSION

// ACPI (ec_commands.h)
#define ACPI_CMD_READ 0x80
#define ACPI_CMD_WRITE 0x81
#define ACPI_CMD_BURST_ENABLE 0x82
#define ACPI_CMD_BURST_DISABLE 0x83
#define ACPI_CMD_QUERY_EVENT 0x84
#define ACPI_MEM_VERSION 0x00
#define ACPI_MEM_TEST 0x01
#define ACPI_MEM_TEST_COMPLIMENT 0x02
#define ACPI_MEM_MAPPED_BEGIN 0x20
#define ACPI_MEM_VERSION_CURRENT 2
#define ACPI_BURST_ACK 0x90

int hostif_parse_request(const uint8_t *buf, size_t size, struct hostif_request *req) {
    if (size < HOSTIF_HEADER_SIZE || size > HOSTIF_PACKET_SIZE) {
        return HOSTIF_RES_REQUEST_TRUNCATED;
    }
    if (buf[0] != HOSTIF_REQUEST_VERSION) {
        return HOSTIF_RES_INVALID_HEADER;
    }
    uint16_t data_len = (uint16_t)(buf[6] | (buf[7] << 8));
    if (size < (size_t)HOSTIF_HEADER_SIZE + data_len) {
        return HOSTIF_RES_REQUEST_TRUNCATED;
    }
    uint8_t sum = 0;
    for (size_t i = 0; i < (size_t)HOSTIF_HEADER_SIZE + data_len; i++) {
        sum += buf[i];
    }
    if (sum != 0) {
        return HOSTIF_RES_INVALID_CHECKSUM;
    }
    req->command = (uint16_t)(buf[2] | (buf[3] << 8));
    req->version = buf[4];
    req->data_len = data_len;
    memcpy(req->data, buf + HOSTIF_HEADER_SIZE, data_len);
    return HOSTIF_RES_SUCCESS;
}

size_t hostif_build_response(uint8_t *buf, size_t max, uint16_t result,
    const uint8_t *data, size_t len, uint8_t *driver_result) {
    if (result != HOSTIF_RES_SUCCESS) {
        len = 0;
    } else if (len > max - HOSTIF_HEADER_SIZE) {
        result = HOSTIF_RES_RESPONSE_TOO_BIG;
        len = 0;
    }
    if (len) {
        memmove(buf + HOSTIF_HEADER_SIZE, data, len);
    }
    buf[0] = HOSTIF_RESPONSE_VERSION;
    buf[1] = 0;
    buf[2] = (uint8_t)result;
    buf[3] = (uint8_t)(result >> 8);
    buf[4] = (uint8_t)len;
    buf[5] = (uint8_t)(len >> 8);
    buf[6] = 0;
    buf[7] = 0;
    uint8_t sum = 0;
    for (size_t i = 0; i < HOSTIF_HEADER_SIZE + len; i++) {
        sum += buf[i];
    }
    buf[1] = (uint8_t)-sum;
    // Stock writes driver_result (the ec_status) as a byte to the data port.
    *driver_result = (uint8_t)result;
    return HOSTIF_HEADER_SIZE + len;
}

static enum hostif_action respond(uint8_t *out, size_t *out_len, uint8_t *driver_result,
    uint16_t result, const uint8_t *data, size_t len) {
    *out_len = hostif_build_response(out, HOSTIF_PACKET_SIZE, result, data, len, driver_result);
    return HOSTIF_RESPOND;
}

// Transport-level command answered natively, as the stock eSPI shim does.
static bool native_command(const struct hostif_request *req, uint8_t *out, size_t *out_len,
    uint8_t *driver_result) {
    if (req->command != HOSTIF_CMD_GET_PROTOCOL_INFO) {
        return false;
    }
    if (req->version != 0) {
        respond(out, out_len, driver_result, HOSTIF_RES_INVALID_VERSION, NULL, 0);
        return true;
    }
    // ec_response_get_protocol_info: versions BIT(3), max request/response
    // EC_LPC_HOST_PACKET_SIZE, flags 0; little-endian.
    uint8_t info[12] = { 0x08, 0, 0, 0,
                         HOSTIF_PACKET_SIZE & 0xff, HOSTIF_PACKET_SIZE >> 8,
                         HOSTIF_PACKET_SIZE & 0xff, HOSTIF_PACKET_SIZE >> 8,
                         0, 0, 0, 0 };
    respond(out, out_len, driver_result, HOSTIF_RES_SUCCESS, info, sizeof(info));
    return true;
}

enum hostif_action hostif_slot_receive(struct hostif_slot *slot, const uint8_t *buf, size_t size,
    uint8_t *out, size_t *out_len, uint8_t *driver_result) {
    slot->requests++;
    if (slot->pending) {
        slot->busy++;
        return respond(out, out_len, driver_result, HOSTIF_RES_BUSY, NULL, 0);
    }
    int ret = hostif_parse_request(buf, size, &slot->req);
    if (ret != HOSTIF_RES_SUCCESS) {
        slot->bad++;
        return respond(out, out_len, driver_result, (uint16_t)ret, NULL, 0);
    }
    if (native_command(&slot->req, out, out_len, driver_result)) {
        slot->native++;
        return HOSTIF_RESPOND;
    }
    slot->pending = true;
    slot->seq++;
    return HOSTIF_QUEUED;
}

enum hostif_action hostif_slot_complete(struct hostif_slot *slot, uint32_t seq, uint16_t result,
    const uint8_t *data, size_t len, uint8_t *out, size_t *out_len, uint8_t *driver_result) {
    if (!slot->pending || seq != slot->seq) {
        slot->late++;
        return HOSTIF_NONE;
    }
    slot->pending = false;
    slot->answered++;
    return respond(out, out_len, driver_result, result, data, len);
}

enum hostif_action hostif_slot_timeout(struct hostif_slot *slot, uint32_t seq,
    uint8_t *out, size_t *out_len, uint8_t *driver_result) {
    if (!slot->pending || seq != slot->seq) {
        return HOSTIF_NONE;
    }
    slot->pending = false;
    slot->timeouts++;
    return respond(out, out_len, driver_result, HOSTIF_RES_TIMEOUT, NULL, 0);
}

void hostif_slot_abort(struct hostif_slot *slot) {
    if (slot->pending) {
        slot->pending = false;
        slot->aborted++;
    }
}

int hostif_acpi_byte(struct hostif_acpi *acpi, bool is_cmd, uint8_t value,
    const uint8_t *memmap, uint8_t *result) {
    acpi->bytes++;
    if (is_cmd) {
        acpi->cmd = value;
        acpi->data_count = 0;
    } else if (!acpi->data_count++) {
        acpi->addr = value;
    }

    if (acpi->cmd == ACPI_CMD_READ && acpi->data_count == 1) {
        switch (acpi->addr) {
            case ACPI_MEM_VERSION:
                *result = ACPI_MEM_VERSION_CURRENT;
                break;
            case ACPI_MEM_TEST:
                *result = acpi->test;
                break;
            case ACPI_MEM_TEST_COMPLIMENT:
                *result = (uint8_t)(0xff - acpi->test);
                break;
            default:
                if (acpi->addr >= ACPI_MEM_MAPPED_BEGIN && memmap) {
                    *result = memmap[acpi->addr - ACPI_MEM_MAPPED_BEGIN];
                } else {
                    acpi->ignored_reads++;
                    *result = 0xff;
                }
                break;
        }
        return HOSTIF_ACPI_OUTPUT;
    }
    if (acpi->cmd == ACPI_CMD_WRITE && acpi->data_count == 2) {
        if (acpi->addr == ACPI_MEM_TEST) {
            acpi->test = value;
        } else {
            acpi->ignored_writes++;
        }
        return 0;
    }
    if (acpi->cmd == ACPI_CMD_QUERY_EVENT && acpi->data_count == 0) {
        // No host events are raised yet: always "none pending".
        acpi->queries++;
        *result = 0;
        return HOSTIF_ACPI_OUTPUT;
    }
    if (acpi->cmd == ACPI_CMD_BURST_ENABLE && acpi->data_count == 0) {
        *result = ACPI_BURST_ACK;
        return HOSTIF_ACPI_OUTPUT | HOSTIF_ACPI_BURST_ON;
    }
    if (acpi->cmd == ACPI_CMD_BURST_DISABLE && acpi->data_count == 0) {
        return HOSTIF_ACPI_BURST_OFF;
    }
    return 0;
}

void hostif_port80_push(struct hostif_port80 *p, uint32_t code) {
    p->total++;
    p->last = code;
    if (p->count == HOSTIF_PORT80_DEPTH) {
        p->dropped++;
        p->head = (uint16_t)((p->head + 1) % HOSTIF_PORT80_DEPTH);
        p->count--;
    }
    p->codes[(p->head + p->count) % HOSTIF_PORT80_DEPTH] = code;
    p->count++;
}

size_t hostif_port80_drain(struct hostif_port80 *p, uint32_t *out, size_t max) {
    size_t n = 0;
    while (n < max && p->count) {
        out[n++] = p->codes[p->head];
        p->head = (uint16_t)((p->head + 1) % HOSTIF_PORT80_DEPTH);
        p->count--;
    }
    return n;
}

int hostif_memmap_check(size_t offset, size_t len, size_t size, bool write) {
    if (offset > size || len > size - offset) {
        return -EINVAL;
    }
    if (write && offset <= HOSTIF_MEMMAP_HOST_CMD_FLAGS && HOSTIF_MEMMAP_HOST_CMD_FLAGS - offset < len) {
        return -EPERM;
    }
    return 0;
}
