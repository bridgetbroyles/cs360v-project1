# 3. Core Systems Concepts

**Host versus guest.** Host code can use libc, files, and real pointers. Guest
code executes inside Unicorn or QEMU and cannot safely hand the host a directly
dereferenceable pointer. In Part II, `dev->vmm->store` is host state while
`MSG_LO/HI` describe a guest address.

**VMM and CPU emulation.** `struct vmm` owns a Unicorn `uc_engine`. Unicorn
executes x86-64 instructions and calls our code when execution touches mapped
MMIO or unmapped memory. We are not implementing an instruction decoder.

**Guest RAM and mapping.** `calloc(1, RAM_SIZE)` creates host memory. Then
`uc_mem_map_ptr()` maps that exact buffer at guest address `RAM_BASE`. This is
host-backed RAM: both Unicorn and `device.c` see the same bytes.

**GVA, GPA, and HVA.** A guest virtual address (GVA) is what guest code uses; a
guest physical address (GPA) is what the emulated machine exposes; a host virtual
address (HVA) is a real C pointer in our process. Paging is disabled in Part I,
so GVA equals GPA, but GPA still does not equal HVA. Translation is explicit.

**MMIO and callbacks.** A load/store to `0x10000000` or `0x20000000` is routed by
Unicorn to registered C callbacks. Their `offset` is relative to the region base.
This lets an ordinary guest store act like a device command.

**Bounds checking.** Never test only the starting address. The whole half-open
range `[gpa, gpa + len)` must fit. Our code avoids computing an unsafe sum by
checking `len <= end - gpa` after proving `gpa >= base`.

**QEMU, virtio, and virtqueues.** QEMU runs a real Linux guest and exposes our
vhost-user backend as virtio device ID 3, a console. Linux's stock driver already
knows the protocol. It describes buffers in a descriptor table, publishes heads
in the avail ring, and waits for our used-ring completion.

**Memory barriers.** Shared-memory publication is ordered. `virtq_rmb()` is an
acquire fence after reading `avail->idx`; `virtq_wmb()` is a release fence after
writing the used element and before incrementing `used->idx`.

