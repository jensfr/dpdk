#ifndef __VIRTIO_1_1_H
#define __VIRTIO_1_1_H

#define VRING_DESC_F_NEXT	1
#define VRING_DESC_F_WRITE	2
#define VRING_DESC_F_INDIRECT	4

#define BATCH_NOT_FIRST 0x0010
#define BATCH_NOT_LAST  0x0020
#define DESC_HW		0x0080
#define VIRTQ_DESC_F_AVAIL	7
#define VIRTQ_DESC_F_USED	15
#define DESC_USED (1ULL << VIRTQ_DESC_F_USED)
#define DESC_AVAIL (1ULL << VIRTQ_DESC_F_AVAIL)

struct vring_desc_1_1 {
        uint64_t addr;
        uint32_t len;
        uint16_t index;
        uint16_t flags;
};

#endif /* __VIRTIO_1_1_H */
