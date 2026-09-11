# 7. Critical Data Structures & State

| Structure/field | Represents | Written by | Read by | Invariant |
|---|---|---|---|---|
| `vmm.uc` | Unicorn CPU | `vmm_create` | all `uc_*` calls | valid until destroy |
| `vmm.ram` | host backing for guest RAM | create/load | Unicorn/device | exactly `RAM_SIZE` bytes |
| `vmm.dev` | MMIO logger instance | create | MMIO callbacks | points back to same VMM |
| `vmm.store` | host log sink | create | device/network | thread-safe, nonnull while running |
| `powered_off/exit_code` | clean-stop state | `serial_write` | `vmm_run` | exit code meaningful after poweroff |
| `faulted/fault_addr` | guest fault state | `mem_invalid` | `vmm_run` | fault takes return priority |
| `vlog_device.status` | READY/error state | init/CMD handler | guest read | old error cleared per command |
| `msg_addr_lo/hi,len,level` | latched operands | guest MMIO writes | CMD handler | persist across commands |
| `seq` | successful LOG count | LOG | guest/STAT/store call | increment once after success |
| `bytes` | successful message byte total | LOG | STAT | excludes formatting bytes |
| `vlog_stats.records/bytes` | guest-visible counters | STAT device | guest | exactly two `uint32_t`s |
| `virtq.desc` | direct descriptor table | guest driver | handler | `num` entries |
| `virtq.avail` | published work | guest | handler | `idx` is free-running count |
| `virtq.used` | returned work | handler | guest | entry visible before idx increment |
| `virtq.num` | ring/table capacity | backend | handler | nonzero, normally power of two |
| `virtq.last_avail` | device cursor | handler | handler | next avail counter to consume |
| `virtq_mem.regions` | GPA-to-HVA mappings | backend | translator | a buffer fits one complete region |
| `vring_desc.addr/len` | guest buffer/table | guest | handler | validate before access |
| `vring_desc.flags/next` | direction/link/type | guest | handler | index checked before follow |
| `vring_avail.idx/ring[]` | heads offered | guest | handler | acquire before contents |
| `vring_used.idx/ring[]` | completions | handler | guest | release before publishing idx |

Important constants: RAM is 16 MiB at `0x00100000`; serial is at `0x10000000`;
device is at `0x20000000`; boot info is the top 64 KiB; fault exit is 77;
messages/virtio records cap at 4096. Descriptor flags are NEXT=1, WRITE=2,
INDIRECT=4.

