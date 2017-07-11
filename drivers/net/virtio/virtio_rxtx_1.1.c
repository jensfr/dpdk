/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2016 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <rte_cycles.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_branch_prediction.h>
#include <rte_mempool.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_prefetch.h>
#include <rte_string_fns.h>
#include <rte_errno.h>
#include <rte_byteorder.h>
#include <rte_cpuflags.h>
#include <rte_net.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_tcp.h>

#include "virtio_logs.h"
#include "virtio_ethdev.h"
#include "virtio_pci.h"
#include "virtqueue.h"
#include "virtio_rxtx.h"

/* Cleanup from completed transmits. */
static inline int
virtio_xmit_cleanup(struct virtqueue *vq)
{
	uint16_t clean_to, idx;
	uint16_t mask = vq->vq_nentries - 1;
	struct vring_desc_1_1 *desc = vq->vq_ring.desc_1_1;
	uint16_t last_cleaned = vq->vq_used_cons_idx - 1;

	clean_to = last_cleaned + vq->vq_rs_thresh;
	if ((desc[clean_to & mask].flags & DESC_HW) != 0) {
		PMD_TX_LOG(DEBUG, "TX descriptor %d is not done",
			clean_to & mask);
		return -1;
	}

	for (idx = last_cleaned + 2; idx < clean_to; idx++)
		desc[idx & mask].flags &= ~DESC_HW;

	vq->vq_used_cons_idx = clean_to + 1;
	vq->vq_free_cnt += vq->vq_rs_thresh;

	return 0;
}

uint16_t
virtio_xmit_pkts_1_1(void *tx_queue, struct rte_mbuf **tx_pkts, uint16_t nb_pkts)
{
	struct virtnet_tx *txvq = tx_queue;
	struct virtqueue *vq = txvq->vq;
	uint16_t i;
	uint16_t head_idx = vq->vq_avail_idx;
	struct vring_desc_1_1 *desc = vq->vq_ring.desc_1_1;
	uint16_t idx;
	struct vq_desc_extra *dxp;
	uint16_t nb_needed, nb_used = vq->vq_nb_used;

	if (unlikely(nb_pkts < 1))
		return nb_pkts;

	PMD_TX_LOG(DEBUG, "%d packets to xmit", nb_pkts);

	if (likely(vq->vq_free_cnt < vq->vq_free_thresh))
		virtio_xmit_cleanup(vq);

	for (i = 0; i < nb_pkts; i++) {
		struct rte_mbuf *txm = tx_pkts[i];
		struct virtio_tx_region *txr = txvq->virtio_net_hdr_mz->addr;

		nb_needed = txm->nb_segs + 1;

		if (unlikely(nb_needed > vq->vq_free_cnt)) {
			if (unlikely(virtio_xmit_cleanup(vq) != 0))
				goto end_of_tx;

			if (unlikely(nb_needed > vq->vq_rs_thresh)) {
				while (nb_needed > vq->vq_free_cnt) {
					if (virtio_xmit_cleanup(vq) != 0)
						goto end_of_tx;
				}
			}
		}

		txvq->stats.bytes += txm->pkt_len;

		vq->vq_free_cnt -= nb_needed;

		idx = (vq->vq_avail_idx++) & (vq->vq_nentries - 1);
		dxp = &vq->vq_descx[idx];
		if (dxp->cookie != NULL)
			rte_pktmbuf_free(dxp->cookie);
		dxp->cookie = txm;

		desc[idx].addr  = txvq->virtio_net_hdr_mem +
				  RTE_PTR_DIFF(&txr[idx].tx_hdr, txr);
		desc[idx].len   = vq->hw->vtnet_hdr_size;
		desc[idx].flags = VRING_DESC_F_NEXT;
		if (i != 0)
			desc[idx].flags |= DESC_HW;
		nb_used = (nb_used + 1) & ~vq->vq_rs_thresh;
		if (nb_used == 0 || nb_used == 1)
			desc[idx].flags |= DESC_WB;

		do {
			idx = (vq->vq_avail_idx++) & (vq->vq_nentries - 1);
			desc[idx].addr  = VIRTIO_MBUF_DATA_DMA_ADDR(txm, vq);
			desc[idx].len   = txm->data_len;
			desc[idx].flags = DESC_HW | VRING_DESC_F_NEXT;
			nb_used = (nb_used + 1) & ~vq->vq_rs_thresh;
			if (nb_used == 0 || nb_used == 1)
				desc[idx].flags |= DESC_WB;
		} while ((txm = txm->next) != NULL);

		desc[idx].flags &= ~VRING_DESC_F_NEXT;
	}

end_of_tx:
	if (likely(i)) {
		rte_smp_wmb();
		vq->vq_ring.desc_1_1[head_idx & (vq->vq_nentries - 1)].flags |= DESC_HW;
	}

	vq->vq_nb_used = nb_used;

	txvq->stats.packets += i;
	txvq->stats.errors  += nb_pkts - i;

	return i;
}
