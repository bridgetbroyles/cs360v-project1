# 12. Quiz Preparation

## Quiz

1. What three responsibilities belong to the three implementation files?
2. Why does GVA equal GPA in Part I but not equal HVA?
3. Calculate `BOOTINFO_BASE` and initial RSP.
4. Which serial callback offset stops the VM, and what state changes first?
5. Why does `mem_invalid()` return false?
6. Why map RAM with `uc_mem_map_ptr()` rather than only allocate it?
7. What happens if the guest binary is 16 MiB plus one byte?
8. Is `[RAM end, RAM end)` accepted by our Part I translator? Part III?
9. Why is `len > end - gpa` safer than `gpa + len > end`?
10. Which MMIO registers retain values across commands?
11. Starting with `seq=3`, `bytes=20`, what does a successful zero-length INFO
    LOG do?
12. What happens for LEVEL=9?
13. Why does each CMD begin with `clear_error()`?
14. A STAT has LEN=7 and a valid address. What changes?
15. A STAT has LEN=100 but only its first eight bytes map. Does ours succeed?
16. Contrast avail and used rings.
17. What does each descriptor flag mean?
18. Why must a writable descriptor not be copied into `rec`?
19. A direct chain has valid data then an unmapped descriptor. What does ours do?
20. What prevents a self-referential direct chain from hanging?
21. What makes an indirect table structurally acceptable?
22. Why can indirect descriptor indices never require more than 65,536 entries?
23. Why translate a full data descriptor even if only ten bytes of record room
    remain?
24. What does used element `len=0` mean here?
25. What work published after the avail snapshot receives service when?
26. Predict the output for payload `"2 cache miss\n"` at sink sequence 5.
27. Why complete an out-of-range head at all?
28. Name three production concerns beyond this implementation.

## Answer Key

1. VMM/machine; custom MMIO device; standard virtqueue backend.
2. Paging is disabled, so guest translation is identity, but host memory is a
   different process address space and needs a checked mapping.
3. RAM end is `0x01100000`; boot info begins `0x010F0000`; RSP is `0x010EFFF0`.
4. Relative offset `0x08`; save exit code, set `powered_off`, then stop Unicorn.
5. It tells Unicorn not to retry the invalid access.
6. Allocation creates host bytes; mapping makes guest accesses resolve to them.
7. Load fails before reading/running it.
8. Part I accepts the empty one-past range; Part III rejects every zero length.
9. Unsigned addition can wrap; remaining-space subtraction cannot after bounds.
10. `MSG_LO`, `MSG_HI`, `LEN`, and `LEVEL`.
11. Writes `[3] INFO \n`, changes seq to 4, leaves bytes 20, ignores address.
12. It succeeds and the store prints `LVL?`.
13. So success clears old errors and OR-based `set_error` cannot combine codes.
14. Sets BADLEN; writes nothing; counters/log unchanged.
15. Yes. LEN proves capacity; the device translates/touches exactly eight bytes.
16. Guest publishes chain heads in avail; device returns completed heads in used.
17. NEXT links, WRITE marks device-writable, INDIRECT points to another table.
18. It is not guest-supplied input and reading it can leak unintended bytes.
19. Keeps already collected bytes, skips invalid data, emits once, completes.
20. The direct hop count is capped at `vq->num`.
21. Nonzero length, exact descriptor-size multiple, whole table maps, outer is not
    writable; nested INDIRECT content is ignored.
22. `next` is a `uint16_t`; our code also caps count to `UINT16_MAX + 1`.
23. The guest declared the full range; validating only a prefix could accept a
    malformed/out-of-map descriptor.
24. The device wrote zero bytes into driver-writable buffers.
25. A later handler invocation/kick, because this call uses a fixed snapshot.
26. `[5] WARN cache miss\n`.
27. To return resources and prevent a bad request from wedging the driver.
28. Examples: reset/error protocol, event-index notifications, feature checks,
    stronger concurrency, zero-copy scatter/gather, stricter race handling.

