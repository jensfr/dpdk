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
#include "virtio_ring.h"

/* Cleanup from completed transmits. */
static void
virtio_xmit_cleanup(struct virtqueue *vq)
{
	uint16_t idx;
	uint16_t size = vq->vq_nentries;
	struct vring_desc_packed *desc = vq->vq_ring.desc_packed;

	idx = vq->vq_used_cons_idx & (size - 1);
	while (desc_is_used(&desc[idx]) &&
	       vq->vq_free_cnt < size) {
		while (desc[idx].flags & VRING_DESC_F_NEXT) {
			vq->vq_free_cnt++;
			idx = ++vq->vq_used_cons_idx & (size - 1);
		}
		vq->vq_free_cnt++;
		idx = ++vq->vq_used_cons_idx & (size - 1);
	}
}

uint16_t
virtio_xmit_pkts_packed(void *tx_queue, struct rte_mbuf **tx_pkts,
		     uint16_t nb_pkts)
{
	struct virtnet_tx *txvq = tx_queue;
	struct virtqueue *vq = txvq->vq;
	uint16_t i;
	struct vring_desc_packed *desc = vq->vq_ring.desc_packed;
	uint16_t idx;
	struct vq_desc_extra *dxp;

	if (unlikely(nb_pkts < 1))
		return nb_pkts;

	PMD_TX_LOG(DEBUG, "%d packets to xmit", nb_pkts);

	if (likely(vq->vq_free_cnt < vq->vq_free_thresh))
		virtio_xmit_cleanup(vq);

	for (i = 0; i < nb_pkts; i++) {
		struct rte_mbuf *txm = tx_pkts[i];
		struct virtio_tx_region *txr = txvq->virtio_net_hdr_mz->addr;
		uint16_t head_idx;
		int wrap_counter;

		if (unlikely(txm->nb_segs + 1 > vq->vq_free_cnt)) {
			virtio_xmit_cleanup(vq);

			if (unlikely(txm->nb_segs + 1 > vq->vq_free_cnt)) {
				PMD_TX_LOG(ERR,
					   "No free tx descriptors to transmit");
				break;
			}
		}

		txvq->stats.bytes += txm->pkt_len;

		vq->vq_free_cnt -= txm->nb_segs + 1;

		idx = (vq->vq_avail_idx++) & (vq->vq_nentries - 1);
		head_idx = idx;
		wrap_counter = vq->vq_ring.avail_wrap_counter;

		if ((vq->vq_avail_idx & (vq->vq_nentries - 1)) == 0)
			toggle_wrap_counter(&vq->vq_ring);

		dxp = &vq->vq_descx[idx];
		if (dxp->cookie != NULL)
			rte_pktmbuf_free(dxp->cookie);
		dxp->cookie = txm;

		desc[idx].addr  = txvq->virtio_net_hdr_mem +
				  RTE_PTR_DIFF(&txr[idx].tx_hdr, txr);
		desc[idx].len   = vq->hw->vtnet_hdr_size;
		desc[idx].flags |= VRING_DESC_F_NEXT;

		do {
			idx = (vq->vq_avail_idx++) & (vq->vq_nentries - 1);
			desc[idx].addr  = VIRTIO_MBUF_DATA_DMA_ADDR(txm, vq);
			desc[idx].len   = txm->data_len;
			desc[idx].flags |= VRING_DESC_F_NEXT;
			if (idx == (vq->vq_nentries - 1))
				toggle_wrap_counter(&vq->vq_ring);
		} while ((txm = txm->next) != NULL);

		desc[idx].flags &= ~VRING_DESC_F_NEXT;

		rte_smp_wmb();
		_set_desc_avail(&desc[head_idx], wrap_counter);
	}

	txvq->stats.packets += i;
	txvq->stats.errors  += nb_pkts - i;

	return i;
}
