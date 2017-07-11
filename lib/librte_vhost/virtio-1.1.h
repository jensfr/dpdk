#ifndef __VIRTIO_1_1_H
#define __VIRTIO_1_1_H

#define __le64	uint64_t
#define __le32	uint32_t
#define __le16	uint16_t

#define VRING_DESC_F_NEXT	1
#define VRING_DESC_F_WRITE	2
#define VRING_DESC_F_INDIRECT	4

#define BATCH_NOT_FIRST 0x0010
#define BATCH_NOT_LAST  0x0020
#define DESC_HW		0x0080
#define DESC_WB		0x0100

struct vring_desc_1_1 {
        __le64 addr;
        __le32 len;
        __le16 index;
        __le16 flags;
};

#endif /* __VIRTIO_1_1_H */
