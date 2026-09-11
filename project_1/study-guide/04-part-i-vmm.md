# 4. Part I — `emulator/vmm.c`

## Memory map and entry state

```text
0x00100000  RAM_BASE       +-----------------------------+
                              16 MiB host-backed RAM
0x010F0000  BOOTINFO_BASE  | 64 KiB reserved boot info   |
0x01100000  RAM end        +-----------------------------+

0x10000000  SERIAL_BASE    4 KiB serial/control MMIO
0x20000000  DEV_BASE       4 KiB logging-device MMIO
```

Initial `RIP = RAM_BASE`, `RSP = BOOTINFO_BASE - 16`, and
`RDI = BOOTINFO_BASE`.

## `serial_write()`

### Purpose, inputs, state, and flow

Unicorn calls it for writes in the serial MMIO region. Inputs are the engine,
relative `offset`, access `size`, written `value`, and `user_data` (`struct vmm *`).
It returns nothing. At `SERIAL_TX`, it sends `(unsigned char)value` to host stdout.
At `SERIAL_POWEROFF`, it stores `(int)value` in `v->exit_code`, sets
`v->powered_off`, and calls `uc_emu_stop()`. Other offsets are ignored.

### Why, edge cases, and alternatives

The low-byte cast exactly implements the byte-oriented console even though the
callback value is 64 bits. Poweroff state is recorded before stopping so
`vmm_run()` can distinguish intentional termination from an emulator error.
We could buffer console output, but direct `putchar()` is simpler and matches the
specified synchronous device. Removing `uc_emu_stop()` makes the guest continue;
removing `powered_off` makes a clean stop look like failure.

### Interview answer

“`serial_write` is the MMIO callback for our control page. Unicorn gives it a
region-relative offset. Offset zero prints the low byte; offset eight records the
guest's exit code, marks a clean shutdown, and stops emulation. Unknown offsets
are intentionally no-ops.”

## `mem_invalid()`

### Purpose, inputs, state, and flow

This `UC_HOOK_MEM_UNMAPPED` callback contains guest faults. It receives the access
type, address, size, and write value, although our policy only needs the address.
It sets `v->faulted = 1`, records `fault_addr`, reports to stderr, stops Unicorn,
and returns `false`, meaning do not map/retry the access.

### Why, edge cases, and alternatives

The guest is untrusted, so a bad read, write, or instruction fetch must become a
controlled VM result (`77`), not a host crash. stderr preserves stdout as the
guest transcript. An alternative is demand-mapping memory and returning `true`,
appropriate for paging, but it would violate this machine's fixed map.

### Interview answer

“The unmapped-memory hook converts any invalid guest access into explicit VMM
state. It records the address, stops the CPU, returns false so Unicorn will not
retry, and `vmm_run` gives faults priority by returning `VMM_EXIT_FAULT`.”

## `vmm_create()`

### Purpose, inputs/outputs, state, and control flow

It initializes the complete machine. Inputs are a destination `struct vmm`, a
trace flag, and log path; it returns 0 or -1.

1. Zero the struct and store `trace`.
2. Open the host `logstore`.
3. Open Unicorn as `UC_ARCH_X86`, `UC_MODE_64`.
4. Allocate 16 MiB of zeroed host RAM.
5. Map it at `RAM_BASE` with `UC_PROT_ALL` using `uc_mem_map_ptr()`.
6. Map serial MMIO with `serial_read`/`serial_write` and `v` as context.
7. Allocate/init `v->dev`.
8. Map device MMIO with device callbacks and `v->dev` as context.
9. Write `RSP = BOOTINFO_BASE - 16` and `RDI = BOOTINFO_BASE`.
10. Register `mem_invalid` for all unmapped accesses.
11. Optionally register instruction tracing over RAM.

Every checked failure prints a diagnostic and releases acquired state. The
`uc_open` failure closes the store directly; later failures call `vmm_destroy()`.

### Why and what would break

Host-backed mapping avoids copying between Unicorn RAM and device-visible RAM.
Separate callback contexts give serial the whole VMM and the logger its device
instance. The stack is 16-byte aligned and below reserved boot info. Without RAM
mapping, the first instruction fetch faults; without RIP/RSP later setup, code or
C calls fail; without MMIO registration, console/device accesses fault.

An alternative is Unicorn-owned memory plus `uc_mem_read()` per command. It may
encapsulate memory better but adds copying and loses the simple shared backing
pointer. A centralized MMIO dispatcher is another option, but separate mappings
are clearer for two devices.

### 30-second interview answer

“`vmm_create` constructs all host state, opens Unicorn, allocates and maps 16 MiB
of host-backed RAM, registers serial and logger MMIO callbacks, initializes the
device, sets ABI registers RSP and RDI, and installs the unmapped-memory hook.
Each setup failure unwinds resources.”

### 2-minute interview answer

Add that host-backed RAM makes device translation `ram + offset`; MMIO callbacks
receive region-relative offsets and distinct contexts; the stack stops 16 bytes
below boot info; the fault hook covers read/write/fetch; RIP is deliberately set
later by `vmm_load_binary`; and poweroff/fault fields let `vmm_run` classify why
Unicorn stopped.

## `vmm_load_binary()`

### Purpose and flow

It opens a flat binary in binary mode, seeks to determine its length, rejects a
negative/unreadable size or anything larger than `RAM_SIZE`, rewinds, reads
exactly that many bytes into `v->ram`, closes the file, and writes
`UC_X86_REG_RIP = RAM_BASE`. Every file or Unicorn error returns -1.

The image is flat, not ELF: byte zero of the file is byte zero of mapped RAM.
Using an ELF loader would support segments and permissions, but would be needless
and incompatible with the supplied `.bin` format. A short read is rejected so
the guest never runs a partial image.

### Interview answer

“We load the complete flat file directly into the host buffer that backs guest
RAM, after a size check, verify the exact read count, then set RIP to its mapped
base. No second Unicorn copy is needed.”

## `vmm_run()`

It calls `uc_emu_start(v->uc, RAM_BASE, 0, 0, 0)`: start at the RAM base, no end
address, timeout, or instruction count. After stopping, fault state has highest
priority and returns 77. A Unicorn error without clean poweroff returns 1 and a
diagnostic. Otherwise it returns the guest's stored exit code; a stop without
poweroff also returns 1. Checking state rather than only `uc_err` matters because
our callbacks intentionally stop Unicorn.

## `vmm_gpa_to_host()`

### Purpose and actual check

This is the Part II security boundary:

```c
ram_end = RAM_BASE + RAM_SIZE;
reject null VMM/RAM, gpa < RAM_BASE, or gpa > ram_end;
reject len > ram_end - gpa;
return v->ram + (gpa - RAM_BASE);
```

Subtracting after the lower-bound check avoids underflow. Comparing length with
remaining space avoids overflow from `gpa + len`. In this final code,
`gpa == ram_end` is accepted only when `len == 0`, producing a legal one-past C
pointer that must not be dereferenced. A positive length there is rejected.

A naive cast `(void *)gpa` would interpret a guest number in the host address
space and could crash or expose host memory. A generic region table would scale
to fragmented RAM, but Part I has exactly one region; Part III uses that table.

### 30-second interview answer

“It proves the complete GPA range fits in the one RAM mapping, using subtraction
to avoid addition overflow, then returns the corresponding offset into the
host-backed buffer. It never directly casts a guest address.”

