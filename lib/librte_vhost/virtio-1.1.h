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

#endif /* __VIRTIO_PACKED_H */
