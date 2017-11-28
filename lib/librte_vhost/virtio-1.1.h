#ifndef __VIRTIO_1_1_H
#define __VIRTIO_1_1_H

#define __le64  uint64_t
#define __le32  uint32_t
#define __le16  uint16_t

#define VRING_DESC_F_NEXT       1
#define VRING_DESC_F_WRITE      2
#define VRING_DESC_F_INDIRECT   4

#define VIRTQ_DESC_F_AVAIL	7
#define VIRTQ_DESC_F_USED	15
#define DESC_USED (1ULL << VIRTQ_DESC_F_USED)
#define DESC_AVAIL (1ULL << VIRTQ_DESC_F_AVAIL)
#define DESC_HW         0x0080

struct vring_desc_1_1 {
        __le64 addr;
        __le32 len;
        __le16 index;
        __le16 flags;
};

static inline int desc_is_avail(struct vhost_virtqueue *vq, struct vring_desc_1_1 *desc)
{
	if ((vq->avail_wrap_counter == 1) && (desc->flags & DESC_AVAIL))
		return 1;
	if ((vq->avail_wrap_counter == 0) && !(desc->flags & DESC_AVAIL))
		return 1;
	return 0;
}

static inline void
set_desc_used (struct vhost_virtqueue *vq, struct vring_desc_1_1 *desc) {
	if (vq->used_wrap_counter)
		desc->flags |= DESC_USED;
	else
		desc->flags &= ~DESC_USED;
}

static inline void
clear_desc_used(struct vhost_virtqueue *vq, struct vring_desc_1_1 *desc) {
	if ((desc->flags & DESC_AVAIL) ^ (desc->flags & DESC_USED)) { 
		set_desc_used(vq, desc);
		return;
	}
	desc->flags &= ~DESC_USED;
}
#endif /* __VIRTIO_1_1_H */

