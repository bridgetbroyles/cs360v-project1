# 14. Common Mistakes

## Things I Am Likely to Get Asked

- Why `uc_mem_map_ptr` is essential even after `calloc`.
- Why callback offsets are relative, not full addresses.
- Why fault is checked before Unicorn error/poweroff.
- Why zero-length LOG does not validate MSG.
- Why STAT checks both offered length and mapped destination.
- Why writable virtio descriptors are skipped.
- Why indirect tables start at descriptor zero.
- Why both direct and indirect walks need hop bounds.
- Why used-ring ordering and advancement are required.
- Why a range must fit one Part III region.

## Things I Am Likely to Get Wrong

- Saying the custom guest uses paging. It does not; GVA equals GPA.
- Saying GPA equals a host pointer. It never does without translation.
- Adding `DEV_BASE` inside a callback. Unicorn already gives a relative offset.
- Saying all commands reset operands. They persist.
- Saying bad LEVEL is an error. It logs as `LVL?`.
- Saying LEN zero produces no record. Part II produces an empty record.
- Saying `SEQ` advances for STAT/FLUSH/NOP/failure. Only successful LOG advances.
- Saying STAT translates `LEN` bytes. Our code translates only the stats struct.
- Saying `set_error` replaces the old code by itself. Our CMD path clears first.
- Saying writable descriptor means “safe to read.” It means device may write.
- Saying invalid descriptor aborts the whole handler. Ours skips/terminates that
  traversal as appropriate, emits accumulated bytes for a valid head, and completes.
- Saying ring index is always below `num`. The counter is free-running; the slot
  uses modulo.
- Saying INDIRECT data itself is payload. It is a table.
- Saying our handler processes arrivals forever. It snapshots `avail->idx`.
- Saying the QEMU integration was part of grading. It is optional; synthetic
  queue tests are graded.

