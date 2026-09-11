# 9. End-to-End Execution Traces

## A. Normal VM boot

`main()` parses options -> `vmm_create()` opens the store/CPU and maps RAM/MMIO ->
`vmm_load_binary()` reads bytes and sets RIP -> optional `vmm_load_bootinfo()`
writes at `BOOTINFO_BASE` -> `vmm_run()` starts Unicorn -> guest `_start` uses the
prepared stack and calls guest `main()` -> guest eventually powers off ->
`vmm_run()` returns its code -> `vmm_destroy()` releases resources.

## B. Serial output

Guest store to `SERIAL_BASE + 0` -> Unicorn recognizes serial MMIO -> passes
offset 0/value to `serial_write()` -> low byte goes to stdout -> guest resumes.

## C. Poweroff

Guest stores code to `SERIAL_BASE + 8` -> callback saves `exit_code`, sets
`powered_off`, stops Unicorn -> `uc_emu_start()` returns -> no fault/error means
`vmm_run()` returns saved code.

## D. Invalid access

Guest reads/writes/executes unmapped GPA -> Unicorn invokes `mem_invalid()` ->
records fault and address, prints stderr, stops, returns false -> run loop sees
`faulted` first -> process exits 77.

## E. MMIO LOG

Guest library splits pointer, writes four operand registers, then CMD=1 ->
callback clears old error -> validates max length -> translates nonempty range ->
store formats `[seq] LEVEL bytes\n` -> device advances `seq` and `bytes` -> guest
observes clean STATUS synchronously.

## F. MMIO STAT

Guest points MSG to writable guest struct and LEN to at least 8 -> CMD=3 ->
callback checks offered size before translation -> validates exactly eight bytes
inside RAM -> writes current counters through translated HVA -> guest reads same
RAM through its original pointer. No log or sequence change.

## G. Virtio request

Linux writes `/dev/hvc0` -> `virtio_console` fills buffers/descriptors -> places
head in avail ring, publishes idx, kicks -> provided backend calls handler ->
handler acquires, walks descriptors, translates/copies readable bytes -> sink
parses `"<level> <message>"` and appends -> handler writes `{head,0}` to used ring,
release-fences, increments used idx -> backend interrupts -> Linux reclaims buffer.

