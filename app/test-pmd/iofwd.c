/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2014 Intel Corporation
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
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_debug.h>
#include <rte_cycles.h>
#include <rte_memory.h>
#include <rte_launch.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_memcpy.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_string_fns.h>
#include <rte_flow.h>
#include <rte_malloc.h>

#include "testpmd.h"
#include "fifo.h"

static inline void
do_write(char *vnf_mem)
{
	uint64_t i = rte_rand();
	uint64_t w = rte_rand();

	vnf_mem[i % ((vnf_memory_footprint * 1024 * 1024 ) /
			RTE_CACHE_LINE_SIZE)] = w;
}

static inline void
do_read(char *vnf_mem)
{
	uint64_t i = rte_rand();
	uint64_t r = 0;

	r = vnf_mem[i % ((vnf_memory_footprint * 1024 * 1024 ) /
			RTE_CACHE_LINE_SIZE)];
	r++;
}

static inline void
do_rw(char *vnf_mem)
{
	do_read(vnf_mem);
	do_write(vnf_mem);
}

/*
 * Simulate route lookups as defined by commandline parameters
 */
static void
sim_memory_lookups(uint16_t nb_pkts)
{
	char *vnf_mem = rte_zmalloc("vnf sim memory",
			 vnf_memory_footprint * 1024 * 1024,
			 RTE_CACHE_LINE_SIZE);
	uint16_t i,j;

	for (i = 0; i < nb_pkts; i++) {
		for (j = 0; j < nb_rnd_write; j++)
			do_write(vnf_mem);
		for (j = 0; j < nb_rnd_read; j++)
			do_read(vnf_mem);
		for (j = 0; j < nb_rnd_read_write; j++)
			do_rw(vnf_mem);
	}
}

/*
 * Forwarding of packets in I/O mode.
 * Forward packets "as-is".
 * This is the fastest possible forwarding operation, as it does not access
 * to packets data.
 */
static void
pkt_burst_io_forward(struct fwd_stream *fs)
{
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	uint16_t nb_rx;
	uint16_t nb_tx = 0;
	uint32_t retry;

#ifdef RTE_TEST_PMD_RECORD_CORE_CYCLES
	uint64_t start_tsc;
	uint64_t end_tsc;
	uint64_t core_cycles;
#endif
#ifdef RTE_TEST_PMD_NOISY
	const uint64_t freq_khz = rte_get_timer_hz() / 1000;
	struct noisy_config *ncf = &noisy_cfg[fs->tx_queue];
	struct rte_mbuf *tmp_pkts[MAX_PKT_BURST];
	uint16_t nb_enqd;
	uint16_t nb_deqd = 0;
	uint64_t delta_ms;
	uint64_t now;
#endif

#ifdef RTE_TEST_PMD_RECORD_CORE_CYCLES
	start_tsc = rte_rdtsc();
#endif

	/*
	 * Receive a burst of packets and forward them.
	 */
	nb_rx = rte_eth_rx_burst(fs->rx_port, fs->rx_queue,
			pkts_burst, nb_pkt_per_burst);
	if (unlikely(nb_rx == 0))
		return;
	fs->rx_packets += nb_rx;

#ifdef RTE_TEST_PMD_RECORD_BURST_STATS
	fs->rx_burst_stats.pkt_burst_spread[nb_rx]++;
#endif
#ifdef RTE_TEST_PMD_NOISY
	if (bsize_before_send > 0) {
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
			if (nb_enqd < nb_rx)
				nb_rx = nb_enqd;
			if (nb_deqd > 0)
				nb_tx = rte_eth_tx_burst(fs->tx_port,
						fs->tx_queue, tmp_pkts,
						nb_deqd);
		}
	} else {
		nb_tx = rte_eth_tx_burst(fs->tx_port, fs->tx_queue,
				pkts_burst, nb_rx);
	}

	/* simulate noisy vnf by trashing cache lines, simulate route lookups */
	sim_memory_lookups(nb_rx);

	/*
	 * TX burst queue drain
	 */
	if (ncf->prev_time == 0) {
		now = ncf->prev_time = rte_get_timer_cycles();
	} else {
		now = rte_get_timer_cycles();
	}
	delta_ms = (now - ncf->prev_time) / freq_khz;
	if (unlikely(delta_ms >= flush_timer) && flush_timer > 0 && (nb_tx == 0)) {
		while (fifo_count(ncf->f) > 0) {
			nb_deqd = fifo_get(ncf->f, tmp_pkts, nb_rx);
			nb_tx = rte_eth_tx_burst(fs->tx_port, fs->tx_queue,
						 tmp_pkts, nb_deqd);
			if(rte_ring_empty(ncf->f))
				break;
		}
		ncf->prev_time = now;
	}
	if (nb_tx < nb_rx && fs->retry_enabled)
		*pkts_burst = *tmp_pkts;
#else
	nb_tx = rte_eth_tx_burst(fs->tx_port, fs->tx_queue,
			pkts_burst, nb_rx);
#endif
	/*
	 * Retry if necessary
	 */
	if (unlikely(nb_tx < nb_rx) && fs->retry_enabled) {
		retry = 0;
		while (nb_tx < nb_rx && retry++ < burst_tx_retry_num) {
			rte_delay_us(burst_tx_delay_time);
			nb_tx += rte_eth_tx_burst(fs->tx_port, fs->tx_queue,
					&pkts_burst[nb_tx], nb_rx - nb_tx);
		}
	}
	fs->tx_packets += nb_tx;
#ifdef RTE_TEST_PMD_RECORD_BURST_STATS
	fs->tx_burst_stats.pkt_burst_spread[nb_tx]++;
#endif
	if (unlikely(nb_tx < nb_rx)) {
		fs->fwd_dropped += (nb_rx - nb_tx);
		do {
			rte_pktmbuf_free(pkts_burst[nb_tx]);
		} while (++nb_tx < nb_rx);
	}
#ifdef RTE_TEST_PMD_RECORD_CORE_CYCLES
	end_tsc = rte_rdtsc();
	core_cycles = (end_tsc - start_tsc);
	fs->core_cycles = (uint64_t) (fs->core_cycles + core_cycles);
#endif
}

struct fwd_engine io_fwd_engine = {
	.fwd_mode_name  = "io",
	.port_fwd_begin = NULL,
	.port_fwd_end   = NULL,
	.packet_fwd     = pkt_burst_io_forward,
};
