# 15. Final Cheat Sheet

## Architecture

Bare-metal path: guest -> Unicorn MMIO -> device -> translated RAM -> logstore.
Linux path: `/dev/hvc0` -> virtio avail/descriptors -> handler -> sink -> used ring.

## Parts I–III

- I: create/run machine and translate one RAM mapping.
- II: synchronous MMIO register command protocol.
- III: consume standard split virtqueue over multiple memory regions.

## Important functions

`serial_write`, `mem_invalid`, `vmm_create`, `vmm_load_binary`, `vmm_run`,
`vmm_gpa_to_host`, both device MMIO callbacks, `virtq_gpa_to_hva`,
`append_data`, `walk_indirect`, `vlog_virtq_handle`.

## Memory/address terminology

GVA=GPA only because paging is off. GPA is untrusted. HVA is dereferenceable only
after translation. Validate the entire half-open range with remaining-space math.

## MMIO registers

ID 0x00, VERSION 0x04, STATUS 0x08, CMD 0x0c, MSG_LO 0x10, MSG_HI 0x14,
LEN 0x18, LEVEL 0x1c, SEQ 0x20. Commands: NOP 0, LOG 1, FLUSH 2, STAT 3.

## Virtqueue terminology

Descriptors name buffers; avail publishes heads; NEXT chains; WRITE is output
permission; INDIRECT names another table; used returns heads to driver.

## Critical invariants and edge cases

- RAM/message/table ranges must fully fit their mapping.
- No guest address is directly cast/dereferenced.
- Command validation precedes side effects.
- Empty MMIO LOG is valid; Part III translator rejects zero length.
- Records cap at 4096.
- All descriptor indices are checked; all walks are bounded.
- Writable buffers contribute no input.
- Used entry becomes visible before used idx increments.
- Malformed available work is still consumed/completed.

## Test and submission commands

```bash
cd project_1/tests
P1_SEEDS=20 ./run_tests.sh
./run_virtq_tests.sh
cd ../vhost && ./run-qemu.sh       # optional Linux integration
cd ../..
git diff --check
git status --short
```

Submit only `project_1/emulator/vmm.c`, `project_1/emulator/device.c`, and
`project_1/vhost/virtio.c` through the course system.

## 20 most important interview facts

1. Unicorn emulates one paging-disabled x86-64 CPU.
2. RAM is 16 MiB, host-backed, based at `0x00100000`.
3. RIP starts at RAM base; RSP is `BOOTINFO_BASE-16`; RDI points to boot info.
4. Serial and logger live in separate 4 KiB MMIO regions.
5. MMIO callbacks receive region-relative offsets.
6. Poweroff state distinguishes intentional `uc_emu_stop()`.
7. Unmapped accesses become fault state and exit code 77.
8. Guest addresses must be translated, never cast.
9. Remaining-space checks avoid unsigned overflow.
10. Device operand registers persist; CMD commits synchronously.
11. Every CMD clears the previous error first.
12. LOG validates, appends, then increments sequence and byte count.
13. Zero-length LOG is valid and ignores its address.
14. STAT writes exactly an eight-byte structure after two validations.
15. Virtio lets Linux's stock console driver use our backend.
16. Avail publishes descriptor-chain heads; used returns them.
17. Writable descriptors are not request payload.
18. Direct and indirect chains both need bounds and hop caps.
19. The record buffer is capped at 4096 bytes.
20. Release-ordering and used-index advancement prevent the guest from hanging.
