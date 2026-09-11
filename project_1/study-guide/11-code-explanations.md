# 11. Code-Level “Explain This” Preparation

## `len > ram_end - gpa`

It checks whether the requested length exceeds remaining mapped bytes without
forming an overflowing sum. It assumes the preceding check proved `gpa` is not
below RAM and not above the end. Removing the ordering could underflow.

## `(uint64_t)msg_addr_hi << 32`

The cast widens before shifting. Shifting a 32-bit value by 32 would be undefined
or lose the high half. OR combines two nonoverlapping halves.

## `clear_error(dev)` before command switch

It makes every command replace the prior command's error outcome. READY survives
because the mask removes only ERROR and bits 8–15. Without it, `set_error` could
OR error codes together and success would leave stale errors.

## `if (dev->len != 0)` around LOG translation

The specification says an empty message is valid and its address is not examined.
It also makes NULL safe because the store only reads `bytes` when `len > 0`.

## STAT translating `sizeof *stats`, not `dev->len`

The guest's LEN is proof it offered enough capacity. The device only touches the
eight-byte struct, so only that range must map. Translating all LEN would reject a
valid eight-byte output followed by unmapped unused space.

## `avail_idx = avail->idx; virtq_rmb()`

The guest writes descriptors/ring entry before publishing idx. We observe idx,
then acquire ordering before consuming earlier writes. The snapshot bounds this
batch.

## `% vq->num`

Avail and used indices are free-running 16-bit counters; physical ring arrays are
finite and reused circularly. Modulo maps logical counter to slot. It is safe only
after rejecting `num == 0`.

## `struct vring_desc d = vq->desc[index]`

It takes a local snapshot after the bounds check, avoiding multiple inconsistent
field reads if guest memory changes. It does not completely eliminate malicious
races, but it narrows them.

## `take = min(d->len, room)`

It enforces the 4096-byte cap. `room` cannot underflow because the helper returns
when `rec_len == cap` and `rec_len` only grows by at most room.

## Hop limits

Direct walks use at most `vq->num`; indirect walks use at most the bounded table
count. A valid acyclic chain cannot need more entries than exist, while a cycle
must eventually exceed the cap.

## Used-ring publication

The code writes `{id=head,len=0}`, release-fences, then increments idx. `id`
returns the entire chain by its head. `len=0` means the device wrote no bytes into
writable buffers. Reversing order could let the guest see a new idx with stale
entry contents.

