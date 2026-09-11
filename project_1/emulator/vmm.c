/* vmm.c: STUDENT IMPLEMENTATION FILE for Project 1 (the VMM core).
 *
 * You build the virtual machine monitor: guest RAM, the serial/control device,
 * device MMIO registration, the initial stack pointer, flat-binary loading, the
 * run loop, and guest->host address translation. The logging device itself is
 * in device.c (also yours).
 *
 * WHAT IS PROVIDED (grader/boot glue, leave alone): opening the log file and
 * the Unicorn CPU, the optional instruction tracer, allocating + init-ing the
 * device instance, the boot-parameter pointer (RDI) and blob loader, and
 * teardown. Everything marked TODO is yours.
 *
 * Every TODO is exercised by the basic test suite (a guest can't boot, print,
 * log, or power off without them), so you can develop against `tests/`.
 *
 * See SPEC.md Part I for the machine ABI (memory map, serial protocol, entry
 * state) and the exact contract of each function.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vmm.h"
#include "device.h"

/* ---- Serial / control region ----------------------------------------- */

/* `static inline` so the unused-until-you-wire-them stubs don't warn; you pass
 * their addresses to uc_mmio_map() when you register the serial region. */
static inline uint64_t serial_read(uc_engine *uc, uint64_t offset,
                                   unsigned size, void *user_data)
{
    (void)uc; (void)offset; (void)size; (void)user_data;
    return 0; /* nothing readable */
}

static inline void serial_write(uc_engine *uc, uint64_t offset,
                                unsigned size, uint64_t value, void *user_data)
{
    (void)size;
    struct vmm *v = user_data;

    switch (offset) {
    case SERIAL_TX:
        putchar((unsigned char)value);
        break;
    case SERIAL_POWEROFF:
        v->exit_code = (int)value;
        v->powered_off = 1;
        uc_emu_stop(uc);
        break;
    default:
        break;
    }
}

/* ---- Guest memory faults ---------------------------------------------- */

/* Unicorn calls this when the guest reads/writes/executes an
 * address that is not mapped (no RAM, no MMIO region).
 *   - record the fault in the VMM (v->faulted, v->fault_addr);
 *   - report it on stderr (NOT stdout, which is the guest's console);
 *   - stop the CPU with uc_emu_stop();
 *   - return false, meaning "do not retry the access".
 * `static inline` so it does not warn until you wire it up in vmm_create(). */
static inline bool mem_invalid(uc_engine *uc, uc_mem_type type, uint64_t address,
                               int size, int64_t value, void *user_data)
{
    struct vmm *v = user_data;
    (void)type;
    (void)size;
    (void)value;

    v->faulted = 1;
    v->fault_addr = address;
    fprintf(stderr, "guest memory fault at 0x%llx\n",
            (unsigned long long)address);
    uc_emu_stop(uc);
    return false;
}

/* ---- Optional per-instruction tracing (provided) --------------------- */

static void trace_code(uc_engine *uc, uint64_t address,
                       uint32_t size, void *user_data)
{
    (void)uc; (void)user_data;
    fprintf(stderr, "[trace] rip=0x%08llx (%u bytes)\n",
            (unsigned long long)address, size);
}

/* ---- Lifecycle -------------------------------------------------------- */

int vmm_create(struct vmm *v, int trace, const char *log_path)
{
    memset(v, 0, sizeof *v);
    v->trace = trace;

    /* provided: open the device's host log sink */
    v->store = logstore_open(log_path);
    if (!v->store) {
        fprintf(stderr, "cannot open log file '%s'\n", log_path);
        return -1;
    }

    /* provided: create the emulated x86-64 CPU */
    uc_err err = uc_open(UC_ARCH_X86, UC_MODE_64, &v->uc);
    if (err) {
        fprintf(stderr, "uc_open: %s\n", uc_strerror(err));
        logstore_close(v->store);
        v->store = NULL;
        return -1;
    }

    v->ram = calloc(1, RAM_SIZE);
    if (!v->ram) {
        fprintf(stderr, "out of memory allocating guest RAM\n");
        vmm_destroy(v);
        return -1;
    }
    err = uc_mem_map_ptr(v->uc, RAM_BASE, RAM_SIZE, UC_PROT_ALL, v->ram);
    if (err) {
        fprintf(stderr, "uc_mem_map_ptr: %s\n", uc_strerror(err));
        vmm_destroy(v);
        return -1;
    }

    err = uc_mmio_map(v->uc, SERIAL_BASE, SERIAL_SIZE,
                      serial_read, v, serial_write, v);
    if (err) {
        fprintf(stderr, "uc_mmio_map(serial): %s\n", uc_strerror(err));
        vmm_destroy(v);
        return -1;
    }

    /* provided: allocate and initialize the device instance (its logic lives
     * in device.c) */
    v->dev = calloc(1, sizeof *v->dev);
    if (!v->dev) {
        fprintf(stderr, "out of memory allocating device\n");
        vmm_destroy(v);
        return -1;
    }
    vlog_device_init(v->dev, v);

    err = uc_mmio_map(v->uc, DEV_BASE, DEV_SIZE,
                      vlog_device_mmio_read, v->dev,
                      vlog_device_mmio_write, v->dev);
    if (err) {
        fprintf(stderr, "uc_mmio_map(device): %s\n", uc_strerror(err));
        vmm_destroy(v);
        return -1;
    }

    uint64_t rsp = BOOTINFO_BASE - 16;
    err = uc_reg_write(v->uc, UC_X86_REG_RSP, &rsp);
    if (err) {
        fprintf(stderr, "uc_reg_write(RSP): %s\n", uc_strerror(err));
        vmm_destroy(v);
        return -1;
    }

    /* provided: boot-parameter pointer. The guest receives BOOTINFO_BASE in
     * RDI (its main()'s first argument). Leave this as-is. */
    uint64_t rdi = BOOTINFO_BASE;
    err = uc_reg_write(v->uc, UC_X86_REG_RDI, &rdi);
    if (err) {
        fprintf(stderr, "uc_reg_write(RDI): %s\n", uc_strerror(err));
        vmm_destroy(v);
        return -1;
    }

    uc_hook h;
    err = uc_hook_add(v->uc, &h, UC_HOOK_MEM_UNMAPPED,
                      mem_invalid, v, 1, 0);
    if (err) {
        fprintf(stderr, "uc_hook_add(unmapped): %s\n", uc_strerror(err));
        vmm_destroy(v);
        return -1;
    }

    /* provided: optional instruction tracing (--trace) */
    if (trace) {
        uc_hook h;
        uc_hook_add(v->uc, &h, UC_HOOK_CODE, trace_code, NULL,
                    RAM_BASE, RAM_BASE + RAM_SIZE - 1);
    }
    return 0;
}

int vmm_load_binary(struct vmm *v, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror("fopen guest binary");
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        perror("fseek guest binary");
        fclose(f);
        return -1;
    }
    long sz = ftell(f);
    if (sz < 0 || (uint64_t)sz > RAM_SIZE) {
        fprintf(stderr, "guest binary too large or unreadable\n");
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        perror("fseek guest binary");
        fclose(f);
        return -1;
    }
    size_t n = fread(v->ram, 1, (size_t)sz, f);
    if (n != (size_t)sz || ferror(f)) {
        fprintf(stderr, "short read loading guest binary\n");
        fclose(f);
        return -1;
    }
    fclose(f);

    uint64_t rip = RAM_BASE;
    uc_err err = uc_reg_write(v->uc, UC_X86_REG_RIP, &rip);
    if (err) {
        fprintf(stderr, "uc_reg_write(RIP): %s\n", uc_strerror(err));
        return -1;
    }
    return 0;
}

/* provided: boot-parameter blob loader (used by the test harness via
 * --bootinfo). Blits the opaque blob into the reserved region at the top of
 * RAM; the guest reads it through the RDI pointer set in vmm_create(). */
int vmm_load_bootinfo(struct vmm *v, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror("fopen bootinfo");
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || (uint64_t)sz > BOOTINFO_SIZE) {
        fprintf(stderr, "bootinfo blob too large or unreadable\n");
        fclose(f);
        return -1;
    }
    size_t n = fread(v->ram + (BOOTINFO_BASE - RAM_BASE), 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) {
        fprintf(stderr, "short read loading bootinfo\n");
        return -1;
    }
    return 0;
}

int vmm_run(struct vmm *v)
{
    uc_err err = uc_emu_start(v->uc, RAM_BASE, 0, 0, 0);

    if (v->faulted)
        return VMM_EXIT_FAULT;
    if (err && !v->powered_off) {
        fprintf(stderr, "uc_emu_start: %s\n", uc_strerror(err));
        return 1;
    }
    return v->powered_off ? v->exit_code : 1;
}

void vmm_destroy(struct vmm *v)
{
    if (v->uc) uc_close(v->uc);
    if (v->store) logstore_close(v->store);
    free(v->ram);
    free(v->dev);
    v->uc = NULL;
    v->store = NULL;
    v->ram = NULL;
    v->dev = NULL;
}

void *vmm_gpa_to_host(struct vmm *v, uint64_t gpa, uint64_t len)
{
    uint64_t ram_end = RAM_BASE + RAM_SIZE;

    if (!v || !v->ram || gpa < RAM_BASE || gpa > ram_end)
        return NULL;
    if (len > ram_end - gpa)
        return NULL;
    return v->ram + (size_t)(gpa - RAM_BASE);
}
