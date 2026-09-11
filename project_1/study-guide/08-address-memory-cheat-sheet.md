# 8. Address & Memory Cheat Sheet

| Address kind | Example | Appears where | Can host C dereference it? |
|---|---:|---|---|
| Guest virtual (GVA) | guest `char *p = 0x101200` | guest C | No |
| Guest physical (GPA) | `MSG=0x101200`, `d.addr` | device/virtqueue | No |
| Host virtual (HVA) | `v->ram + 0x1200` | host C | Yes, after validation |
| MMIO guest address | `0x20000018` | guest load/store | No; Unicorn dispatches callback |
| MMIO callback offset | `0x18` | `device.c` switch | Not an address; register selector |

With paging disabled, GVA `0x101200` equals GPA `0x101200`. Translation computes
offset `0x101200 - 0x100000 = 0x1200`, producing HVA `v->ram + 0x1200`.

Range example: RAM ends at `0x01100000`. GPA `0x010ffff0`, length 16 is valid;
length 17 is invalid. Never validate with `gpa + len <= end` alone because
`UINT64_MAX - 15 + 32` wraps to a small number and might falsely pass. Our
subtraction form makes overflow impossible after the lower-bound check.

Part III adds multiple regions and gaps. A range starting eight bytes before one
region's end with length 64 is invalid even if another region exists later; the
backend has no contiguous HVA covering that range.

