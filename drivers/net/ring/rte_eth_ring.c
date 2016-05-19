/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
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

#include "rte_eth_ring.h"
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_string_fns.h>
#include <rte_dev.h>
#include <rte_kvargs.h>
#include <rte_errno.h>

/* Following code probably only works with Linux */
#include <unistd.h>

#define ETH_RING_NUMA_NODE_ACTION_ARG	"nodeaction"
#define ETH_RING_ACTION_CREATE		"CREATE"
#define ETH_RING_ACTION_ATTACH		"ATTACH"

static const char *valid_arguments[] = {
	ETH_RING_NUMA_NODE_ACTION_ARG,
	NULL
};

static const char *drivername = "Rings PMD";
static struct rte_eth_link pmd_link = {
		.link_speed = 10000,
		.link_duplex = ETH_LINK_FULL_DUPLEX,
		.link_status = 0
};

static int
buf_is_cap(struct rte_mbuf * buf)
{
	if(buf->userdata == buf_is_cap)
		return 1;

	return 0;
}

/* forward declarations to avoid missing declarations */
static uint16_t eth_ring_normal_rx(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs);
static uint16_t eth_ring_normal_tx(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs);

static uint16_t eth_ring_bypass_tx(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs);
static uint16_t eth_ring_bypass_rx(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs);

static uint16_t eth_ring_creation_rx(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs);
static uint16_t eth_ring_destruction_rx(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs);

static uint16_t eth_ring_send_cap_creation_tx(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs);
static uint16_t eth_ring_send_cap_destruction_tx(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs);


/*
 * send a cap (special buffer that indicates that is the last)
 */
static void
send_cap_normal(void *q)
{
	struct tx_ring_queue *tx_q = q;

	if (rte_spinlock_trylock(&tx_q->send_cap_lock) == 0)
		return;

	/* has a cap been already sent?*/
	if(tx_q->cap_sent) {
		rte_spinlock_unlock(&tx_q->send_cap_lock);
		return;
	}

	tx_q->cap_sent = 1;

	/* A horrible method to lock for the memory pool */
	struct rte_eth_dev * normal_port =
		normal_port = &rte_eth_devices[tx_q->normal_id];
	const struct pmd_internals *internal = normal_port->data->dev_private;
	struct rte_mempool *mb_pool = internal->rx_ring_queues[0].mb_pool;

	struct rte_mbuf *caps[5] = {0};

	int ret, i, ntosend;
	do {
		ret = rte_mempool_get_bulk(mb_pool, (void **) caps, 5);
	} while(ret != 0);

	/* it is the way to detect if the packets is an cap.
	 * The userdata filed is fill with a particular memory adress, in this case
	 * buf_is_cap
	 */
	for (i = 0; i < 5; i++)
		caps[i]->userdata = buf_is_cap;

	ntosend = 5;
	i = 0;
	do {
		i += eth_ring_normal_tx(q, caps, ntosend - i);
	} while(i < ntosend);

	rte_spinlock_unlock(&tx_q->send_cap_lock);
}

static void
send_cap_bypass(void *q)
{
	struct tx_ring_queue *tx_q = q;

	if (rte_spinlock_trylock(&tx_q->send_cap_lock) == 0)
		return;

	/* has a cap been already sent?*/
	if(tx_q->cap_sent) {
		rte_spinlock_unlock(&tx_q->send_cap_lock);
		return;
	}

	tx_q->cap_sent = 1;

	/* A horrible method to lock for the memory pool */
	struct rte_eth_dev * normal_port =
		normal_port = &rte_eth_devices[tx_q->normal_id];
	const struct pmd_internals *internal = normal_port->data->dev_private;
	struct rte_mempool *mb_pool = internal->rx_ring_queues[0].mb_pool;

	struct rte_mbuf *caps[5] = {0};

	int ret, i, ntosend;
	do {
		ret = rte_mempool_get_bulk(mb_pool, (void **) caps, 5);
	} while(ret != 0);

	/* it is the way to detect if the packets is an cap.
	 * The userdata filed is fill with a particular memory adress, in this case
	 * buf_is_cap
	 */
	for (i = 0; i < 5; i++)
		caps[i]->userdata = buf_is_cap;

	ntosend = 5;
	i = 0;
	do {
		i += eth_ring_bypass_tx(q, caps, ntosend - i);
	} while(i < ntosend);

	rte_spinlock_unlock(&tx_q->send_cap_lock);
}

/*
 * these two functions receive/send packets using only the primary device
 */
static uint16_t
eth_ring_normal_rx(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs)
{
	void **ptrs = (void *)&bufs[0];
	struct rx_ring_queue *r = q;
	const uint16_t nb_rx = (uint16_t)rte_ring_dequeue_burst(r->rng,
			ptrs, nb_bufs);
	if (r->rng->flags & RING_F_SC_DEQ)
		r->rx_pkts.cnt += nb_rx;
	else
		rte_atomic64_add(&(r->rx_pkts), nb_rx);
	return nb_rx;
}

static uint16_t
eth_ring_normal_tx(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs)
{
	void **ptrs = (void *)&bufs[0];
	struct tx_ring_queue *r = q;
	const uint16_t nb_tx = (uint16_t)rte_ring_enqueue_burst(r->rng,
			ptrs, nb_bufs);
	if (r->rng->flags & RING_F_SP_ENQ) {
		r->tx_pkts.cnt += nb_tx;
		r->err_pkts.cnt += nb_bufs - nb_tx;
	} else {
		rte_atomic64_add(&(r->tx_pkts), nb_tx);
		rte_atomic64_add(&(r->err_pkts), nb_bufs - nb_tx);
	}
	return nb_tx;
}

/*
 * this function sends packets using only the secondary configured port
 */
static uint16_t
eth_ring_bypass_tx(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs)
{
	struct tx_ring_queue *r = q;
	const uint16_t nb_tx = rte_eth_tx_burst(r->bypass_id, 0, bufs, nb_bufs);

	r->tx_pkts.cnt += nb_tx;
	r->err_pkts.cnt += nb_bufs - nb_tx;

	return nb_tx;
}

/*
 * this function reads packets from both links, the primary one and the
 * secondary one
 */
static uint16_t
eth_ring_bypass_rx(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs)
{
	struct rx_ring_queue *r = q;

	/* XXX: in the case the read packets < nb_bufs, read remaining from secondary */
	/* In the case there are packets in the auxiliar link */
	if (unlikely(rte_ring_count(r->rng))) {
		return eth_ring_normal_rx(q, bufs, nb_bufs);
	}

	const uint16_t nb_rx = rte_eth_rx_burst(r->bypass_id, 0, bufs, nb_bufs);

	r->rx_pkts.cnt += nb_rx;

	return nb_rx;
}

//static void
//close_secondary(uint8_t normal_id, uint8_t bypass_id)
//{
//	char name[50];		/*XXX: change 50 by a propper value */
//
//	struct rte_mbuf * temp[1024];
//	uint16_t nb_rx;
//
//	struct rte_eth_dev * normal_port;
//	struct pmd_internals * internals;
//
//	normal_port = &rte_eth_devices[normal_id];
//	internals = normal_port->data->dev_private;
//
//	snprintf(name, sizeof(name), "temp%d", normal_id);
//	internals->temp_ring = rte_ring_create(name, 1024, 0, 0);
//	if(internals->temp_ring == NULL)
//		return;
//
//	/* read packets into the temp ring */
//	nb_rx = rte_eth_rx_burst(bypass_id, 0, temp, 1024);
//	rte_ring_enqueue_bulk(internals->temp_ring, (void **) temp, nb_rx);
//
//	rte_eth_dev_stop(bypass_id);
//	rte_eth_dev_close(bypass_id);
//	rte_eth_dev_detach(bypass_id, name);
//}

//static uint16_t
//eth_ring_tx_close(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs)
//{
//	struct rx_ring_queue * tx_q = q;
//	struct rte_eth_dev * normal_port;
//
//	normal_port = &rte_eth_devices[tx_q->normal_id];
//
//	/* now, the devices behaves in the standard way */
//	normal_port->rx_pkt_burst = eth_ring_temp_rx;
//	normal_port->tx_pkt_burst = eth_ring_normal_tx;
//
//	close_secondary(tx_q->normal_id, tx_q->bypass_id);
//
//	return eth_ring_normal_tx(q, bufs, nb_bufs);
//}
//
//static uint16_t
//eth_ring_rx_close(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs)
//{
//	struct rx_ring_queue * rx_q = q;
//	struct rte_eth_dev * normal_port;
//
//	normal_port = &rte_eth_devices[rx_q->normal_id];
//
//	/* now, the devices behaves in the standard way */
//	normal_port->rx_pkt_burst = eth_ring_temp_rx;
//	normal_port->tx_pkt_burst = eth_ring_normal_tx;
//
//	close_secondary(rx_q->normal_id, rx_q->bypass_id);
//
//	return eth_ring_normal_rx(q, bufs, nb_bufs);
//}



/* this function reads packets from the temp ring until it gets empty, then that
 * happens the device starts reading the primary port
 */
//static uint16_t
//eth_ring_temp_rx(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs)
//{
//	struct rx_ring_queue * rx_q = q;
//	struct rte_eth_dev * normal_port;
//	struct rte_ring * r;
//	struct pmd_internals * internals;
//
//	void **ptrs = (void *)&bufs[0];
//
//	normal_port = &rte_eth_devices[rx_q->normal_id];
//	internals = normal_port->data->dev_private;
//	r = internals->temp_ring;
//
//	/* are there packets in the temporal ring? */
//	if (rte_ring_count(r)) {
//		const uint16_t nb_rx = (uint16_t)rte_ring_dequeue_burst(r,
//			ptrs, nb_bufs);
//
//		rx_q->rx_pkts.cnt += nb_rx;
//
//		return nb_rx;
//	}
//
//	rte_ring_free(r);
//
//	/* now, there are not more packets in the temporal ring, then the device
//	 * behaves in the standard way
//	 */
//	normal_port->rx_pkt_burst = eth_ring_normal_rx;
//
//	return eth_ring_normal_rx(q, bufs, nb_bufs);
//}

/*
 * This function is used for a limited amount of time to read packets while
 * creating a direct path. The function does the following:
 * - reads packets until the cap packet is found
 * - set the "capfound" flag of the device
 * - changes the receive function to eth_ring_bypass_rx
 */
static uint16_t
eth_ring_creation_rx(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs)
{
	struct rx_ring_queue * rx_q = q;
	struct rte_eth_dev * normal_port =
		normal_port = &rte_eth_devices[rx_q->normal_id];
	uint16_t i;

	/* read packets in normal device */
	uint16_t nb_rx = eth_ring_normal_rx(q, bufs, nb_bufs);

	for (i = 0; i < nb_rx; i++) {
		if (buf_is_cap(bufs[i])) {
			/*
			 * this is very important, in the case the device is going to be
			 * closed, send the cap in case it has not been sent
			 */
			send_cap_normal(normal_port->data->tx_queues[0]);
			normal_port->rx_pkt_burst = eth_ring_bypass_rx;
			nb_rx--;
			break;
		}
	}

	rx_q->rx_pkts.cnt += nb_rx;

	return nb_rx;
}

/*
 * This function is used for a limited amount of time to read packes while
 * destroying a direct link. It does:
 * - reads packets using ONLY the bypass device until cap is found
 * - sets the "capfound" flag of the device
 * - changes teh receive function to eth_ring_normal_rx
 */
static uint16_t
eth_ring_destruction_rx(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs)
{
	struct rx_ring_queue * rx_q = q;
	struct rte_eth_dev * normal_port =
		normal_port = &rte_eth_devices[rx_q->normal_id];
	uint16_t i;
	char name[50]; /* XXX: buf size */

	/* we cannot use the eth_ring_bypass_rx here because it will also try to read
	 * the normal device */
	/* read packets in bypass device */
	uint16_t nb_rx = rte_eth_rx_burst(rx_q->bypass_id, 0, bufs, nb_bufs);

	rx_q->rx_pkts.cnt += nb_rx;

	/*
	 * XXX: remove the buffers that are still caps
	 */

	for (i = 0; i < nb_rx; i++) {
		if (buf_is_cap(bufs[i])) {

			/*
			 * this is very important, in the case the device is going to be
			 * closed, send the cap in case it has not been sent
			 */
			send_cap_bypass(normal_port->data->tx_queues[0]);

			normal_port->rx_pkt_burst = eth_ring_normal_rx;
			nb_rx--;
			rte_eth_dev_stop(rx_q->bypass_id);
			rte_eth_dev_close(rx_q->bypass_id);
			rte_eth_dev_detach(rx_q->bypass_id, name);
			break;
		}
	}

	return nb_rx;
}

/*
 * Adding a bypass device is an async task, then it delegates sending the cap
 * to this function. In that way it is never executed within a transmission
 * operation
 */
static uint16_t
eth_ring_send_cap_creation_tx(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs)
{
	struct tx_ring_queue *rx_q = q;
	struct rte_eth_dev * normal_port =
		normal_port = &rte_eth_devices[rx_q->normal_id];
	struct pmd_internals * internals = normal_port->data->dev_private;

	normal_port->tx_pkt_burst = eth_ring_bypass_tx;

	send_cap_normal(q);

	internals->state = STATE_BYPASS;

	return eth_ring_bypass_tx(q, bufs, nb_bufs);
}

static uint16_t
eth_ring_send_cap_destruction_tx(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs)
{
	struct tx_ring_queue *rx_q = q;
	struct rte_eth_dev * normal_port =
		normal_port = &rte_eth_devices[rx_q->normal_id];
	struct pmd_internals * internals = normal_port->data->dev_private;

	normal_port->tx_pkt_burst = eth_ring_normal_tx;

	send_cap_bypass(q);

	internals->state = STATE_NORMAL;

	return eth_ring_normal_tx(q, bufs, nb_bufs);
}

struct thread_args
{
	void (*func)(void *);
	void *fargs;
	int timeout;
};

static void *
thread_task(void *args_)
{
	struct thread_args args = *((struct thread_args *) args_);
	free(args_);

	usleep(args.timeout);
	args.func(args.fargs);
	return NULL;
}

/*
 * creates a thread that executes the func function in time useconds
 */
static int
schedule_timeout(void (*func)(void *), void *fargs, int timeout)
{
	pthread_t tid;
	pthread_attr_t attr;

	struct thread_args * args = malloc(sizeof(*args));
	args->func = func;
	args->fargs = fargs;
	args->timeout = timeout;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	pthread_create(&tid, &attr, thread_task,(void *)args);

	return 0;
}

static int
eth_dev_configure(struct rte_eth_dev *dev __rte_unused) { return 0; }

static int
eth_dev_start(struct rte_eth_dev *dev)
{
	dev->data->dev_link.link_status = 1;
	return 0;
}

static void
eth_dev_stop(struct rte_eth_dev *dev)
{
	dev->data->dev_link.link_status = 0;
}

static int
eth_dev_set_link_down(struct rte_eth_dev *dev)
{
	dev->data->dev_link.link_status = 0;
	return 0;
}

static int
eth_dev_set_link_up(struct rte_eth_dev *dev)
{
	dev->data->dev_link.link_status = 1;
	return 0;
}

static int
eth_rx_queue_setup(struct rte_eth_dev *dev,uint16_t rx_queue_id,
				    uint16_t nb_rx_desc,
				    unsigned int socket_id __rte_unused,
				    const struct rte_eth_rxconf *rx_conf,
				    struct rte_mempool *mb_pool)
{
	struct pmd_internals *internals = dev->data->dev_private;
	dev->data->rx_queues[rx_queue_id] = &internals->rx_ring_queues[rx_queue_id];

	/* save config to be used in secondary device when required */
	internals->rx_ring_queues[rx_queue_id].nb_rx_desc = nb_rx_desc;

	memcpy(&internals->rx_ring_queues[rx_queue_id].rx_conf, rx_conf,
			 sizeof(*rx_conf));

	internals->rx_ring_queues[rx_queue_id].mb_pool = mb_pool;

	return 0;
}

static int
eth_tx_queue_setup(struct rte_eth_dev *dev, uint16_t tx_queue_id,
				    uint16_t nb_tx_desc,
				    unsigned int socket_id __rte_unused,
				    const struct rte_eth_txconf *tx_conf)
{
	struct pmd_internals *internals = dev->data->dev_private;
	dev->data->tx_queues[tx_queue_id] = &internals->tx_ring_queues[tx_queue_id];

	/* save config to be used in secondary device when required */
	internals->tx_ring_queues[tx_queue_id].nb_tx_desc = nb_tx_desc;

	memcpy(&internals->tx_ring_queues[tx_queue_id].tx_conf, tx_conf,
			 sizeof(*tx_conf));

	return 0;
}


static void
eth_dev_info(struct rte_eth_dev *dev,
		struct rte_eth_dev_info *dev_info)
{
	struct pmd_internals *internals = dev->data->dev_private;
	dev_info->driver_name = drivername;
	dev_info->max_mac_addrs = 1;
	dev_info->max_rx_pktlen = (uint32_t)-1;
	dev_info->max_rx_queues = (uint16_t)internals->nb_rx_queues;
	dev_info->max_tx_queues = (uint16_t)internals->nb_tx_queues;
	dev_info->min_rx_bufsize = 0;
	dev_info->pci_dev = NULL;
}

static void
eth_stats_get(struct rte_eth_dev *dev, struct rte_eth_stats *igb_stats)
{
	unsigned i;
	unsigned long rx_total = 0, tx_total = 0, tx_err_total = 0;
	const struct pmd_internals *internal = dev->data->dev_private;

	for (i = 0; i < RTE_ETHDEV_QUEUE_STAT_CNTRS &&
			i < internal->nb_rx_queues; i++) {
		igb_stats->q_ipackets[i] = internal->rx_ring_queues[i].rx_pkts.cnt;
		rx_total += igb_stats->q_ipackets[i];
	}

	for (i = 0; i < RTE_ETHDEV_QUEUE_STAT_CNTRS &&
			i < internal->nb_tx_queues; i++) {
		igb_stats->q_opackets[i] = internal->tx_ring_queues[i].tx_pkts.cnt;
		igb_stats->q_errors[i] = internal->tx_ring_queues[i].err_pkts.cnt;
		tx_total += igb_stats->q_opackets[i];
		tx_err_total += igb_stats->q_errors[i];
	}

	igb_stats->ipackets = rx_total;
	igb_stats->opackets = tx_total;
	igb_stats->oerrors = tx_err_total;
}

static void
eth_stats_reset(struct rte_eth_dev *dev)
{
	unsigned i;
	struct pmd_internals *internal = dev->data->dev_private;
	for (i = 0; i < internal->nb_rx_queues; i++)
		internal->rx_ring_queues[i].rx_pkts.cnt = 0;
	for (i = 0; i < internal->nb_tx_queues; i++) {
		internal->tx_ring_queues[i].tx_pkts.cnt = 0;
		internal->tx_ring_queues[i].err_pkts.cnt = 0;
	}
}

static void
eth_mac_addr_remove(struct rte_eth_dev *dev __rte_unused,
	uint32_t index __rte_unused)
{
}

static void
eth_mac_addr_add(struct rte_eth_dev *dev __rte_unused,
	struct ether_addr *mac_addr __rte_unused,
	uint32_t index __rte_unused,
	uint32_t vmdq __rte_unused)
{
}

static void
eth_queue_release(void *q __rte_unused) { ; }
static int
eth_link_update(struct rte_eth_dev *dev __rte_unused,
		int wait_to_complete __rte_unused) { return 0; }

static const struct eth_dev_ops ops = {
	.dev_start = eth_dev_start,
	.dev_stop = eth_dev_stop,
	.dev_set_link_up = eth_dev_set_link_up,
	.dev_set_link_down = eth_dev_set_link_down,
	.dev_configure = eth_dev_configure,
	.dev_infos_get = eth_dev_info,
	.rx_queue_setup = eth_rx_queue_setup,
	.tx_queue_setup = eth_tx_queue_setup,
	.rx_queue_release = eth_queue_release,
	.tx_queue_release = eth_queue_release,
	.link_update = eth_link_update,
	.stats_get = eth_stats_get,
	.stats_reset = eth_stats_reset,
	.mac_addr_remove = eth_mac_addr_remove,
	.mac_addr_add = eth_mac_addr_add,
};

int
rte_eth_from_rings(const char *name, struct rte_ring *const rx_queues[],
		const unsigned nb_rx_queues,
		struct rte_ring *const tx_queues[],
		const unsigned nb_tx_queues,
		const unsigned numa_node)
{
	struct rte_eth_dev_data *data = NULL;
	struct pmd_internals *internals = NULL;
	struct rte_eth_dev *eth_dev = NULL;

	unsigned i;

	/* do some parameter checking */
	if (rx_queues == NULL && nb_rx_queues > 0) {
		rte_errno = EINVAL;
		goto error;
	}
	if (tx_queues == NULL && nb_tx_queues > 0) {
		rte_errno = EINVAL;
		goto error;
	}
	if (nb_rx_queues > RTE_PMD_RING_MAX_RX_RINGS) {
		rte_errno = EINVAL;
		goto error;
	}

	RTE_LOG(INFO, PMD, "Creating rings-backed ethdev on numa socket %u\n",
			numa_node);

	/* now do all data allocation - for eth_dev structure, dummy pci driver
	 * and internal (private) data
	 */
	data = rte_zmalloc_socket(name, sizeof(*data), 0, numa_node);
	if (data == NULL) {
		rte_errno = ENOMEM;
		goto error;
	}

	data->rx_queues = rte_zmalloc_socket(name, sizeof(void *) * nb_rx_queues,
			0, numa_node);
	if (data->rx_queues == NULL) {
		rte_errno = ENOMEM;
		goto error;
	}

	data->tx_queues = rte_zmalloc_socket(name, sizeof(void *) * nb_tx_queues,
			0, numa_node);
	if (data->tx_queues == NULL) {
		rte_errno = ENOMEM;
		goto error;
	}

	internals = rte_zmalloc_socket(name, sizeof(*internals), 0, numa_node);
	if (internals == NULL) {
		rte_errno = ENOMEM;
		goto error;
	}

	/* reserve an ethdev entry */
	eth_dev = rte_eth_dev_allocate(name, RTE_ETH_DEV_VIRTUAL);
	if (eth_dev == NULL) {
		rte_errno = ENOSPC;
		goto error;
	}

	/* now put it all together
	 * - store queue data in internals,
	 * - store numa_node info in eth_dev_data
	 * - point eth_dev_data to internals
	 * - and point eth_dev structure to new eth_dev_data structure
	 */
	/* NOTE: we'll replace the data element, of originally allocated eth_dev
	 * so the rings are local per-process */

	internals->nb_rx_queues = nb_rx_queues;
	internals->nb_tx_queues = nb_tx_queues;
	for (i = 0; i < nb_rx_queues; i++) {
		internals->rx_ring_queues[i].rng = rx_queues[i];
		data->rx_queues[i] = &internals->rx_ring_queues[i];
	}
	for (i = 0; i < nb_tx_queues; i++) {
		internals->tx_ring_queues[i].rng = tx_queues[i];
		data->tx_queues[i] = &internals->tx_ring_queues[i];
		rte_spinlock_init(&internals->tx_ring_queues[i].send_cap_lock);
	}

	data->dev_private = internals;
	data->port_id = eth_dev->data->port_id;
	memmove(data->name, eth_dev->data->name, sizeof(data->name));
	data->nb_rx_queues = (uint16_t)nb_rx_queues;
	data->nb_tx_queues = (uint16_t)nb_tx_queues;
	data->dev_link = pmd_link;
	data->mac_addrs = &internals->address;

	eth_dev->data = data;
	eth_dev->driver = NULL;
	eth_dev->dev_ops = &ops;
	eth_dev->data->dev_flags = RTE_ETH_DEV_DETACHABLE;
	eth_dev->data->kdrv = RTE_KDRV_NONE;
	eth_dev->data->drv_name = drivername;
	eth_dev->data->numa_node = numa_node;

	TAILQ_INIT(&(eth_dev->link_intr_cbs));

	internals->state = STATE_NORMAL;

	/* finally assign rx and tx ops */
	eth_dev->rx_pkt_burst = eth_ring_normal_rx;
	eth_dev->tx_pkt_burst = eth_ring_normal_tx;

	return data->port_id;

error:
	if (data) {
		rte_free(data->rx_queues);
		rte_free(data->tx_queues);
	}
	rte_free(data);
	rte_free(internals);

	return -1;
}

int
rte_eth_from_internals(char * name, struct pmd_internals * internals)
{
	struct rte_eth_dev_data *data = NULL;
	struct rte_eth_dev *eth_dev = NULL;
	int numa_node = 0;	/* XXX: */
	unsigned i;

	RTE_LOG(INFO, PMD, "Ivshmem: creating rings-backed ethdev\n");

	/* now do all data allocation - for eth_dev structure, dummy pci driver
	 * and internal (private) data
	 */
	data = rte_zmalloc_socket(name, sizeof(*data), 0, numa_node);
	if (data == NULL) {
		rte_errno = ENOMEM;
		goto error;
	}

	data->rx_queues = rte_zmalloc_socket(name,
			sizeof(void *) * (internals->nb_rx_queues), 0, numa_node);
	if (data->rx_queues == NULL) {
		rte_errno = ENOMEM;
		goto error;
	}

	data->tx_queues = rte_zmalloc_socket(name,
			sizeof(void *) * (internals->nb_tx_queues), 0, numa_node);
	if (data->tx_queues == NULL) {
		rte_errno = ENOMEM;
		goto error;
	}

	/* reserve an ethdev entry */
	eth_dev = rte_eth_dev_allocate(name, RTE_ETH_DEV_VIRTUAL);
	if (eth_dev == NULL) {
		rte_errno = ENOSPC;
		goto error;
	}

	/* now put it all together
	 * - store queue data in internals,
	 * - store numa_node info in eth_dev_data
	 * - point eth_dev_data to internals
	 * - and point eth_dev structure to new eth_dev_data structure
	 */
	/* NOTE: we'll replace the data element, of originally allocated eth_dev
	 * so the rings are local per-process */

	for (i = 0; i < internals->nb_rx_queues; i++) {
		data->rx_queues[i] = &internals->rx_ring_queues[i];
	}
	for (i = 0; i < internals->nb_tx_queues; i++) {
		data->tx_queues[i] = &internals->tx_ring_queues[i];
		rte_spinlock_init(&internals->tx_ring_queues[i].send_cap_lock);
	}

	data->dev_private = internals;
	data->port_id = eth_dev->data->port_id;
	memmove(data->name, eth_dev->data->name, sizeof(data->name));
	data->nb_rx_queues = (uint16_t)internals->nb_rx_queues;
	data->nb_tx_queues = (uint16_t)internals->nb_tx_queues;
	data->dev_link = pmd_link;
	data->mac_addrs = &internals->address;

	eth_dev->data = data;
	eth_dev->driver = NULL;
	eth_dev->dev_ops = &ops;
	eth_dev->data->dev_flags = RTE_ETH_DEV_DETACHABLE;
	eth_dev->data->kdrv = RTE_KDRV_NONE;
	eth_dev->data->drv_name = drivername;
	eth_dev->data->numa_node = numa_node;

	TAILQ_INIT(&(eth_dev->link_intr_cbs));

	internals->state = STATE_NORMAL;

	/* finally assign rx and tx ops */
	eth_dev->rx_pkt_burst = eth_ring_normal_rx;
	eth_dev->tx_pkt_burst = eth_ring_normal_tx;

	return data->port_id;

error:
	if (data) {
		rte_free(data->rx_queues);
		rte_free(data->tx_queues);
	}
	rte_free(data);

	return -1;
}

int
rte_eth_from_ring(struct rte_ring *r)
{
	return rte_eth_from_rings(r->name, &r, 1, &r, 1,
			r->memzone ? r->memzone->socket_id : SOCKET_ID_ANY);
}

int rte_eth_ring_add_bypass_device(uint8_t normal_id, uint8_t bypass_id)
{
	struct rte_eth_dev *normal_port;

	struct rx_ring_queue *rx_q;
	struct tx_ring_queue *tx_q;
	int errval;

	if (!rte_eth_dev_is_valid_port(normal_id)) {
		RTE_LOG(ERR, PMD, "port id '%d' is not valid\n", normal_id);
		return -1;
	}

	if (!rte_eth_dev_is_valid_port(bypass_id)) {
		RTE_LOG(ERR, PMD, "port id '%d' is not valid\n", bypass_id);
		return -1;
	}

	rte_eth_dev_stop(bypass_id);

	normal_port = &rte_eth_devices[normal_id];

	/* Setup device */
	/* TODO: multiqueue support  */
	errval = rte_eth_dev_configure(bypass_id, 1, 1,
			  &(normal_port->data->dev_conf));
	if (errval != 0) {
		RTE_LOG(ERR, PMD, "Cannot configure slave device: port %u , err (%d)",
				bypass_id, errval);
		return errval;
	}

	/* Setup Rx Queues */
	rx_q = (struct rx_ring_queue *)normal_port->data->rx_queues[0];
	errval = rte_eth_rx_queue_setup(bypass_id, 0,
				rx_q->nb_rx_desc,
				rte_eth_dev_socket_id(bypass_id),
				&(rx_q->rx_conf), rx_q->mb_pool);
	if (errval != 0) {
		RTE_LOG(ERR, PMD, "rte_eth_rx_queue_setup: port=%d queue_id %d, err (%d)",
			bypass_id, 0, errval);
		return errval;
	}

	rx_q->bypass_id = bypass_id;

	/* Setup Tx Queues */
	tx_q = (struct tx_ring_queue *)normal_port->data->tx_queues[0];
	errval = rte_eth_tx_queue_setup(bypass_id, 0,
				tx_q->nb_tx_desc,
				rte_eth_dev_socket_id(bypass_id),
				&tx_q->tx_conf);
	if (errval != 0) {
		RTE_LOG(ERR, PMD, "rte_eth_tx_queue_setup: port=%d queue_id %d, err (%d)",
				bypass_id, 0, errval);
		return errval;
	}

	tx_q->bypass_id = bypass_id;

	/* Start device */
	errval = rte_eth_dev_start(bypass_id);
	if (errval != 0) {
		RTE_LOG(ERR, PMD, "rte_eth_dev_start: port=%u, err (%d)",
				bypass_id, errval);
		return -1;
	}

	tx_q->cap_sent = 0; /* no cap has been sent */

	/* this is used to guarantee that a cap packet is sent.
	 * Note that this is useful when applications are doing other things than
	 * sending traffic
	 */
	schedule_timeout(send_cap_normal, tx_q, 10000);

	/* now the port behaves in a special way */
	normal_port->rx_pkt_burst = eth_ring_creation_rx;
	normal_port->tx_pkt_burst = eth_ring_send_cap_creation_tx;

	return 0;
}

int rte_eth_ring_remove_bypass_device(uint8_t normal_id)
{
	struct rte_eth_dev * normal_port;
	struct tx_ring_queue *tx_q;

	if (!rte_eth_dev_is_valid_port(normal_id)) {
		RTE_LOG(ERR, PMD, "port id '%d' is not valid\n", normal_id);
		return -1;
	}

	normal_port = &rte_eth_devices[normal_id];

	tx_q = (struct tx_ring_queue *)normal_port->data->tx_queues[0];
	tx_q->cap_sent = 0; /* no cap has been sent */

	/* this is used to guarantee that a cap packet is sent.
	 * Note that this is useful when applications are doing other things than
	 * sending traffic
	 */
	schedule_timeout(send_cap_bypass, tx_q, 10000);

	/* when called, close the secondary device */
	normal_port->rx_pkt_burst = eth_ring_destruction_rx;
	normal_port->tx_pkt_burst = eth_ring_send_cap_destruction_tx;

	return 0;
}

enum dev_action{
	DEV_CREATE,
	DEV_ATTACH
};

static int
eth_dev_ring_create(const char *name, const unsigned numa_node,
		enum dev_action action)
{
	/* rx and tx are so-called from point of view of first port.
	 * They are inverted from the point of view of second port
	 */
	struct rte_ring *rxtx[RTE_PMD_RING_MAX_RX_RINGS];
	unsigned i;
	char rng_name[RTE_RING_NAMESIZE];
	unsigned num_rings = RTE_MIN(RTE_PMD_RING_MAX_RX_RINGS,
			RTE_PMD_RING_MAX_TX_RINGS);

	for (i = 0; i < num_rings; i++) {
		snprintf(rng_name, sizeof(rng_name), "ETH_RXTX%u_%s", i, name);
		rxtx[i] = (action == DEV_CREATE) ?
				rte_ring_create(rng_name, 1024, numa_node,
						RING_F_SP_ENQ|RING_F_SC_DEQ) :
				rte_ring_lookup(rng_name);
		if (rxtx[i] == NULL)
			return -1;
	}

	if (rte_eth_from_rings(name, rxtx, num_rings, rxtx, num_rings, numa_node) < 0)
		return -1;

	return 0;
}

struct node_action_pair {
	char name[PATH_MAX];
	unsigned node;
	enum dev_action action;
};

struct node_action_list {
	unsigned total;
	unsigned count;
	struct node_action_pair *list;
};

static int parse_kvlist (const char *key __rte_unused, const char *value, void *data)
{
	struct node_action_list *info = data;
	int ret;
	char *name;
	char *action;
	char *node;
	char *end;

	name = strdup(value);

	ret = -EINVAL;

	if (!name) {
		RTE_LOG(WARNING, PMD, "command line paramter is empty for ring pmd!\n");
		goto out;
	}

	node = strchr(name, ':');
	if (!node) {
		RTE_LOG(WARNING, PMD, "could not parse node value from %s", name);
		goto out;
	}

	*node = '\0';
	node++;

	action = strchr(node, ':');
	if (!action) {
		RTE_LOG(WARNING, PMD, "could not action value from %s", node);
		goto out;
	}

	*action = '\0';
	action++;

	/*
	 * Need to do some sanity checking here
	 */

	if (strcmp(action, ETH_RING_ACTION_ATTACH) == 0)
		info->list[info->count].action = DEV_ATTACH;
	else if (strcmp(action, ETH_RING_ACTION_CREATE) == 0)
		info->list[info->count].action = DEV_CREATE;
	else
		goto out;

	errno = 0;
	info->list[info->count].node = strtol(node, &end, 10);

	if ((errno != 0) || (*end != '\0')) {
		RTE_LOG(WARNING, PMD, "node value %s is unparseable as a number\n", node);
		goto out;
	}

	snprintf(info->list[info->count].name, sizeof(info->list[info->count].name), "%s", name);

	info->count++;

	ret = 0;
out:
	free(name);
	return ret;
}

static int
rte_pmd_ring_devinit(const char *name, const char *params)
{
	struct rte_kvargs *kvlist = NULL;
	int ret = 0;
	struct node_action_list *info = NULL;

	RTE_LOG(INFO, PMD, "Initializing pmd_ring for %s\n", name);

	if (params == NULL || params[0] == '\0') {
		ret = eth_dev_ring_create(name, rte_socket_id(), DEV_CREATE);
		if (ret == -1) {
			RTE_LOG(INFO, PMD,
				"Attach to pmd_ring for %s\n", name);
			ret = eth_dev_ring_create(name, rte_socket_id(),
						  DEV_ATTACH);
		}
	}
	else {
		kvlist = rte_kvargs_parse(params, valid_arguments);

		if (!kvlist) {
			RTE_LOG(INFO, PMD, "Ignoring unsupported parameters when creating"
					" rings-backed ethernet device\n");
			ret = eth_dev_ring_create(name, rte_socket_id(),
						  DEV_CREATE);
			if (ret == -1) {
				RTE_LOG(INFO, PMD,
					"Attach to pmd_ring for %s\n",
					name);
				ret = eth_dev_ring_create(name, rte_socket_id(),
							  DEV_ATTACH);
			}
			return ret;
		} else {
			ret = rte_kvargs_count(kvlist, ETH_RING_NUMA_NODE_ACTION_ARG);
			info = rte_zmalloc("struct node_action_list",
					   sizeof(struct node_action_list) +
					   (sizeof(struct node_action_pair) * ret),
					   0);
			if (!info)
				goto out_free;

			info->total = ret;
			info->list = (struct node_action_pair*)(info + 1);

			ret = rte_kvargs_process(kvlist, ETH_RING_NUMA_NODE_ACTION_ARG,
						 parse_kvlist, info);

			if (ret < 0)
				goto out_free;

			for (info->count = 0; info->count < info->total; info->count++) {
				ret = eth_dev_ring_create(name,
							  info->list[info->count].node,
							  info->list[info->count].action);
				if ((ret == -1) &&
				    (info->list[info->count].action == DEV_CREATE)) {
					RTE_LOG(INFO, PMD,
						"Attach to pmd_ring for %s\n",
						name);
					ret = eth_dev_ring_create(name,
							info->list[info->count].node,
							DEV_ATTACH);
				}
			}
		}
	}

out_free:
	rte_kvargs_free(kvlist);
	rte_free(info);
	return ret;
}

static int
rte_pmd_ring_devuninit(const char *name)
{
	struct rte_eth_dev *eth_dev = NULL;

	RTE_LOG(INFO, PMD, "Un-Initializing pmd_ring for %s\n", name);

	if (name == NULL)
		return -EINVAL;

	/* find an ethdev entry */
	eth_dev = rte_eth_dev_allocated(name);
	if (eth_dev == NULL)
		return -ENODEV;

	eth_dev_stop(eth_dev);

	if (eth_dev->data) {
		rte_free(eth_dev->data->rx_queues);
		rte_free(eth_dev->data->tx_queues);
		rte_free(eth_dev->data->dev_private);
	}
	rte_free(eth_dev->data);

	rte_eth_dev_release_port(eth_dev);
	return 0;
}

static struct rte_driver pmd_ring_drv = {
	.name = "eth_ring",
	.type = PMD_VDEV,
	.init = rte_pmd_ring_devinit,
	.uninit = rte_pmd_ring_devuninit,
};

PMD_REGISTER_DRIVER(pmd_ring_drv);
