/* virtio.c: STUDENT IMPLEMENTATION FILE for Project 1 (Part III).
 *
 * In Part II you invented a device protocol (MMIO registers). Here you implement
 * the one the whole world actually uses: a virtio split virtqueue, driven by a
 * REAL QEMU virtual machine, whose stock virtio_console driver talks to your
 * code with no guest-side changes at all.
 *
 * The provided backend (backend.c) speaks the vhost-user protocol to QEMU, maps
 * the guest's memory, sets up the rings, and calls you every time the guest
 * kicks the queue. Everything below the seam is yours: TWO functions.
 *
 * WHAT IS PROVIDED: virtq.h (the ring structures, the memory map, the log sink),
 * backend.c (all vhost-user plumbing), run-qemu.sh (boots the VM).
 *
 * ============================================================================
 * TASK (a): virtq_gpa_to_hva(), the hypervisor's address translation.
 * ============================================================================
 * You wrote this once already: vmm_gpa_to_host() in Part I. This is the same
 * contract against a real hypervisor's memory map, except now there can be
 * SEVERAL regions with GAPS between them (see `struct virtq_mem` in virtq.h).
 *
 * Return a host pointer for [gpa, gpa+len), or NULL unless the range lies
 * ENTIRELY inside ONE region. The guest picks these numbers, so reject:
 *   - a range that runs off the end of its region (no straddling two regions),
 *   - an address in a gap between regions, or outside all of them,
 *   - a gpa + len that overflows.
 * Get this wrong and the guest can make your hypervisor read (or crash on)
 * memory that is not its own.
 *
 * ============================================================================
 * TASK (b): vlog_virtq_handle(), the virtqueue.
 * ============================================================================
 * For every chain the guest has made available:
 *
 *   1. Read the head index from the AVAIL ring:
 *          head = vq->avail->ring[vq->last_avail % vq->num]
 *      (First read vq->avail->idx to see how many are ready, and virtq_rmb()
 *      before you trust the ring contents.)
 *   2. Walk the DESCRIPTOR CHAIN from `head`. For each descriptor:
 *        - VRING_DESC_F_WRITE set  -> it is space for the DEVICE to write into.
 *          This queue is guest->host, so it carries no data: skip it.
 *        - VRING_DESC_F_INDIRECT set -> it is not data either! `d->addr` points
 *          at a TABLE of further descriptors in guest memory (`d->len` bytes of
 *          them, so d->len / sizeof(struct vring_desc) entries), which form
 *          their own chain starting at index 0. Translate the table and walk it.
 *          (Indirect tables do not nest.)
 *        - otherwise it is data: translate d->addr with
 *              virtq_gpa_to_hva(mem, d->addr, d->len)
 *          (CHECK FOR NULL) and append its bytes to the record.
 *        - follow d->next while VRING_DESC_F_NEXT is set.
 *      Concatenate the chain's bytes into one record, capped at VIRTQ_MAX_RECORD
 *      (never overflow your buffer).
 *   3. Emit it:  vlog_sink_emit(sink, bytes, len);
 *   4. COMPLETE the chain on the USED ring:
 *          vq->used->ring[vq->used->idx % vq->num] = { .id = head, .len = 0 };
 *          virtq_wmb();
 *          vq->used->idx++;
 *      (This is the virtio equivalent of the Part II STATUS/SEQ readback: it is
 *      how the driver learns its buffer is free again. Get it wrong and the
 *      guest hangs after a few writes.)
 *   5. Advance vq->last_avail and count the chain.
 *
 * Return the number of chains you completed; the backend raises the guest's
 * interrupt when that is > 0.
 *
 * SAFETY: the rings live in memory the GUEST owns and can change at any time. A
 * descriptor index, a chain, or a length can be nonsense. Bounds-check every
 * index against the table size, check every translation, and never loop forever
 * on a cyclic chain. A hypervisor must not be crashable by its guest.
 *
 * See SPEC.md Part III. Develop against `cd tests && ./run_virtq_tests.sh`, then
 * watch it drive a real VM with `cd vhost && ./run-qemu.sh`.
 */
#include "virtq.h"

#include <string.h>

/* ---- (a) guest-physical -> host-virtual -------------------------------- */

void *virtq_gpa_to_hva(const struct virtq_mem *mem, uint64_t gpa, uint64_t len)
{
    if (!mem || !mem->regions || len == 0)
        return NULL;

    for (unsigned i = 0; i < mem->nregions; i++) {
        const struct virtq_mem_region *r = &mem->regions[i];

        if (!r->hva || gpa < r->gpa)
            continue;
        uint64_t offset = gpa - r->gpa;
        if (offset <= r->size && len <= r->size - offset)
            return r->hva + offset;
    }
    return NULL;
}

/* ---- (b) the virtqueue ------------------------------------------------- */

static void append_data(char *rec, uint32_t *rec_len,
                        const struct virtq_mem *mem,
                        const struct vring_desc *d)
{
    if ((d->flags & VRING_DESC_F_WRITE) || d->len == 0 ||
        *rec_len == VIRTQ_MAX_RECORD)
        return;

    const void *src = virtq_gpa_to_hva(mem, d->addr, d->len);
    if (!src)
        return;

    uint32_t room = VIRTQ_MAX_RECORD - *rec_len;
    uint32_t take = d->len < room ? d->len : room;
    memcpy(rec + *rec_len, src, take);
    *rec_len += take;
}

static void walk_indirect(char *rec, uint32_t *rec_len,
                          const struct virtq_mem *mem,
                          const struct vring_desc *outer)
{
    if (outer->flags & VRING_DESC_F_WRITE)
        return;
    if (outer->len == 0 || outer->len % sizeof(struct vring_desc) != 0)
        return;

    const struct vring_desc *table =
        virtq_gpa_to_hva(mem, outer->addr, outer->len);
    if (!table)
        return;

    uint32_t count = outer->len / sizeof(struct vring_desc);
    if (count > UINT16_MAX + 1u)
        count = UINT16_MAX + 1u;
    uint16_t index = 0;
    for (uint32_t hops = 0; hops < count; hops++) {
        if (index >= count)
            break;
        struct vring_desc d = table[index];

        /* Indirect tables cannot contain another indirect descriptor. */
        if (!(d.flags & VRING_DESC_F_INDIRECT))
            append_data(rec, rec_len, mem, &d);
        if (!(d.flags & VRING_DESC_F_NEXT))
            break;
        index = d.next;
    }
}

int vlog_virtq_handle(struct virtq *vq, const struct virtq_mem *mem,
                      struct vlog_sink *sink)
{
    if (!vq || !mem || !sink || !vq->desc || !vq->avail || !vq->used ||
        vq->num == 0)
        return 0;

    uint16_t avail_idx = vq->avail->idx;
    virtq_rmb();
    int completed = 0;

    while (vq->last_avail != avail_idx) {
        uint16_t head = vq->avail->ring[vq->last_avail % vq->num];
        char rec[VIRTQ_MAX_RECORD];
        uint32_t rec_len = 0;

        if (head < vq->num) {
            uint16_t index = head;
            for (uint32_t hops = 0; hops < vq->num; hops++) {
                if (index >= vq->num)
                    break;
                struct vring_desc d = vq->desc[index];

                if (d.flags & VRING_DESC_F_INDIRECT)
                    walk_indirect(rec, &rec_len, mem, &d);
                else
                    append_data(rec, &rec_len, mem, &d);

                if (!(d.flags & VRING_DESC_F_NEXT))
                    break;
                index = d.next;
            }
            vlog_sink_emit(sink, rec, rec_len);
        }

        uint16_t used_idx = vq->used->idx;
        vq->used->ring[used_idx % vq->num] =
            (struct vring_used_elem){ .id = head, .len = 0 };
        virtq_wmb();
        vq->used->idx = (uint16_t)(used_idx + 1);
        vq->last_avail++;
        completed++;
    }
    return completed;
}
