/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2016-2018 Microsoft Corporation
 * Copyright(c) 2013-2016 Brocade Communications Systems, Inc.
 * All rights reserved.
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#include <rte_ethdev.h>
#include <rte_memcpy.h>
#include <rte_string_fns.h>
#include <rte_memzone.h>
#include <rte_malloc.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_ether.h>
#include <rte_ethdev_driver.h>
#include <rte_cycles.h>
#include <rte_errno.h>
#include <rte_memory.h>
#include <rte_eal.h>
#include <rte_dev.h>
#include <rte_bus_vmbus.h>

#include "hn_logs.h"
#include "hn_var.h"
#include "hn_rndis.h"
#include "hn_nvs.h"
#include "ndis.h"

#define HN_TX_OFFLOAD_CAPS (DEV_TX_OFFLOAD_IPV4_CKSUM | \
			    DEV_TX_OFFLOAD_TCP_CKSUM  | \
			    DEV_TX_OFFLOAD_UDP_CKSUM  | \
			    DEV_TX_OFFLOAD_TCP_TSO    | \
			    DEV_TX_OFFLOAD_MULTI_SEGS | \
			    DEV_TX_OFFLOAD_VLAN_INSERT)

#define HN_RX_OFFLOAD_CAPS (DEV_RX_OFFLOAD_CHECKSUM | \
			    DEV_RX_OFFLOAD_VLAN_STRIP | \
			    DEV_RX_OFFLOAD_CRC_STRIP)

int hn_logtype_init;
int hn_logtype_driver;

struct hn_xstats_name_off {
	char name[RTE_ETH_XSTATS_NAME_SIZE];
	unsigned int offset;
};

static const struct hn_xstats_name_off hn_stat_strings[] = {
	{ "good_packets",           offsetof(struct hn_stats, packets) },
	{ "good_bytes",             offsetof(struct hn_stats, bytes) },
	{ "errors",                 offsetof(struct hn_stats, errors) },
	{ "allocation_failed",      offsetof(struct hn_stats, nomemory) },
	{ "multicast_packets",      offsetof(struct hn_stats, multicast) },
	{ "broadcast_packets",      offsetof(struct hn_stats, broadcast) },
	{ "undersize_packets",      offsetof(struct hn_stats, size_bins[0]) },
	{ "size_64_packets",        offsetof(struct hn_stats, size_bins[1]) },
	{ "size_65_127_packets",    offsetof(struct hn_stats, size_bins[2]) },
	{ "size_128_255_packets",   offsetof(struct hn_stats, size_bins[3]) },
	{ "size_256_511_packets",   offsetof(struct hn_stats, size_bins[4]) },
	{ "size_512_1023_packets",  offsetof(struct hn_stats, size_bins[5]) },
	{ "size_1024_1518_packets", offsetof(struct hn_stats, size_bins[6]) },
	{ "size_1519_max_packets",  offsetof(struct hn_stats, size_bins[7]) },
};

static struct rte_eth_dev *
eth_dev_vmbus_allocate(struct rte_vmbus_device *dev, size_t private_data_size)
{
	struct rte_eth_dev *eth_dev;
	const char *name;

	if (!dev)
		return NULL;

	name = dev->device.name;

	if (rte_eal_process_type() == RTE_PROC_PRIMARY) {
		eth_dev = rte_eth_dev_allocate(name);
		if (!eth_dev) {
			PMD_DRV_LOG(NOTICE, "can not allocate rte ethdev");
			return NULL;
		}

		if (private_data_size) {
			eth_dev->data->dev_private =
				rte_zmalloc_socket(name, private_data_size,
						     RTE_CACHE_LINE_SIZE, dev->device.numa_node);
			if (!eth_dev->data->dev_private) {
				PMD_DRV_LOG(NOTICE, "can not allocate driver data");
				rte_eth_dev_release_port(eth_dev);
				return NULL;
			}
		}
	} else {
		eth_dev = rte_eth_dev_attach_secondary(name);
		if (!eth_dev) {
			PMD_DRV_LOG(NOTICE, "can not attach secondary");
			return NULL;
		}
	}

	eth_dev->device = &dev->device;
	eth_dev->intr_handle = &dev->intr_handle;

	return eth_dev;
}

static void
eth_dev_vmbus_release(struct rte_eth_dev *eth_dev)
{
	/* free ether device */
	rte_eth_dev_release_port(eth_dev);

	if (rte_eal_process_type() == RTE_PROC_PRIMARY)
		rte_free(eth_dev->data->dev_private);

	eth_dev->data->dev_private = NULL;

	/*
	 * Secondary process will check the name to attach.
	 * Clear this field to avoid attaching a released ports.
	 */
	eth_dev->data->name[0] = '\0';

	eth_dev->device = NULL;
	eth_dev->intr_handle = NULL;
}

/* Update link status.
 * Note: the DPDK definition of "wait_to_complete"
 *   means block this call until link is up.
 *   which is not worth supporting.
 */
static int
hn_dev_link_update(struct rte_eth_dev *dev,
		   __rte_unused int wait_to_complete)
{
	struct hn_data *hv = dev->data->dev_private;
	struct rte_eth_link link, old;
	int error;

	old = dev->data->dev_link;

	error = hn_rndis_get_linkstatus(hv);
	if (error)
		return error;

	hn_rndis_get_linkspeed(hv);

	link = (struct rte_eth_link) {
		.link_duplex = ETH_LINK_FULL_DUPLEX,
		.link_autoneg = ETH_LINK_SPEED_FIXED,
		.link_speed = hv->link_speed / 10000,
	};

	if (hv->link_status == NDIS_MEDIA_STATE_CONNECTED)
		link.link_status = ETH_LINK_UP;
	else
		link.link_status = ETH_LINK_DOWN;

	if (old.link_status == link.link_status)
		return 0;

	PMD_INIT_LOG(DEBUG, "Port %d is %s", dev->data->port_id,
		     (link.link_status == ETH_LINK_UP) ? "up" : "down");

	return rte_eth_linkstatus_set(dev, &link);
}

static void hn_dev_info_get(struct rte_eth_dev *dev,
			    struct rte_eth_dev_info *dev_info)
{
	struct hn_data *hv = dev->data->dev_private;

	dev_info->speed_capa = ETH_LINK_SPEED_10G;
	dev_info->min_rx_bufsize = HN_MIN_RX_BUF_SIZE;
	dev_info->max_rx_pktlen  = HN_MAX_XFER_LEN;
	dev_info->max_mac_addrs  = 1;

	dev_info->hash_key_size = NDIS_HASH_KEYSIZE_TOEPLITZ;
	dev_info->flow_type_rss_offloads =
		ETH_RSS_IPV4 | ETH_RSS_IPV6 | ETH_RSS_TCP | ETH_RSS_UDP;

	dev_info->max_rx_queues = hv->max_queues;
	dev_info->max_tx_queues = hv->max_queues;

	hn_rndis_get_offload(hv, dev_info);
}

static void
hn_dev_promiscuous_enable(struct rte_eth_dev *dev)
{
	struct hn_data *hv = dev->data->dev_private;

	hn_rndis_set_rxfilter(hv, NDIS_PACKET_TYPE_PROMISCUOUS);
}

static void
hn_dev_promiscuous_disable(struct rte_eth_dev *dev)
{
	struct hn_data *hv = dev->data->dev_private;
	uint32_t filter;

	filter = NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_BROADCAST;
	if (dev->data->all_multicast)
		filter |= NDIS_PACKET_TYPE_ALL_MULTICAST;
	hn_rndis_set_rxfilter(hv, filter);
}

static void
hn_dev_allmulticast_enable(struct rte_eth_dev *dev)
{
	struct hn_data *hv = dev->data->dev_private;

	hn_rndis_set_rxfilter(hv, NDIS_PACKET_TYPE_DIRECTED |
			      NDIS_PACKET_TYPE_ALL_MULTICAST |
			NDIS_PACKET_TYPE_BROADCAST);
}

static void
hn_dev_allmulticast_disable(struct rte_eth_dev *dev)
{
	struct hn_data *hv = dev->data->dev_private;

	hn_rndis_set_rxfilter(hv, NDIS_PACKET_TYPE_DIRECTED |
			     NDIS_PACKET_TYPE_BROADCAST);
}

/* Setup shared rx/tx queue data */
static int hn_subchan_configure(struct hn_data *hv,
				uint32_t subchan)
{
	struct vmbus_channel *primary = hn_primary_chan(hv);
	int err;
	unsigned int retry = 0;

	PMD_DRV_LOG(DEBUG,
		    "open %u subchannels", subchan);

	/* Send create sub channels command */
	err = hn_nvs_alloc_subchans(hv, &subchan);
	if (err)
		return  err;

	while (subchan > 0) {
		struct vmbus_channel *new_sc;
		uint16_t chn_index;

		err = rte_vmbus_subchan_open(primary, &new_sc);
		if (err == -ENOENT && ++retry < 1000) {
			/* This can happen if not ready yet */
			rte_delay_ms(10);
			continue;
		}

		if (err) {
			PMD_DRV_LOG(ERR,
				    "open subchannel failed: %d", err);
			return err;
		}

		rte_vmbus_set_latency(hv->vmbus, new_sc,
				      HN_CHAN_LATENCY_NS);

		retry = 0;
		chn_index = rte_vmbus_sub_channel_index(new_sc);
		if (chn_index == 0 || chn_index > hv->max_queues) {
			PMD_DRV_LOG(ERR,
				    "Invalid subchannel offermsg channel %u",
				    chn_index);
			return -EIO;
		}

		PMD_DRV_LOG(DEBUG, "new sub channel %u", chn_index);
		hv->channels[chn_index] = new_sc;
		--subchan;
	}

	return err;
}

static int hn_dev_configure(struct rte_eth_dev *dev)
{
	const struct rte_eth_conf *dev_conf = &dev->data->dev_conf;
	const struct rte_eth_rxmode *rxmode = &dev_conf->rxmode;
	const struct rte_eth_txmode *txmode = &dev_conf->txmode;

	const struct rte_eth_rss_conf *rss_conf =
		&dev_conf->rx_adv_conf.rss_conf;
	struct hn_data *hv = dev->data->dev_private;
	uint64_t unsupported;
	int err, subchan;

	PMD_INIT_FUNC_TRACE();

	unsupported = txmode->offloads & ~HN_TX_OFFLOAD_CAPS;
	if (unsupported) {
		PMD_DRV_LOG(NOTICE,
			    "unsupported TX offload: %#" PRIx64,
			    unsupported);
		return -EINVAL;
	}

	unsupported = rxmode->offloads & ~HN_RX_OFFLOAD_CAPS;
	if (unsupported) {
		PMD_DRV_LOG(NOTICE,
			    "unsupported RX offload: %#" PRIx64,
			    rxmode->offloads);
		return -EINVAL;
	}

	err = hn_rndis_conf_offload(hv, txmode->offloads,
				    rxmode->offloads);
	if (err) {
		PMD_DRV_LOG(NOTICE,
			    "offload configure failed");
		return err;
	}

	hv->num_queues = RTE_MAX(dev->data->nb_rx_queues,
				 dev->data->nb_tx_queues);
	subchan = hv->num_queues - 1;
	if (subchan > 0) {
		err = hn_subchan_configure(hv, subchan);
		if (err) {
			PMD_DRV_LOG(NOTICE,
				    "subchannel configuration failed");
			return err;
		}

		err = hn_rndis_conf_rss(hv, rss_conf);
		if (err) {
			PMD_DRV_LOG(NOTICE,
				    "rss configuration failed");
			return err;
		}
	}

	return 0;
}

static int hn_dev_stats_get(struct rte_eth_dev *dev,
			    struct rte_eth_stats *stats)
{
	unsigned int i;

	for (i = 0; i < dev->data->nb_tx_queues; i++) {
		const struct hn_tx_queue *txq = dev->data->tx_queues[i];

		if (!txq)
			continue;

		stats->opackets += txq->stats.packets;
		stats->obytes += txq->stats.bytes;
		stats->oerrors += txq->stats.errors + txq->stats.nomemory;

		if (i < RTE_ETHDEV_QUEUE_STAT_CNTRS) {
			stats->q_opackets[i] = txq->stats.packets;
			stats->q_obytes[i] = txq->stats.bytes;
		}
	}

	for (i = 0; i < dev->data->nb_rx_queues; i++) {
		const struct hn_rx_queue *rxq = dev->data->rx_queues[i];

		if (!rxq)
			continue;

		stats->ipackets += rxq->stats.packets;
		stats->ibytes += rxq->stats.bytes;
		stats->ierrors += rxq->stats.errors;
		stats->imissed += rxq->ring_full;

		if (i < RTE_ETHDEV_QUEUE_STAT_CNTRS) {
			stats->q_ipackets[i] = rxq->stats.packets;
			stats->q_ibytes[i] = rxq->stats.bytes;
		}
	}

	stats->rx_nombuf = dev->data->rx_mbuf_alloc_failed;
	return 0;
}

static void
hn_dev_stats_reset(struct rte_eth_dev *dev)
{
	unsigned int i;

	PMD_INIT_FUNC_TRACE();

	for (i = 0; i < dev->data->nb_tx_queues; i++) {
		struct hn_tx_queue *txq = dev->data->tx_queues[i];

		if (!txq)
			continue;
		memset(&txq->stats, 0, sizeof(struct hn_stats));
	}

	for (i = 0; i < dev->data->nb_rx_queues; i++) {
		struct hn_rx_queue *rxq = dev->data->rx_queues[i];

		if (!rxq)
			continue;

		memset(&rxq->stats, 0, sizeof(struct hn_stats));
		rxq->ring_full = 0;
	}
}

static int
hn_dev_xstats_get_names(struct rte_eth_dev *dev,
			struct rte_eth_xstat_name *xstats_names,
			__rte_unused unsigned int limit)
{
	unsigned int i, t, count = 0;

	PMD_INIT_FUNC_TRACE();

	if (!xstats_names)
		return dev->data->nb_tx_queues * RTE_DIM(hn_stat_strings)
			+ dev->data->nb_rx_queues * RTE_DIM(hn_stat_strings);

	/* Note: limit checked in rte_eth_xstats_names() */
	for (i = 0; i < dev->data->nb_tx_queues; i++) {
		const struct hn_tx_queue *txq = dev->data->tx_queues[i];

		if (!txq)
			continue;

		for (t = 0; t < RTE_DIM(hn_stat_strings); t++)
			snprintf(xstats_names[count++].name,
				 RTE_ETH_XSTATS_NAME_SIZE,
				 "tx_q%u_%s", i, hn_stat_strings[t].name);
	}

	for (i = 0; i < dev->data->nb_rx_queues; i++)  {
		const struct hn_rx_queue *rxq = dev->data->rx_queues[i];

		if (!rxq)
			continue;

		for (t = 0; t < RTE_DIM(hn_stat_strings); t++)
			snprintf(xstats_names[count++].name,
				 RTE_ETH_XSTATS_NAME_SIZE,
				 "rx_q%u_%s", i,
				 hn_stat_strings[t].name);
	}

	return count;
}

static int
hn_dev_xstats_get(struct rte_eth_dev *dev,
		  struct rte_eth_xstat *xstats,
		  unsigned int n)
{
	unsigned int i, t, count = 0;

	const unsigned int nstats =
		dev->data->nb_tx_queues * RTE_DIM(hn_stat_strings)
		+ dev->data->nb_rx_queues * RTE_DIM(hn_stat_strings);
	const char *stats;

	PMD_INIT_FUNC_TRACE();

	if (n < nstats)
		return nstats;

	for (i = 0; i < dev->data->nb_tx_queues; i++) {
		const struct hn_tx_queue *txq = dev->data->tx_queues[i];

		if (!txq)
			continue;

		stats = (const char *)&txq->stats;
		for (t = 0; t < RTE_DIM(hn_stat_strings); t++)
			xstats[count++].value = *(const uint64_t *)
				(stats + hn_stat_strings[t].offset);
	}

	for (i = 0; i < dev->data->nb_rx_queues; i++) {
		const struct hn_rx_queue *rxq = dev->data->rx_queues[i];

		if (!rxq)
			continue;

		stats = (const char *)&rxq->stats;
		for (t = 0; t < RTE_DIM(hn_stat_strings); t++)
			xstats[count++].value = *(const uint64_t *)
				(stats + hn_stat_strings[t].offset);
	}

	return count;
}

static int
hn_dev_start(struct rte_eth_dev *dev)
{
	struct hn_data *hv = dev->data->dev_private;

	PMD_INIT_FUNC_TRACE();

	/* check if lsc interrupt feature is enabled */
	if (dev->data->dev_conf.intr_conf.lsc) {
		PMD_DRV_LOG(ERR, "link status not supported yet");
		return -ENOTSUP;
	}

	return hn_rndis_set_rxfilter(hv,
				     NDIS_PACKET_TYPE_BROADCAST |
				     NDIS_PACKET_TYPE_ALL_MULTICAST |
				     NDIS_PACKET_TYPE_DIRECTED);
}

static void
hn_dev_stop(struct rte_eth_dev *dev)
{
	struct hn_data *hv = dev->data->dev_private;

	PMD_INIT_FUNC_TRACE();

	hn_rndis_set_rxfilter(hv, 0);
}

static void
hn_dev_close(struct rte_eth_dev *dev __rte_unused)
{
	PMD_INIT_LOG(DEBUG, "close");
}

static const struct eth_dev_ops hn_eth_dev_ops = {
	.dev_configure		= hn_dev_configure,
	.dev_start		= hn_dev_start,
	.dev_stop		= hn_dev_stop,
	.dev_close		= hn_dev_close,
	.dev_infos_get		= hn_dev_info_get,
	.txq_info_get		= hn_dev_tx_queue_info,
	.rxq_info_get		= hn_dev_rx_queue_info,
	.promiscuous_enable     = hn_dev_promiscuous_enable,
	.promiscuous_disable    = hn_dev_promiscuous_disable,
	.allmulticast_enable    = hn_dev_allmulticast_enable,
	.allmulticast_disable   = hn_dev_allmulticast_disable,
	.tx_queue_setup		= hn_dev_tx_queue_setup,
	.tx_queue_release	= hn_dev_tx_queue_release,
	.tx_done_cleanup        = hn_dev_tx_done_cleanup,
	.rx_queue_setup		= hn_dev_rx_queue_setup,
	.rx_queue_release	= hn_dev_rx_queue_release,
	.link_update		= hn_dev_link_update,
	.stats_get		= hn_dev_stats_get,
	.xstats_get		= hn_dev_xstats_get,
	.xstats_get_names	= hn_dev_xstats_get_names,
	.stats_reset            = hn_dev_stats_reset,
	.xstats_reset		= hn_dev_stats_reset,
};

/*
 * Setup connection between PMD and kernel.
 */
static int
hn_attach(struct hn_data *hv, unsigned int mtu)
{
	int error;

	/* Attach NVS */
	error = hn_nvs_attach(hv, mtu);
	if (error)
		goto failed_nvs;

	/* Attach RNDIS */
	error = hn_rndis_attach(hv);
	if (error)
		goto failed_rndis;

	/*
	 * NOTE:
	 * Under certain conditions on certain versions of Hyper-V,
	 * the RNDIS rxfilter is _not_ zero on the hypervisor side
	 * after the successful RNDIS initialization.
	 */
	hn_rndis_set_rxfilter(hv, NDIS_PACKET_TYPE_NONE);
	return 0;
failed_rndis:
	hn_nvs_detach(hv);
failed_nvs:
	return error;
}

static void
hn_detach(struct hn_data *hv)
{
	hn_nvs_detach(hv);
	hn_rndis_detach(hv);
}

static int
eth_hn_dev_init(struct rte_eth_dev *eth_dev)
{
	struct hn_data *hv = eth_dev->data->dev_private;
	struct rte_device *device = eth_dev->device;
	struct rte_vmbus_device *vmbus;
	unsigned int rxr_cnt;
	int err, max_chan;

	PMD_INIT_FUNC_TRACE();

	vmbus = container_of(device, struct rte_vmbus_device, device);
	eth_dev->dev_ops = &hn_eth_dev_ops;
	eth_dev->tx_pkt_burst = &hn_xmit_pkts;
	eth_dev->rx_pkt_burst = &hn_recv_pkts;

	/*
	 * for secondary processes, we don't initialize any further as primary
	 * has already done this work.
	 */
	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return 0;

	/* Since Hyper-V only supports one MAC address, just use local data */
	eth_dev->data->mac_addrs = &hv->mac_addr;

	hv->vmbus = vmbus;
	hv->rxbuf_res = &vmbus->resource[HV_RECV_BUF_MAP];
	hv->chim_res  = &vmbus->resource[HV_SEND_BUF_MAP];
	hv->port_id = eth_dev->data->port_id;

	/* Initialize primary channel input for control operations */
	err = rte_vmbus_chan_open(vmbus, &hv->channels[0]);
	if (err)
		return err;

	rte_vmbus_set_latency(hv->vmbus, hv->channels[0],
			      HN_CHAN_LATENCY_NS);

	hv->primary = hn_rx_queue_alloc(hv, 0,
					eth_dev->device->numa_node);

	if (!hv->primary)
		return -ENOMEM;

	err = hn_attach(hv, ETHER_MTU);
	if  (err)
		goto failed;

	err = hn_tx_pool_init(eth_dev);
	if (err)
		goto failed;

	err = hn_rndis_get_eaddr(hv, hv->mac_addr.addr_bytes);
	if (err)
		goto failed;

	max_chan = rte_vmbus_max_channels(vmbus);
	PMD_INIT_LOG(DEBUG, "VMBus max channels %d", max_chan);
	if (max_chan <= 0)
		goto failed;

	if (hn_rndis_query_rsscaps(hv, &rxr_cnt) != 0)
		rxr_cnt = 1;

	hv->max_queues = RTE_MIN(rxr_cnt, (unsigned int)max_chan);

	return 0;

failed:
	PMD_INIT_LOG(NOTICE, "device init failed");

	hn_detach(hv);
	return err;
}

static int
eth_hn_dev_uninit(struct rte_eth_dev *eth_dev)
{
	struct hn_data *hv = eth_dev->data->dev_private;

	PMD_INIT_FUNC_TRACE();

	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return 0;

	hn_dev_stop(eth_dev);
	hn_dev_close(eth_dev);

	eth_dev->dev_ops = NULL;
	eth_dev->tx_pkt_burst = NULL;
	eth_dev->rx_pkt_burst = NULL;

	hn_detach(hv);
	rte_vmbus_chan_close(hv->primary->chan);
	rte_free(hv->primary);

	eth_dev->data->mac_addrs = NULL;

	return 0;
}

static int eth_hn_probe(struct rte_vmbus_driver *drv __rte_unused,
			struct rte_vmbus_device *dev)
{
	struct rte_eth_dev *eth_dev;
	int ret;

	PMD_INIT_FUNC_TRACE();

	eth_dev = eth_dev_vmbus_allocate(dev, sizeof(struct hn_data));
	if (!eth_dev)
		return -ENOMEM;

	ret = eth_hn_dev_init(eth_dev);
	if (ret)
		eth_dev_vmbus_release(eth_dev);
	else
		rte_eth_dev_probing_finish(eth_dev);

	return ret;
}

static int eth_hn_remove(struct rte_vmbus_device *dev)
{
	struct rte_eth_dev *eth_dev;
	int ret;

	PMD_INIT_FUNC_TRACE();

	eth_dev = rte_eth_dev_allocated(dev->device.name);
	if (!eth_dev)
		return -ENODEV;

	ret = eth_hn_dev_uninit(eth_dev);
	if (ret)
		return ret;

	eth_dev_vmbus_release(eth_dev);
	return 0;
}

/* Network device GUID */
static const rte_uuid_t hn_net_ids[] = {
	/*  f8615163-df3e-46c5-913f-f2d2f965ed0e */
	RTE_UUID_INIT(0xf8615163, 0xdf3e, 0x46c5, 0x913f, 0xf2d2f965ed0eULL),
	{ 0 }
};

static struct rte_vmbus_driver rte_netvsc_pmd = {
	.id_table = hn_net_ids,
	.probe = eth_hn_probe,
	.remove = eth_hn_remove,
};

RTE_PMD_REGISTER_VMBUS(net_netvsc, rte_netvsc_pmd);
RTE_PMD_REGISTER_KMOD_DEP(net_netvsc, "* uio_hv_generic");

RTE_INIT(hn_init_log);
static void
hn_init_log(void)
{
	hn_logtype_init = rte_log_register("pmd.net.netvsc.init");
	if (hn_logtype_init >= 0)
		rte_log_set_level(hn_logtype_init, RTE_LOG_NOTICE);
	hn_logtype_driver = rte_log_register("pmd.net.netvsc.driver");
	if (hn_logtype_driver >= 0)
		rte_log_set_level(hn_logtype_driver, RTE_LOG_NOTICE);
}
