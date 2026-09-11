/* device.c: STUDENT IMPLEMENTATION FILE for Project 1 (Part II).
 *
 * You implement the paravirtual logging device here. The emulator (vmm.c) maps
 * a DEV_SIZE-byte MMIO region at DEV_BASE and routes every read/write within it
 * to vlog_device_mmio_read() / vlog_device_mmio_write() below.
 *
 * The device reads a log message from guest memory and hands it, via the
 * provided log store, to the host log file. The store lives
 * in the VMM (dev->vmm->store); you append validated records with
 * logstore_append(store, seq, level, bytes, len). It owns the record format
 * and is thread-safe (the provided network ingest path shares it).
 *
 * WHAT IS PROVIDED: the register ABI and device state (device.h),
 * vlog_device_init(), the log store (logstore_append / logstore_flush), and the
 * small helpers below (msg_addr / set_error / clear_error). Use them or ignore
 * them as you see fit.
 *
 * YOUR TASK: implement the two MMIO handlers:
 *   - vlog_device_mmio_read():  return the value of the addressed register.
 *   - vlog_device_mmio_write(): update operand registers, and on a CMD write
 *                               execute the command (NOP / LOG / FLUSH).
 * See SPEC.md for the register map (§2), STATUS/errors (§3), and commands (§4).
 *
 * Rules:
 *   - Access the guest message buffer ONLY through vmm_gpa_to_host(). Never cast
 *     a guest address to a host pointer yourself, and always check the result.
 *   - Report errors through the STATUS register (use set_error()); do not crash
 *     the emulator on malformed input, and do not write a partial record to the
 *     log when an error is detected.
 */
#include "device.h"
#include "vmm.h"

#include <string.h>

/* ---- Helpers (provided; use if you find them handy) ------------------ */

/* Reassemble the 64-bit guest-physical message address from its two halves. */
static inline uint64_t msg_addr(const struct vlog_device *dev)
{
    return (uint64_t)dev->msg_addr_lo | ((uint64_t)dev->msg_addr_hi << 32);
}

/* Set the ERROR flag with an error code (SPEC.md §3). */
static inline void set_error(struct vlog_device *dev, uint32_t code)
{
    dev->status |= VLOG_STATUS_ERROR | (code << VLOG_STATUS_ERR_SHIFT);
}

/* Clear the ERROR flag and error code. */
static inline void clear_error(struct vlog_device *dev)
{
    dev->status &= ~(VLOG_STATUS_ERROR | (0xffu << VLOG_STATUS_ERR_SHIFT));
}

/* ---- Provided: one-time device state initialization ------------------ */

void vlog_device_init(struct vlog_device *dev, struct vmm *vmm)
{
    dev->vmm         = vmm;
    dev->status      = VLOG_STATUS_READY;
    dev->msg_addr_lo = 0;
    dev->msg_addr_hi = 0;
    dev->len         = 0;
    dev->level       = VLOG_LVL_INFO;
    dev->seq         = 0;
    dev->bytes       = 0;
}

/* ---- Commands (SPEC.md §4) -------------------------------------------- */

/* LOG: append the LEN bytes at MSG to the log as one record. Everything is
 * validated before anything is written, so a failure appends nothing. */
static void cmd_log(struct vlog_device *dev)
{
    if (dev->len > VLOG_MAX_MSG) {
        set_error(dev, VLOG_ERR_BADLEN);
        return;
    }

    /* An empty message is valid, and its address is not examined. */
    const void *msg = NULL;
    if (dev->len > 0) {
        msg = vmm_gpa_to_host(dev->vmm, msg_addr(dev), dev->len);
        if (msg == NULL) {
            set_error(dev, VLOG_ERR_BADADDR);
            return;
        }
    }

    /* records are numbered from 0, so log with the current SEQ, then bump it */
    logstore_append(dev->vmm->store, dev->seq, dev->level, msg, dev->len);
    dev->seq++;
    dev->bytes += dev->len;
}

/* STAT: write the device's counters INTO the guest's buffer at MSG. This is
 * the one command that writes guest memory, so check the guest really offered
 * enough room at a real RAM address before writing anything. */
static void cmd_stat(struct vlog_device *dev)
{
    if (dev->len < sizeof(struct vlog_stats)) {
        set_error(dev, VLOG_ERR_BADLEN);
        return;
    }

    void *out = vmm_gpa_to_host(dev->vmm, msg_addr(dev), sizeof(struct vlog_stats));
    if (out == NULL) {
        set_error(dev, VLOG_ERR_BADADDR);
        return;
    }

    struct vlog_stats stats = { .records = dev->seq, .bytes = dev->bytes };
    /* memcpy, because the guest's buffer may not be aligned */
    memcpy(out, &stats, sizeof stats);
}

static void run_command(struct vlog_device *dev, uint32_t cmd)
{
    /* set_error() ORs its code into STATUS, so start every command with the
     * error cleared. A command that succeeds leaves it that way. */
    clear_error(dev);

    switch (cmd) {
    case VLOG_CMD_NOP:
        break;
    case VLOG_CMD_LOG:
        cmd_log(dev);
        break;
    case VLOG_CMD_FLUSH:
        logstore_flush(dev->vmm->store);
        break;
    case VLOG_CMD_STAT:
        cmd_stat(dev);
        break;
    default:
        set_error(dev, VLOG_ERR_BADCMD);
        break;
    }
}

/* ---- Your task: the MMIO handlers ------------------------------------ */

uint64_t vlog_device_mmio_read(uc_engine *uc, uint64_t offset,
                               unsigned size, void *user_data)
{
    (void)uc; (void)size;
    struct vlog_device *dev = user_data;

    switch (offset) {
    case VLOG_REG_ID:      return VLOG_MAGIC;
    case VLOG_REG_VERSION: return VLOG_VERSION;
    case VLOG_REG_STATUS:  return dev->status;
    case VLOG_REG_MSG_LO:  return dev->msg_addr_lo;
    case VLOG_REG_MSG_HI:  return dev->msg_addr_hi;
    case VLOG_REG_LEN:     return dev->len;
    case VLOG_REG_LEVEL:   return dev->level;
    case VLOG_REG_SEQ:     return dev->seq;
    default:               return 0; /* CMD is write-only; unknown offsets read 0 */
    }
}

void vlog_device_mmio_write(uc_engine *uc, uint64_t offset,
                            unsigned size, uint64_t value, void *user_data)
{
    (void)uc; (void)size;
    struct vlog_device *dev = user_data;
    uint32_t value32 = (uint32_t)value; /* all registers are 32-bit */

    switch (offset) {
    case VLOG_REG_MSG_LO: dev->msg_addr_lo = value32; break;
    case VLOG_REG_MSG_HI: dev->msg_addr_hi = value32; break;
    case VLOG_REG_LEN:    dev->len = value32;         break;
    case VLOG_REG_LEVEL:  dev->level = value32;       break;
    case VLOG_REG_CMD:    run_command(dev, value32);  break;
    default:              break; /* read-only registers and unknown offsets */
    }
}
