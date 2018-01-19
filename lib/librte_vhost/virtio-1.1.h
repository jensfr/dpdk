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

#endif /* __VIRTIO_PACKED_H */
