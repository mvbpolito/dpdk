/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
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

#ifndef _RTE_ETH_RING_H_
#define _RTE_ETH_RING_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <rte_ring.h>
#include <rte_ethdev.h>

/* XXX: there are some duplicated fields among these two structs */

enum tx_state {NORMAL_TX, CREATION_TX, BYPASS_TX, DESTRUCTION_TX};
enum rx_state {NORMAL_RX, CREATION_RX, BYPASS_RX, DESTRUCTION_RX};

struct rx_ring_queue {
	struct rte_ring *rng;
	uint8_t normal_id;
	uint8_t bypass_id;

	uint16_t nb_rx_desc;
	/**< Number of RX descriptors available for the queue */
	struct rte_eth_rxconf rx_conf;
	/**< Copy of RX configuration structure for queue */
	struct rte_mempool *mb_pool;
	/**< Reference to mbuf pool to use for RX queue */

	enum rx_state state;

	uint64_t rx_pkts;
	//uint64_t rx_bytes;
	uint64_t rx_pkts_bypass; /* packets read from the bypass channel */
};

struct tx_ring_queue {
	struct rte_ring *rng;
	struct pmd_internals * internals;
	uint8_t normal_id;
	uint8_t bypass_id;

	uint16_t nb_tx_desc;
	/**< Number of TX descriptors available for the queue */
	struct rte_eth_txconf tx_conf;
	/**< Copy of TX configuration structure for queue */

	enum tx_state state;

	uint64_t tx_pkts;
	//uint64_t tx_bytes;
	uint64_t err_pkts;
	uint64_t tx_pkts_bypass; /* packets sent over the bypass port */
	uint64_t err_pkts_bypass;
};

enum port_mode_t {MODE_NORMAL, MODE_BYPASS, MODE_ERROR};
enum bypass_device_state_t {BYPASS_ATTACHED, BYPASS_DETACHED};

struct pmd_internals {
	unsigned nb_rx_queues;
	unsigned nb_tx_queues;

	struct rx_ring_queue rx_ring_queues[RTE_PMD_RING_MAX_RX_RINGS];
	struct tx_ring_queue tx_ring_queues[RTE_PMD_RING_MAX_TX_RINGS];

	enum port_mode_t mode;
	enum bypass_device_state_t bypass_state;

	char bypass_dev[30]; /* pci address of the bypass dev if any */

	struct ether_addr address;
};

/**
 * Create a new ethdev port from a set of rings
 *
 * @param name
 *    name to be given to the new ethdev port
 * @param rx_queues
 *    pointer to array of rte_rings to be used as RX queues
 * @param nb_rx_queues
 *    number of elements in the rx_queues array
 * @param tx_queues
 *    pointer to array of rte_rings to be used as TX queues
 * @param nb_tx_queues
 *    number of elements in the tx_queues array
 * @param numa_node
 *    the numa node on which the memory for this port is to be allocated
 * @return
 *    the port number of the newly created the ethdev or -1 on error.
 */
int rte_eth_from_rings(const char *name,
		struct rte_ring * const rx_queues[],
		const unsigned nb_rx_queues,
		struct rte_ring *const tx_queues[],
		const unsigned nb_tx_queues,
		const unsigned numa_node);

int rte_eth_from_internals(char * name,
	struct pmd_internals * internals, struct rte_pci_device *dev);
int rte_pmd_ring_destroy(const char *name, int destroy_internals);

/**
 * Create a new ethdev port from a ring
 *
 * This function is a shortcut call for rte_eth_from_rings for the
 * case where one wants to take a single rte_ring and use it as though
 * it were an ethdev
 *
 * @param ring
 *    the ring to be used as an ethdev
 * @return
 *    the port number of the newly created ethdev, or -1 on error
 */
int rte_eth_from_ring(struct rte_ring *r);


int rte_eth_ring_add_bypass_device(uint8_t normal_id, uint8_t bypass_id);
int rte_eth_ring_remove_bypass_device(uint8_t normal_id);

#ifdef __cplusplus
}
#endif

#endif
