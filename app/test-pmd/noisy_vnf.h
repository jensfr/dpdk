#ifndef __NOISY_VNF_H
#define __NOISY_VNF_H
#include <stdint.h>
#include <rte_mbuf.h>
#include <rte_ring.h>
#include "testpmd.h"

/**
 * Add elements to fifo. Return number of written elements
 */
static inline unsigned int
fifo_put(struct rte_ring *r, struct rte_mbuf **data, unsigned num)
{

	return rte_ring_enqueue_burst(r, (void **)data, num, NULL);
}

/**
 * Get elements from fifo. Return number of read elements
 */
static inline unsigned int
fifo_get(struct rte_ring *r, struct rte_mbuf **data, unsigned num)
{
	return rte_ring_dequeue_burst(r, (void **) data, num, NULL);
}

static inline unsigned int
fifo_count(struct rte_ring *r)
{
	return rte_ring_count(r);
}

static inline int
fifo_full(struct rte_ring *r)
{
	return rte_ring_full(r);
}

struct rte_ring *noisy_init(uint32_t qi, uint32_t pi);

#endif
