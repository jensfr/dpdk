#ifndef __VIRTIO_1_1_H
#define __VIRTIO_1_1_H

#define VRING_DESC_F_NEXT       1
#define VRING_DESC_F_WRITE      2
#define VRING_DESC_F_INDIRECT   4

#define VIRTQ_DESC_F_AVAIL      7
#define VIRTQ_DESC_F_USED      15
#define DESC_USED               (1ULL << VIRTQ_DESC_F_USED)
#define DESC_AVAIL              (1ULL << VIRTQ_DESC_F_AVAIL)

struct vring_desc_1_1 {
	uint64_t addr;
	uint32_t len;
	uint16_t index;
	uint16_t flags;
};

static inline void
toggle_wrap_counter(struct vhost_virtqueue *vq)
{
	vq->used_wrap_counter ^= 1;
}

static inline int
desc_is_avail(struct vhost_virtqueue *vq, struct vring_desc_1_1 *desc)
{
	if (!vq)
		return -1;

	if (vq->used_wrap_counter == 1)
		if ((desc->flags & DESC_AVAIL) && !(desc->flags & DESC_USED))
			return 1;
	if (vq->used_wrap_counter == 0)
		if (!(desc->flags & DESC_AVAIL) && (desc->flags & DESC_USED))
			return 1;
	return 0;
}

static inline void
_set_desc_used(struct vring_desc_1_1 *desc, int wrap_counter)
{
	uint16_t flags = desc->flags;

	if (wrap_counter == 1) {
		flags |= DESC_USED;
		flags |= DESC_AVAIL;
	} else {
		flags &= ~DESC_USED;
		flags &= ~DESC_AVAIL;
	}

	desc->flags = flags;
}

static inline void
set_desc_used(struct vhost_virtqueue *vq, struct vring_desc_1_1 *desc)
{
	_set_desc_used(desc, vq->used_wrap_counter);
}

#endif /* __VIRTIO_1_1_H */
