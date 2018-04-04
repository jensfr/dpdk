#ifndef __VIRTIO_PACKED_H
#define __VIRTIO_PACKED_H

#define VRING_DESC_F_NEXT       1
#define VRING_DESC_F_WRITE      2
#define VRING_DESC_F_INDIRECT   4

#define VIRTQ_DESC_F_AVAIL      7
#define VIRTQ_DESC_F_USED      15
#define DESC_USED               (1ULL << VIRTQ_DESC_F_USED)
#define DESC_AVAIL              (1ULL << VIRTQ_DESC_F_AVAIL)

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

static inline uint16_t
increase_index (uint16_t index, uint32_t size)
{	
	return ++index >= size ? 0 : index;
}

static inline uint16_t
update_index (struct vhost_virtqueue *vq, uint16_t index, uint32_t size) {
	index = increase_index(index, size);
	if (increase_index(index, size) == 0)
		toggle_wrap_counter(vq);
	return index;
}


static inline int
desc_is_avail(struct vhost_virtqueue *vq, struct vring_desc_packed *desc)
{
	if (unlikely(!vq))
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
_set_desc_used(struct vring_desc_packed *desc, int wrap_counter)
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
set_desc_used(struct vhost_virtqueue *vq, struct vring_desc_packed *desc)
{
	_set_desc_used(desc, vq->used_wrap_counter);
}

#endif /* __VIRTIO_PACKED_H */
