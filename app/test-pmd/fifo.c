#ifdef RTE_TEST_PMD_NOISY
//#include <rte_ring.h>
#include "fifo.h"
//#include "testpmd.h"


struct rte_ring * fifo_init(uint32_t qi)
{
	struct noisy_config *n = &noisy_cfg[qi];

	n->f = rte_ring_create("noisy ring", 1024, rte_socket_id(),
						RING_F_SP_ENQ | RING_F_SC_DEQ);
	return n->f;
}

#endif
