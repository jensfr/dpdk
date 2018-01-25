#ifndef __VIRTIO_PACKED_H
#define __VIRTIO_PACKED_H

#define VRING_DESC_F_NEXT       1
#define VRING_DESC_F_WRITE      2
#define VRING_DESC_F_INDIRECT   4
#define VRING_DESC_F_AVAIL	(1ULL << 7)
#define VRING_DESC_F_USED	(1ULL << 15)

struct vring_desc_packed {
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
desc_is_avail(struct vhost_virtqueue *vq, struct vring_desc_packed *desc)
{
	if (vq->used_wrap_counter == 1) {
		if ((desc->flags & VRING_DESC_F_AVAIL) &&
			!(desc->flags & VRING_DESC_F_USED))
			return 1;
	}
	if (vq->used_wrap_counter == 0) {
		if (!(desc->flags & VRING_DESC_F_AVAIL) &&
			(desc->flags & VRING_DESC_F_USED))
			return 1;
	}
	return 0;
}

static inline void
_set_desc_used(struct vring_desc_packed *desc, int wrap_counter)
{
	uint16_t flags = desc->flags;

	if (wrap_counter == 1) {
		flags |= VRING_DESC_F_USED;
		flags |= VRING_DESC_F_AVAIL;
	} else {
		flags &= ~VRING_DESC_F_USED;
		flags &= ~VRING_DESC_F_AVAIL;
	}

	desc->flags = flags;
}

static inline void
set_desc_used(struct vhost_virtqueue *vq, struct vring_desc_packed *desc)
{
	_set_desc_used(desc, vq->used_wrap_counter);
}

#endif /* __VIRTIO_PACKED_H */
