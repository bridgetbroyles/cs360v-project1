# 5. Part II — `emulator/device.c`

## Device architecture and register map

All registers are 32-bit and offsets are relative to `DEV_BASE`:

| Offset | Register | Access | Meaning |
|---:|---|---|---|
| `0x00` | `ID` | RO | `0x31474C56` (`VLG1`) |
| `0x04` | `VERSION` | RO | 1 |
| `0x08` | `STATUS` | RO | READY, ERROR, error code |
| `0x0C` | `CMD` | WO | execute NOP/LOG/FLUSH/STAT |
| `0x10/14` | `MSG_LO/HI` | RW | halves of a 64-bit GPA |
| `0x18` | `LEN` | RW | byte length |
| `0x1C` | `LEVEL` | RW | DEBUG=0, INFO=1, WARN=2, ERROR=3 |
| `0x20` | `SEQ` | RO | successful LOG count |

`struct vlog_device` retains operands across commands. `status` begins READY;
`seq` and `bytes` begin zero. `msg_addr()` rebuilds the address with a cast before
the 32-bit left shift. `clear_error()` removes both ERROR and the old code.

## `vlog_device_mmio_read()`

This is a pure register switch. It returns constants or stored device fields for
the eight readable registers and zero for CMD or unknown offsets. `uc` and `size`
are unused because only aligned 32-bit accesses are required. An array-backed
register bank would be shorter but would blur read-only/computed semantics.

## `vlog_device_mmio_write()`

The first switch stores the low 32 bits of operand writes and returns. Read-only
and unknown offsets return without changing anything. Only CMD proceeds. Before
dispatching every command, it calls `clear_error()`. This is crucial because
`set_error()` ORs bits; clearing first makes STATUS describe only the most recent
command.

- `NOP`: return; error is already cleared.
- `LOG`: reject `len > 4096`; for nonzero length translate exactly the message
  range; reject NULL; append with current `seq` and `level`; increment `seq`; add
  `len` to `bytes`.
- `FLUSH`: call `logstore_flush()` and return.
- `STAT`: require `len >= sizeof(struct vlog_stats)`; translate exactly the
  eight-byte stats output, not the full offered buffer; write `seq` and `bytes`.
- Other command: set `VLOG_ERR_BADCMD`.

Validation happens before log or guest-memory side effects. `LEN == 0` LOG skips
address validation and passes NULL with length zero; `logstore_append()` only
calls `fwrite` when length is nonzero, so this safely creates an empty record.
Unknown levels are accepted and formatted by the store as `LVL?`. Counters only
change on successful LOG.

## LOG trace

```text
guest vlog_log(msg, len, level)
  -> write MSG_LO
  -> write MSG_HI
  -> write LEN
  -> write LEVEL
  -> write CMD=LOG
       clear old error
       len <= 4096?
       len==0 OR vmm_gpa_to_host(MSG,len) succeeds?
       logstore_append(store, seq, level, host_pointer, len)
       seq++, bytes += len
  -> guest reads STATUS and possibly SEQ
```

Example: `MSG=0x00101200`, `LEN=5`, `LEVEL=2`, bytes `hello`, and `SEQ=7`
produces `[7] WARN hello\n`, then `SEQ=8` and `bytes += 5`.

## STAT trace

```text
guest allocates struct vlog_stats in guest RAM
  -> writes its GPA to MSG_LO/HI
  -> writes LEN >= 8
  -> writes CMD=STAT
       clear old error
       verify offered length first
       translate exactly [MSG, MSG+8)
       write { records=seq, bytes=bytes } through HVA
  -> callback returns synchronously
  -> guest reads the now-updated structure
```

LOG reads guest memory; STAT writes guest memory. Checking offered `LEN` before
writing prevents an eight-byte host write into a smaller guest buffer. Translating
only the struct allows a larger offered buffer whose tail crosses RAM, because we
touch only the first eight bytes—exactly the specification.

## Error table and interview answer

| Condition | Result |
|---|---|
| unknown command | ERROR + `BADCMD` (1) |
| nonzero LOG range invalid | ERROR + `BADADDR` (2) |
| LOG length >4096 | ERROR + `BADLEN` (3) |
| STAT offered length <8 | ERROR + `BADLEN` (3), write nothing |
| STAT eight-byte range invalid | ERROR + `BADADDR` (2), write nothing |
| next successful command | clears prior ERROR and code |

“The device is a synchronous stateful register protocol. Operand writes latch
address, length, and level. A CMD write clears the previous error, validates all
guest-controlled ranges, then performs exactly one action. LOG translates and
reads guest bytes into the thread-safe store; STAT validates an output buffer and
writes counters back. Failures neither append nor advance sequence state.”

