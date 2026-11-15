// Native eSPI host interface for the Lilac diagnostic target, configured as
// stock ec-lilac (zephyr/shim/src/espi.c with program/framework Kconfig):
// quad I/O at up to 50 MHz, virtual-wire + peripheral + OOB channels, ACPI
// EC on 0x62/0x66 with SCI on GPIO ec_sci_l, host commands (protocol 3) on
// 0x200/0x204/0x800, the 512-byte memmap window at 0xe00, multi-byte Port 80
// and the 8042 window. Python handles host commands through a single slot;
// native code answers on its own if Python misses the deadline, so the host
// never sees a stuck busy flag. Packet and byte semantics live in hostif.c.
#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/espi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <soc.h>

#include "hostif.h"
#include "lilac_diag.h"

LOG_MODULE_REGISTER(lilac_espi, LOG_LEVEL_INF);

#define ESPI_NODE DT_NODELABEL(espi0)
static const struct device *const espi_dev = DEVICE_DT_GET(ESPI_NODE);

// ec_sci_l: open-drain, released high by diagnostic.c (stock gpio.dtsi).
static const struct device *const sci_port = DEVICE_DT_GET(DT_NODELABEL(gpio7));
#define SCI_PIN 6
#define SCI_PULSE_US 65  // stock lpc_generate_sci() with CONFIG_PLATFORM_EC_SCI_GPIO

#define EC_LPC_STATUS_PROCESSING 0x04
#define EC_LPC_STATUS_BURST_MODE 0x10
#define EC_COMMAND_PROTOCOL_3 0xda
#define EC_MEMMAP_HOST_CMD_FLAGS HOSTIF_MEMMAP_HOST_CMD_FLAGS
#define EC_HOST_CMD_FLAG_LPC_ARGS_SUPPORTED 0x01
#define EC_HOST_CMD_FLAG_VERSION_3 0x02
#define ACPI_MEMMAP_OFFSET 0x20  // ACPI addresses 0x20.. map to memmap[0..]
#define ACPI_BURST_TIMEOUT_MS 1000  // stock acpi_disable_burst_deferred

static struct k_spinlock lock;
static struct hostif_slot slot;
static struct hostif_acpi acpi;
static struct hostif_port80 port80;
static uint8_t *hc_shm;     // host command window (request in, response out)
static uint8_t *memmap;     // 512-byte ACPI shared memory window at 0xe00
static int config_result = -EAGAIN;
static bool vw_ready, started;
static int8_t espi_rst = -1;  // last eSPI_RST# level from the driver, -1 until an edge
static int8_t pltrst = -1;    // last PLTRST# VW level, -1 until received
static uint32_t espi_resets, pltrst_edges, kbc_bytes, unknown_protocol;
static struct k_work_delayable hc_timeout_work;
static struct k_work_delayable burst_work;
static uint32_t timeout_seq;

static void send_result(uint8_t driver_result) {
    uint32_t data = driver_result;
    int ret = espi_write_lpc_request(espi_dev, ECUSTOM_HOST_CMD_SEND_RESULT, &data);
    if (ret) {
        LOG_ERR("host command result write failed: %d", ret);
    }
}

// Caller holds `lock`. Copies the built response into the host window.
static void deliver(enum hostif_action action, const uint8_t *out, size_t len, uint8_t driver_result) {
    if (action != HOSTIF_RESPOND) {
        return;
    }
    if (hc_shm) {
        memcpy(hc_shm, out, len);
    }
    send_result(driver_result);
}

static uint8_t out_buf[HOSTIF_PACKET_SIZE];

static void handle_host_command(uint32_t data) {
    uint32_t shm = 0;
    size_t len;
    uint8_t driver_result;
    k_spinlock_key_t key = k_spin_lock(&lock);
    if ((data & 0xff) != EC_COMMAND_PROTOCOL_3) {
        // Stock logs and never answers; answer instead of stranding the host.
        unknown_protocol++;
        send_result(HOSTIF_RES_INVALID_HEADER);
        k_spin_unlock(&lock, key);
        return;
    }
    if (espi_read_lpc_request(espi_dev, ECUSTOM_HOST_CMD_GET_PARAM_MEMORY, &shm) || !shm) {
        send_result(HOSTIF_RES_ERROR);
        k_spin_unlock(&lock, key);
        return;
    }
    hc_shm = (uint8_t *)shm;
    // LPC/eSPI does not carry the request size: offer the whole window, as stock.
    enum hostif_action action = hostif_slot_receive(&slot, hc_shm, HOSTIF_PACKET_SIZE,
        out_buf, &len, &driver_result);
    deliver(action, out_buf, len, driver_result);
    if (action == HOSTIF_QUEUED) {
        // A stale timer from an earlier request is restarted here; one that
        // still fires finds a different seq or no pending request and no-ops.
        timeout_seq = slot.seq;
        k_work_reschedule(&hc_timeout_work, K_MSEC(LILAC_HC_TIMEOUT_MS));
    }
    k_spin_unlock(&lock, key);
}

static void hc_timeout_handler(struct k_work *work) {
    ARG_UNUSED(work);
    size_t len;
    uint8_t driver_result;
    k_spinlock_key_t key = k_spin_lock(&lock);
    enum hostif_action action = hostif_slot_timeout(&slot, timeout_seq, out_buf, &len, &driver_result);
    deliver(action, out_buf, len, driver_result);
    k_spin_unlock(&lock, key);
    if (action == HOSTIF_RESPOND) {
        LOG_WRN("host command 0x%04x timed out", slot.req.command);
    }
}

static void lpc_status_update(uint32_t set, uint32_t clear) {
    uint32_t status;
    if (espi_read_lpc_request(espi_dev, EACPI_READ_STS, &status) == 0) {
        status = (status | set) & ~clear;
        espi_write_lpc_request(espi_dev, EACPI_WRITE_STS, &status);
    }
}

static void generate_sci(void) {
    gpio_pin_set_raw(sci_port, SCI_PIN, 1);
    k_busy_wait(SCI_PULSE_US);
    gpio_pin_set_raw(sci_port, SCI_PIN, 0);
    k_busy_wait(SCI_PULSE_US);
    gpio_pin_set_raw(sci_port, SCI_PIN, 1);
}

static void burst_handler(struct k_work *work) {
    ARG_UNUSED(work);
    lpc_status_update(0, EC_LPC_STATUS_BURST_MODE);
}

// As stock handle_acpi_write(): process, clear PROCESSING, then SCI for IBE/OBF.
static void handle_acpi(uint32_t data) {
    struct espi_evt_data_acpi *evt = (struct espi_evt_data_acpi *)&data;
    uint8_t result = 0;
    k_spinlock_key_t key = k_spin_lock(&lock);
    int flags = hostif_acpi_byte(&acpi, evt->type, evt->data,
        memmap ? memmap + ACPI_MEMMAP_OFFSET : NULL, &result);
    k_spin_unlock(&lock, key);
    if (flags & HOSTIF_ACPI_OUTPUT) {
        uint32_t out = result;
        espi_write_lpc_request(espi_dev, EACPI_WRITE_CHAR, &out);
    }
    if (flags & HOSTIF_ACPI_BURST_ON) {
        lpc_status_update(EC_LPC_STATUS_BURST_MODE, 0);
        k_work_reschedule(&burst_work, K_MSEC(ACPI_BURST_TIMEOUT_MS));
    }
    if (flags & HOSTIF_ACPI_BURST_OFF) {
        k_work_cancel_delayable(&burst_work);
        lpc_status_update(0, EC_LPC_STATUS_BURST_MODE);
    }
    lpc_status_update(0, EC_LPC_STATUS_PROCESSING);
    generate_sci();
}

static void peripheral_handler(const struct device *dev, struct espi_callback *cb, struct espi_event event) {
    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    switch (event.evt_details & 0xffff) {
        case ESPI_PERIPHERAL_DEBUG_PORT80: {
            k_spinlock_key_t key = k_spin_lock(&lock);
            hostif_port80_push(&port80, event.evt_data);
            k_spin_unlock(&lock, key);
            break;
        }
        case ESPI_PERIPHERAL_HOST_IO:
            handle_acpi(event.evt_data);
            break;
        case ESPI_PERIPHERAL_EC_HOST_CMD:
            handle_host_command(event.evt_data);
            break;
        case ESPI_PERIPHERAL_8042_KBC:
            // No keyboard controller yet: the driver has consumed the byte.
            kbc_bytes++;
            break;
        default:
            break;
    }
}

static void bus_handler(const struct device *dev, struct espi_callback *cb, struct espi_event event) {
    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    k_spinlock_key_t key = k_spin_lock(&lock);
    if (event.evt_type == ESPI_BUS_RESET) {
        espi_rst = event.evt_data ? 1 : 0;
        espi_resets++;
        if (!event.evt_data) {
            vw_ready = false;
            hostif_slot_abort(&slot);
        }
    } else if (event.evt_type == ESPI_BUS_EVENT_CHANNEL_READY) {
        if (event.evt_details == ESPI_CHANNEL_VWIRE && event.evt_data) {
            vw_ready = true;  // stock custom_espi_channel_ready_handler()
        }
    } else if (event.evt_type == ESPI_BUS_EVENT_VWIRE_RECEIVED &&
               event.evt_details == ESPI_VWIRE_SIGNAL_PLTRST) {
        if (pltrst != (int8_t)event.evt_data) {
            pltrst_edges++;
        }
        pltrst = event.evt_data ? 1 : 0;
        if (!event.evt_data) {
            hostif_slot_abort(&slot);
        }
    }
    k_spin_unlock(&lock, key);
}

// Framework zephyr-ec deltas that CP's Zephyr lacks, applied after the driver
// has initialized: VW index 33h input on VWEVMS[8] (ESPI_VWIRE_INDEX_33H) and
// a host-writable ACPI shared-memory window
// (ESPI_PERIPHERAL_ACPI_SHM_REGION_WRITE_PROTECT_ENABLE=n).
static void apply_fork_deltas(void) {
    struct espi_reg *const espi = (struct espi_reg *)DT_REG_ADDR(ESPI_NODE);
    SET_FIELD(espi->VWEVMS[8], NPCX_VWEVMS_INDEX, 0x33);
    espi->VWEVMS[8] |= BIT(NPCX_VWEVMS_IE) | BIT(NPCX_VWEVMS_WE) | BIT(NPCX_VWEVMS_INDEX_EN);

    struct shm_reg *const shm = (struct shm_reg *)DT_REG_ADDR_BY_NAME(DT_NODELABEL(host_sub), shm);
    shm->WIN2_WR_PROT = 0;
}

static struct espi_callback callbacks[2];

// Same order as stock zephyr_shim_setup_espi(): callbacks, config, then host
// interrupts. Runs before main, independent of Python.
static int lilac_espi_init(void) {
    k_work_init_delayable(&hc_timeout_work, hc_timeout_handler);
    k_work_init_delayable(&burst_work, burst_handler);
    if (!device_is_ready(espi_dev)) {
        config_result = -ENODEV;
        return 0;
    }
    espi_init_callback(&callbacks[0], bus_handler,
        ESPI_BUS_RESET | ESPI_BUS_EVENT_CHANNEL_READY | ESPI_BUS_EVENT_VWIRE_RECEIVED);
    espi_init_callback(&callbacks[1], peripheral_handler, ESPI_BUS_PERIPHERAL_NOTIFICATION);
    espi_add_callback(espi_dev, &callbacks[0]);
    espi_add_callback(espi_dev, &callbacks[1]);

    struct espi_cfg cfg = {
        .io_caps = ESPI_IO_MODE_QUAD_LINES,
        .channel_caps = ESPI_CHANNEL_VWIRE | ESPI_CHANNEL_PERIPHERAL | ESPI_CHANNEL_OOB,
        .max_freq = 50,
    };
    config_result = espi_config(espi_dev, &cfg);
    if (config_result) {
        LOG_ERR("eSPI config failed: %d", config_result);
        return 0;
    }
    apply_fork_deltas();

    uint32_t shm = 0;
    if (espi_read_lpc_request(espi_dev, EACPI_GET_SHARED_MEMORY, &shm) == 0 && shm) {
        memmap = (uint8_t *)shm;
        // As stock lpc_host_command_init().
        memmap[EC_MEMMAP_HOST_CMD_FLAGS] =
            EC_HOST_CMD_FLAG_LPC_ARGS_SUPPORTED | EC_HOST_CMD_FLAG_VERSION_3;
    }
    uint32_t enable = 1;
    espi_write_lpc_request(espi_dev, ECUSTOM_HOST_SUBS_INTERRUPT_EN, &enable);
    started = true;
    return 0;
}
SYS_INIT(lilac_espi_init, APPLICATION, 0);

bool lilac_espi_vw_ready(void) {
    return vw_ready;
}

int lilac_memmap_read(size_t offset, uint8_t *out, size_t len) {
    int ret = hostif_memmap_check(offset, len, LILAC_MEMMAP_SIZE, false);
    if (ret) {
        return ret;
    }
    k_spinlock_key_t key = k_spin_lock(&lock);
    if (memmap == NULL) {
        k_spin_unlock(&lock, key);
        return -ENODEV;
    }
    memcpy(out, memmap + offset, len);
    k_spin_unlock(&lock, key);
    return 0;
}

int lilac_memmap_update(size_t offset, const uint8_t *data, size_t len, uint8_t mask) {
    int ret = hostif_memmap_check(offset, len, LILAC_MEMMAP_SIZE, true);
    if (ret) {
        return ret;
    }
    k_spinlock_key_t key = k_spin_lock(&lock);
    if (memmap == NULL) {
        k_spin_unlock(&lock, key);
        return -ENODEV;
    }
    for (size_t i = 0; i < len; i++) {
        uint8_t *b = &memmap[offset + i];
        *b = (uint8_t)((*b & ~mask) | (data[i] & mask));
    }
    k_spin_unlock(&lock, key);
    return 0;
}

void lilac_espi_status(struct lilac_espi_status *st) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    st->config_result = started ? 0 : config_result;
    st->vw_ready = vw_ready;
    st->espi_rst = espi_rst;
    st->pltrst = pltrst;
    st->espi_resets = espi_resets;
    st->pltrst_edges = pltrst_edges;
    st->hc_requests = slot.requests;
    st->hc_answered = slot.answered;
    st->hc_native = slot.native;
    st->hc_bad = slot.bad;
    st->hc_busy = slot.busy;
    st->hc_timeouts = slot.timeouts;
    st->hc_late = slot.late;
    st->hc_aborted = slot.aborted;
    st->hc_unknown_protocol = unknown_protocol;
    st->acpi_bytes = acpi.bytes;
    st->acpi_queries = acpi.queries;
    st->acpi_ignored_reads = acpi.ignored_reads;
    st->acpi_ignored_writes = acpi.ignored_writes;
    st->kbc_bytes = kbc_bytes;
    st->port80_total = port80.total;
    st->port80_dropped = port80.dropped;
    st->port80_last = port80.last;
    k_spin_unlock(&lock, key);
}

size_t lilac_port80_drain(uint32_t *out, size_t max) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    size_t n = hostif_port80_drain(&port80, out, max);
    k_spin_unlock(&lock, key);
    return n;
}

int lilac_hc_poll(struct lilac_hc_request *req) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    int ret = -EAGAIN;
    if (slot.pending) {
        req->seq = slot.seq;
        req->command = slot.req.command;
        req->version = slot.req.version;
        req->data_len = slot.req.data_len;
        memcpy(req->data, slot.req.data, slot.req.data_len);
        ret = 0;
    }
    k_spin_unlock(&lock, key);
    return ret;
}

int lilac_hc_respond(uint32_t seq, uint16_t result, const uint8_t *data, size_t len) {
    if (len > HOSTIF_DATA_MAX) {
        return -EINVAL;
    }
    size_t out_len;
    uint8_t driver_result;
    k_spinlock_key_t key = k_spin_lock(&lock);
    enum hostif_action action = hostif_slot_complete(&slot, seq, result, data, len,
        out_buf, &out_len, &driver_result);
    deliver(action, out_buf, out_len, driver_result);
    k_spin_unlock(&lock, key);
    // The armed timer is left running; it finds no pending request and no-ops.
    return action == HOSTIF_RESPOND ? 0 : -ETIMEDOUT;
}
