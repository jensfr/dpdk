/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2018 Red Hat Corp.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>

#include <sys/queue.h>
#include <sys/stat.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_debug.h>
#include <rte_cycles.h>
#include <rte_memory.h>
#include <rte_launch.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_memcpy.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_ethdev.h>
#include <rte_flow.h>
#include <rte_malloc.h>

#include "testpmd.h"
#include "noisy_vnf.h"

static inline void
do_write(char *vnf_mem)
{
	uint64_t i = rte_rand();
	uint64_t w = rte_rand();

	vnf_mem[i % ((noisy_lkup_mem_sz * 1024 * 1024) /
			RTE_CACHE_LINE_SIZE)] = w;
}

static inline void
do_read(char *vnf_mem)
{
	uint64_t i = rte_rand();
	uint64_t r;

	r = vnf_mem[i % ((noisy_lkup_mem_sz * 1024 * 1024) /
			RTE_CACHE_LINE_SIZE)];
	r++;
}

static inline void
do_readwrite(char *vnf_mem)
{
	do_read(vnf_mem);
	do_write(vnf_mem);
}

/*
 * Simulate route lookups as defined by commandline parameters
 */
static void
sim_memory_lookups(struct noisy_config *ncf, uint16_t nb_pkts)
{
	uint16_t i, j;
	
	for (i = 0; i < nb_pkts; i++) {
		for (j = 0; j < noisy_lkup_num_writes; j++)
			do_write(ncf->vnf_mem);
		for (j = 0; j < noisy_lkup_num_reads; j++)
			do_read(ncf->vnf_mem);
		for (j = 0; j < noisy_lkup_num_reads_write; j++)
			do_readwrite(ncf->vnf_mem);
	}
}

/*
 * Forwarding of packets in noisy VNF mode.
 * Forward packets but perform memory operations first as specified on cmdline.
 */
static void
pkt_burst_noisy_vnf(struct fwd_stream *fs)
{
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	uint16_t nb_rx;
	uint16_t nb_tx = 0;
	uint32_t retry;
	const uint64_t freq_khz = rte_get_timer_hz() / 1000;
	struct noisy_config *ncf = &noisy_cfg[fs->tx_queue];
	struct rte_mbuf *tmp_pkts[MAX_PKT_BURST];
	uint16_t nb_enqd;
	uint16_t nb_deqd = 0;
	uint64_t delta_ms;
	uint64_t now;

	/*
	 * Receive a burst of packets and forward them.
	 */
	nb_rx = rte_eth_rx_burst(fs->rx_port, fs->rx_queue,
			pkts_burst, nb_pkt_per_burst);
	if (unlikely(nb_rx == 0))
		return;
	fs->rx_packets += nb_rx;

	if (noisy_tx_sw_bufsz > 0) {
		if (rte_ring_free_count(ncf->f) >= nb_rx) {
			/* enqueue into fifo */
			nb_enqd = fifo_put(ncf->f, pkts_burst, nb_rx);
			if (nb_enqd < nb_rx)
				nb_rx = nb_enqd;
		} else {
			/* fifo is full, dequeue first */
			nb_deqd = fifo_get(ncf->f, tmp_pkts, nb_rx);
			/* enqueue into fifo */
			nb_enqd = fifo_put(ncf->f, pkts_burst, nb_deqd);
			sim_memory_lookups(ncf, nb_rx);
			if (nb_enqd < nb_rx)
				nb_rx = nb_enqd;
			if (nb_deqd > 0)
				nb_tx = rte_eth_tx_burst(fs->tx_port,
						fs->tx_queue, tmp_pkts,
						nb_deqd);
		}
	} else {
		sim_memory_lookups(ncf, nb_rx);
		nb_tx = rte_eth_tx_burst(fs->tx_port, fs->tx_queue,
				pkts_burst, nb_rx);
	}

	/*
	 * TX burst queue drain
	 */
	if (ncf->prev_time == 0)
		now = ncf->prev_time = rte_get_timer_cycles();
	else
		now = rte_get_timer_cycles();
	delta_ms = (now - ncf->prev_time) / freq_khz;
	if (unlikely(delta_ms >= noisy_tx_sw_buf_flush_time) && noisy_tx_sw_buf_flush_time > 0 &&
		     (nb_tx == 0)) {
		while (fifo_count(ncf->f) > 0) {
			nb_deqd = fifo_get(ncf->f, tmp_pkts, nb_rx);
			nb_tx = rte_eth_tx_burst(fs->tx_port, fs->tx_queue,
						 tmp_pkts, nb_deqd);
			if (rte_ring_empty(ncf->f))
				break;
		}
		ncf->prev_time = now;
	}
	if (nb_tx < nb_rx && fs->retry_enabled)
		*pkts_burst = *tmp_pkts;

	/*
	 * Retry if necessary
	 */
	if (unlikely(nb_tx < nb_rx) && fs->retry_enabled) {
		retry = 0;
		while (nb_tx < nb_rx && retry++ < burst_tx_retry_num) {
			rte_delay_us(burst_tx_delay_time);
			sim_memory_lookups(ncf, nb_rx);
			nb_tx += rte_eth_tx_burst(fs->tx_port, fs->tx_queue,
					&pkts_burst[nb_tx], nb_rx - nb_tx);
		}
	}
	fs->tx_packets += nb_tx;
	if (unlikely(nb_tx < nb_rx)) {
		fs->fwd_dropped += (nb_rx - nb_tx);
		do {
			rte_pktmbuf_free(pkts_burst[nb_tx]);
		} while (++nb_tx < nb_rx);
	}
}

struct fwd_engine noisy_vnf_engine = {
	.fwd_mode_name  = "noisy",
	.port_fwd_begin = NULL,
	.port_fwd_end   = NULL,
	.packet_fwd     = pkt_burst_noisy_vnf,
};

#define NOISY_STRSIZE 256
#define NOISY_RING "noisy_ring_%d:%d\n"
struct rte_ring *
noisy_init(uint32_t qi, uint32_t pi)
{
	struct noisy_config *n = &noisy_cfg[qi];
	char name[NOISY_STRSIZE];

	if (noisy_lkup_mem_sz == 0)
		return NULL;	

	snprintf(name, NOISY_STRSIZE, NOISY_RING, pi, qi);
	n->f = rte_ring_create(name, noisy_tx_sw_bufsz,
			rte_socket_id(), 0);
	n->vnf_mem = (char *) rte_zmalloc("vnf sim memory",
			 noisy_lkup_mem_sz * 1024 * 1024,
			 RTE_CACHE_LINE_SIZE);
	if (n->vnf_mem == NULL)
		printf("allocating vnf memory failed\n");

	return n->f;
}
