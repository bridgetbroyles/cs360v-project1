# 6. Part III — `vhost/virtio.c`

## Why virtio and why no guest changes

Our custom MMIO ABI needs our custom `guest/vlog.c`. Virtio supplies a standard
ABI. QEMU advertises a vhost-user device with virtio ID 3, the console device;
Linux's existing `virtio_console` driver binds and turns writes to `/dev/hvc0`
into descriptors. QEMU negotiates and maps shared memory through provided
`backend.c`; our code only implements address translation and queue consumption.

## Split virtqueue anatomy

```text
Descriptor table                 Avail ring                  Used ring
+--------------------+           +------------------+        +------------------+
| 0: addr,len,flags  |<--head----| ring[last % num] |        | ring[used % num] |
| 1: addr,len,flags  |           | idx = published  |        |   id=head,len=0  |
| ...                |           +------------------+        | idx = completed  |
+--------------------+                   guest -> device     +------------------+
                                                               device -> guest
```

`VRING_DESC_F_NEXT` says follow `next`. `VRING_DESC_F_WRITE` means the device may
write that buffer; our receive-only queue must not read it. `VRING_DESC_F_INDIRECT`
means `addr/len` describe another descriptor table rather than payload.

Single descriptor:

```text
avail head=2 -> desc[2] -> GPA payload "1 hello\n" -> end
```

Chained descriptors:

```text
head=0 -> desc[0] "1 hel" --NEXT--> desc[3] "lo\n" -> end
                    concatenate: "1 hello\n"
```

Writable descriptors are traversed but contribute no bytes:

```text
readable "1 hi" -> writable scratch (skip) -> readable "\n"
```

Indirect descriptor:

```text
outer desc[4] has INDIRECT
  addr --------------------> indirect table in guest RAM
                              [0] "1 in" -> [1] "direct\n"
```

Indirect tables start at index 0 and may not nest.

## `virtq_gpa_to_hva()`

The real guest map is `struct virtq_mem`: an array of regions, each with GPA
base, size, and HVA base. The function rejects null maps/region tables and
zero-length requests. For each region it:

1. Skips a null HVA or a GPA below that region.
2. Computes `offset = gpa - r->gpa` safely.
3. Accepts only if `offset <= size` and `len <= size - offset`.
4. Returns `r->hva + offset`.

Because `len` is nonzero, an offset exactly at region end fails the remaining
space test. A range cannot combine adjacent regions; it must fit one. Using
subtraction prevents `gpa + len` overflow. This differs intentionally from the
Part I translator: Part III rejects zero-length translations outright.

## Helpers: `append_data()` and `walk_indirect()`

`append_data()` skips writable descriptors, zero-length data, and work after the
record reaches 4096 bytes. It translates the complete descriptor before copying.
If translation fails, it skips the descriptor. It computes remaining capacity,
copies `min(d->len, room)`, and advances `rec_len`. Notice that even when only a
prefix fits, translation validates the descriptor's full declared range first.
That prevents accepting a partly invalid guest descriptor.

`walk_indirect()` rejects writable outer descriptors, empty/misaligned tables,
and untranslated tables. Table length must be a multiple of `sizeof(vring_desc)`.
It caps the count to `UINT16_MAX + 1`, because indirect `next` is only 16 bits,
starts at index zero, bounds-checks every index, snapshots each descriptor into a
local `d`, ignores nested INDIRECT content, follows NEXT, and caps hops at count.
The hop limit stops cycles.

## `vlog_virtq_handle()`—actual control flow

### Inputs and state

It receives queue pointers, the guest memory map, and the log sink. It returns
the number of chains completed. Important state is `vq->num`, `last_avail`,
`avail->idx`, `used->idx`, and each chain's `head`, `index`, `rec`, and `rec_len`.

### Step-by-step

1. Reject null inputs/rings or `num == 0` with zero work.
2. Snapshot `avail_idx = vq->avail->idx`.
3. Execute `virtq_rmb()` before trusting published ring contents.
4. While `last_avail != avail_idx`, read the head from the modulo ring slot.
5. Create a local 4096-byte record and zero its logical length.
6. If `head < num`, walk at most `num` direct descriptors:
   - stop before any out-of-range index;
   - snapshot `vq->desc[index]` into local `d`;
   - INDIRECT calls `walk_indirect`; ordinary data calls `append_data`;
   - stop without NEXT, otherwise assign `index = d.next`.
7. For a valid head, emit the accumulated record once. The sink removes CR/LF,
   parses an optional one-digit level plus space, assigns sequence, and appends.
8. Regardless of a malformed head/chain, publish a used element with the original
   head and `len=0`, because this device wrote zero bytes.
9. Release fence, increment `used->idx`, advance `last_avail`, increment count.
10. Return completed count so the backend knows whether to interrupt the guest.

```text
new avail entry
  -> head bounds check
  -> bounded direct walk
       -> writable? skip
       -> indirect? validate table + bounded nested walk
       -> data? validate full GPA range + bounded copy
  -> one sink emission for valid head
  -> used.ring[used.idx % num] = {head, 0}
  -> release barrier -> used.idx++
  -> last_avail++
```

### Defensive behavior

- Invalid queue pointers/zero size: no dereference.
- Head out of range: no descriptor access or emission, but completion advances.
- Direct next out of range: stop before indexing it; already gathered bytes may
  still be emitted.
- Direct cycle: at most `vq->num` hops.
- Invalid payload GPA or memory gap: skip that descriptor.
- Writable descriptor: never read, preventing data leakage.
- Overlong record: truncate to 4096 without overflowing the stack buffer.
- Invalid indirect address/shape: skip indirect content.
- Indirect next out of range/cycle: bounds/hop limits stop it.
- Nested indirect: ignored because virtio forbids it.

The handler intentionally completes malformed chains. Dropping completion would
let one bad request wedge the driver. An alternative is returning an error and
resetting the whole device, which is stricter but outside this assignment's API.
The avail snapshot means work published during this call waits for a later kick;
re-reading continuously might reduce latency but risks starvation under a busy
producer.

### 30-second interview answer

“We snapshot the avail index with acquire ordering, consume each published head,
walk direct or indirect descriptors with index and hop bounds, skip writable or
unmapped buffers, concatenate at most 4096 bytes, emit once, then return the head
through the used ring with release ordering. Both cursors advance, and the return
count drives interrupts.”

### 2-minute interview answer

Add how GPAs are validated against one memory region, how NEXT and INDIRECT alter
the walk, why writable means skip, why used length is zero, why modulo indexes a
free-running 16-bit counter into a finite ring, and how malformed chains are
bounded and still completed so the guest cannot hang.

