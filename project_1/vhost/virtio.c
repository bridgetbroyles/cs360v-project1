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
    /* An empty range is not a buffer. */
    if (len == 0)
        return NULL;

    /* gpa + len must not wrap around past 2^64. */
    if (gpa + len < gpa)
        return NULL;

    for (unsigned i = 0; i < mem->nregions; i++) {
        const struct virtq_mem_region *region = &mem->regions[i];

        /* The range must start inside this region... */
        if (gpa < region->gpa || gpa - region->gpa >= region->size)
            continue;

        /* ...and end inside it too. Comparing against the room left in the
         * region (rather than computing gpa + len) cannot overflow. */
        uint64_t offset = gpa - region->gpa;
        if (len > region->size - offset)
            return NULL;

        return region->hva + offset;
    }

    /* Not inside any region: in a gap, or outside guest RAM entirely. */
    return NULL;
}

/* ---- (b) the virtqueue ------------------------------------------------- */

/* Copy one readable data descriptor's bytes onto the end of the record.
 * Skips the descriptor if its address does not translate. Never writes past
 * VIRTQ_MAX_RECORD. Returns the new record length. */
static uint32_t append_data(const struct vring_desc *d, const struct virtq_mem *mem,
                            char *rec, uint32_t rec_len)
{
    const void *src = virtq_gpa_to_hva(mem, d->addr, d->len);
    if (src == NULL)
        return rec_len;

    uint32_t room = VIRTQ_MAX_RECORD - rec_len;
    uint32_t copy_len = (d->len < room) ? d->len : room;
    memcpy(rec + rec_len, src, copy_len);
    return rec_len + copy_len;
}

/* Walk the descriptor chain that starts at index `start` in `table` (which
 * has `table_size` entries), appending every readable data descriptor to the
 * record. `allow_indirect` is 1 for the main descriptor table and 0 inside an
 * indirect table, because indirect tables do not nest. Returns the new record
 * length. */
static uint32_t walk_chain(const struct vring_desc *table, uint32_t table_size,
                           uint32_t start, int allow_indirect,
                           const struct virtq_mem *mem,
                           char *rec, uint32_t rec_len)
{
    uint32_t index = start;

    /* A well-formed chain visits each descriptor at most once, so taking more
     * steps than the table has entries means the guest built a loop. */
    for (uint32_t hops = 0; hops < table_size; hops++) {
        /* The guest picks the index; never read outside the table. */
        if (index >= table_size)
            break;

        /* Take a private copy: the guest can rewrite the table at any time,
         * and we want to check and use the same values. */
        struct vring_desc d = table[index];

        if (d.flags & VRING_DESC_F_INDIRECT) {
            if (allow_indirect) {
                uint32_t entries = d.len / (uint32_t)sizeof(struct vring_desc);
                const struct vring_desc *indirect_table = virtq_gpa_to_hva(
                    mem, d.addr, (uint64_t)entries * sizeof(struct vring_desc));
                /* Only walk a table that is really in guest RAM and properly
                 * aligned (reading a misaligned struct is undefined behavior). */
                if (indirect_table != NULL &&
                    (uintptr_t)indirect_table % _Alignof(struct vring_desc) == 0)
                    rec_len = walk_chain(indirect_table, entries, 0, 0, mem, rec, rec_len);
            }
        } else if (d.flags & VRING_DESC_F_WRITE) {
            /* Space for the device to write into; carries no data for us. */
        } else {
            rec_len = append_data(&d, mem, rec, rec_len);
        }

        if (!(d.flags & VRING_DESC_F_NEXT))
            break;
        index = d.next;
    }
    return rec_len;
}

int vlog_virtq_handle(struct virtq *vq, const struct virtq_mem *mem,
                      struct vlog_sink *sink)
{
    /* No ring to process (and avoid dividing by zero below). */
    if (vq->num == 0)
        return 0;

    /* How many chains the driver has published so far. Read it once, then
     * fence so the ring entries we read next are at least this fresh. */
    uint16_t avail_idx = vq->avail->idx;
    virtq_rmb();

    int completed = 0;
    while (vq->last_avail != avail_idx) {
        uint16_t head = vq->avail->ring[vq->last_avail % vq->num];

        /* Gather the chain's bytes into one record. walk_chain rejects an
         * out-of-range head on its own, leaving the record empty. */
        char rec[VIRTQ_MAX_RECORD];
        uint32_t rec_len = walk_chain(vq->desc, vq->num, head, 1, mem, rec, 0);

        /* The sink ignores an empty record, so a bad chain logs nothing. */
        vlog_sink_emit(sink, rec, rec_len);

        /* Hand the chain back to the driver, even a bad one, so it never
         * waits forever. We wrote nothing into its buffers, so len is 0. */
        uint16_t used_idx = vq->used->idx;
        vq->used->ring[used_idx % vq->num].id = head;
        vq->used->ring[used_idx % vq->num].len = 0;
        virtq_wmb();
        vq->used->idx = (uint16_t)(used_idx + 1);

        vq->last_avail++;
        completed++;
    }
    return completed;
}
