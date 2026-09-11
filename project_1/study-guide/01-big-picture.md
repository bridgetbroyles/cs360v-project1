# 1. Big Picture

## What we built

We built the same logging service through two device interfaces:

1. A tiny x86-64 virtual machine monitor (VMM) using Unicorn.
2. A custom memory-mapped I/O (MMIO) logging device for our bare-metal guest.
3. A real virtio/vhost-user backend that a normal Linux `virtio_console` driver
   can use under QEMU.

The **host** is the machine running the emulator/backend. The **guest** is the
program or Linux VM being run. The VMM gives the bare-metal guest an emulated CPU,
RAM, and devices. MMIO means that special guest addresses represent device
registers rather than ordinary RAM. Virtio is a standardized paravirtual device
protocol; its data path uses shared-memory queues called virtqueues.

```text
Part I + II: bare-metal guest and custom MMIO

 guest C code
    | ordinary pointer (GVA = GPA because paging is off)
    | writes MSG/LEN/LEVEL, then CMD
    v
 Unicorn CPU ---- MMIO dispatch ----> device.c
    |                                  |
    | host-backed RAM                  | vmm_gpa_to_host()
    |                                  v
    +---------------------------- guest message bytes
                                       |
                                       v
                                  logstore_append()
                                       |
                                       v
                                    log file

Part III: Linux and standard virtio

 Linux write("/dev/hvc0")
    |
 virtio_console driver publishes descriptors
    v
 avail ring --kick--> vhost-user backend --> vlog_virtq_handle()
                                           | GPA-to-HVA translation
                                           | concatenate readable buffers
                                           v
                                      vlog_sink_emit()
                                           |
                                           v
                                        log file
                                           |
 used ring <---------------- complete chain
```

There are three parts because they separate three layers of systems work:

- Part I constructs the machine that can execute a guest.
- Part II invents a simple device protocol and attaches it to that machine.
- Part III implements the equivalent service using a real industry protocol.

At the lower level, both device paths solve the same trust-boundary problem:
the guest supplies an address and length, the host validates that range against
the guest memory map, converts it to a host pointer, and only then reads bytes.
Part II signals completion through `STATUS` and `SEQ`; Part III signals completion
through the used ring.

What to learn deeply: the two address translators, command validation, and the
avail/descriptor/used sequence. Memorize: the memory map, registers, flags, and
entry registers. Only recognize: most vhost-user plumbing in `backend.c`, which
was provided.

