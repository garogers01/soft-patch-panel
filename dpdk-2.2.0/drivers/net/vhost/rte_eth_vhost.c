/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2010-2015 Intel Corporation.
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
 #include "rte_eth_vhost.h"
#include <unistd.h>
#include <pthread.h>

#include <rte_mbuf.h>
#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <rte_memcpy.h>
#include <rte_dev.h>
#include <rte_errno.h>
#include <rte_kvargs.h>
#include <rte_virtio_net.h>

#define ETH_VHOST_IFACE_ARG		"iface"

#ifdef RTE_LIBRTE_PMD_VHOST_DEBUG
#define PMD_DEBUG_TRACE(fmt, args...) do {                        \
		RTE_LOG(ERR, PMD, "%s: " fmt, __func__, ## args); \
	} while (0)
#else
#define PMD_DEBUG_TRACE(fmt, args...)
#endif

static const char *drivername = "VHOST PMD";

static const char *valid_arguments[] = {
	ETH_VHOST_IFACE_ARG,
	NULL
};

static struct ether_addr base_eth_addr = {
	.addr_bytes = {
		0x56 /* V */,
		0x48 /* H */,
		0x4F /* O */,
		0x53 /* S */,
		0x54 /* T */,
		0x00
	}
};

struct vhost_queue {
	struct virtio_net *device;
	struct pmd_internal *internal;
	struct rte_mempool *mb_pool;
	rte_atomic64_t rx_pkts;
	rte_atomic64_t tx_pkts;
	rte_atomic64_t err_pkts;
	rte_atomic16_t rx_executing;
	rte_atomic16_t tx_executing;
};

struct pmd_internal {
	TAILQ_ENTRY(pmd_internal) next;
	char *dev_name;
	char *iface_name;
	unsigned nb_rx_queues;
	unsigned nb_tx_queues;
	rte_atomic16_t xfer;

	struct vhost_queue rx_vhost_queues[RTE_PMD_RING_MAX_RX_RINGS];
	struct vhost_queue tx_vhost_queues[RTE_PMD_RING_MAX_TX_RINGS];
};

TAILQ_HEAD(pmd_internal_head, pmd_internal); static struct 
pmd_internal_head internals_list =
	TAILQ_HEAD_INITIALIZER(internals_list);

static pthread_mutex_t internal_list_lock = PTHREAD_MUTEX_INITIALIZER;

static struct rte_eth_link pmd_link = {
		.link_speed = 10000,
		.link_duplex = ETH_LINK_FULL_DUPLEX,
		.link_status = 0
};

static uint16_t
eth_vhost_rx(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs) {
	struct vhost_queue *r = q;
	uint16_t nb_rx = 0;

	if (unlikely(r->internal == NULL))
		return 0;

	if (unlikely(rte_atomic16_read(&r->internal->xfer) == 0))
		return 0;

	rte_atomic16_set(&r->rx_executing, 1);

	if (unlikely(rte_atomic16_read(&r->internal->xfer) == 0))
		goto out;

	nb_rx = (uint16_t)rte_vhost_dequeue_burst(r->device,
			VIRTIO_TXQ, r->mb_pool, bufs, nb_bufs);

	rte_atomic64_add(&(r->rx_pkts), nb_rx);

out:
	rte_atomic16_set(&r->rx_executing, 0);

	return nb_rx;
}

static uint16_t
eth_vhost_tx(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs) {
	struct vhost_queue *r = q;
	uint16_t i, nb_tx = 0;

	if (unlikely(r->internal == NULL))
		return 0;

	if (unlikely(rte_atomic16_read(&r->internal->xfer) == 0))
		return 0;

	rte_atomic16_set(&r->tx_executing, 1);

	if (unlikely(rte_atomic16_read(&r->internal->xfer) == 0))
		goto out;

	nb_tx = (uint16_t)rte_vhost_enqueue_burst(r->device,
			VIRTIO_RXQ, bufs, nb_bufs);

	rte_atomic64_add(&(r->tx_pkts), nb_tx);
	rte_atomic64_add(&(r->err_pkts), nb_bufs - nb_tx);

	for (i = 0; likely(i < nb_tx); i++)
		rte_pktmbuf_free(bufs[i]);

out:
	rte_atomic16_set(&r->tx_executing, 0);

	return nb_tx;
}

static int
eth_dev_configure(struct rte_eth_dev *dev __rte_unused) { return 0; }

static int
eth_dev_start(struct rte_eth_dev *dev)
{
	struct pmd_internal *internal = dev->data->dev_private;

	return rte_vhost_driver_register(internal->iface_name);
}

static void
eth_dev_stop(struct rte_eth_dev *dev)
{
	struct pmd_internal *internal = dev->data->dev_private;

	rte_vhost_driver_unregister(internal->iface_name);
}

static int
eth_rx_queue_setup(struct rte_eth_dev *dev, uint16_t rx_queue_id,
		   uint16_t nb_rx_desc __rte_unused,
		   unsigned int socket_id __rte_unused,
		   const struct rte_eth_rxconf *rx_conf __rte_unused,
		   struct rte_mempool *mb_pool)
{
	struct pmd_internal *internal = dev->data->dev_private;

	RTE_LOG(INFO, PMD, "VHOST rx queue %d\n", rx_queue_id);

	internal->rx_vhost_queues[rx_queue_id].mb_pool = mb_pool;
	dev->data->rx_queues[rx_queue_id] = &internal->rx_vhost_queues[rx_queue_id];
	return 0;
}

static int
eth_tx_queue_setup(struct rte_eth_dev *dev, uint16_t tx_queue_id,
		   uint16_t nb_tx_desc __rte_unused,
		   unsigned int socket_id __rte_unused,
		   const struct rte_eth_txconf *tx_conf __rte_unused) {
	struct pmd_internal *internal = dev->data->dev_private;

	RTE_LOG(INFO, PMD, "VHOST tx queue %d\n", tx_queue_id);
		
	dev->data->tx_queues[tx_queue_id] = &internal->tx_vhost_queues[tx_queue_id];
	return 0;
}


static void
eth_dev_info(struct rte_eth_dev *dev,
	     struct rte_eth_dev_info *dev_info) {
	struct pmd_internal *internal = dev->data->dev_private;

	dev_info->driver_name = drivername;
	dev_info->max_mac_addrs = 1;
	dev_info->max_rx_pktlen = (uint32_t)-1;
	dev_info->max_rx_queues = (uint16_t)internal->nb_rx_queues;
	dev_info->max_tx_queues = (uint16_t)internal->nb_tx_queues;
	dev_info->min_rx_bufsize = 0;
	dev_info->pci_dev = dev->pci_dev;
}

static void
eth_stats_get(struct rte_eth_dev *dev, struct rte_eth_stats *igb_stats) 
{
	unsigned i;
	unsigned long rx_total = 0, tx_total = 0, tx_err_total = 0;
	const struct pmd_internal *internal = dev->data->dev_private;

	for (i = 0; i < RTE_ETHDEV_QUEUE_STAT_CNTRS &&
	     i < internal->nb_rx_queues; i++) {
		igb_stats->q_ipackets[i] = internal->rx_vhost_queues[i].rx_pkts.cnt;
		rx_total += igb_stats->q_ipackets[i];
	}

	for (i = 0; i < RTE_ETHDEV_QUEUE_STAT_CNTRS &&
	     i < internal->nb_tx_queues; i++) {
		igb_stats->q_opackets[i] = internal->tx_vhost_queues[i].tx_pkts.cnt;
		igb_stats->q_errors[i] = internal->tx_vhost_queues[i].err_pkts.cnt;
		tx_total += igb_stats->q_opackets[i];
		tx_err_total += igb_stats->q_errors[i];
	}

	igb_stats->ipackets = rx_total;
	igb_stats->opackets = tx_total;
	igb_stats->oerrors = tx_err_total;
}

static void
eth_stats_reset(struct rte_eth_dev *dev) {
	unsigned i;
	struct pmd_internal *internal = dev->data->dev_private;

	for (i = 0; i < internal->nb_rx_queues; i++)
		internal->rx_vhost_queues[i].rx_pkts.cnt = 0;
	for (i = 0; i < internal->nb_tx_queues; i++) {
		internal->tx_vhost_queues[i].tx_pkts.cnt = 0;
		internal->tx_vhost_queues[i].err_pkts.cnt = 0;
	}
}

static void
eth_queue_release(void *q __rte_unused) { ; } static int 
eth_link_update(struct rte_eth_dev *dev __rte_unused,
		int wait_to_complete __rte_unused) { return 0; }

static const struct eth_dev_ops ops = {
	.dev_start = eth_dev_start,
	.dev_stop = eth_dev_stop,
	.dev_configure = eth_dev_configure,
	.dev_infos_get = eth_dev_info,
	.rx_queue_setup = eth_rx_queue_setup,
	.tx_queue_setup = eth_tx_queue_setup,
	.rx_queue_release = eth_queue_release,
	.tx_queue_release = eth_queue_release,
	.link_update = eth_link_update,
	.stats_get = eth_stats_get,
	.stats_reset = eth_stats_reset,
};

static struct eth_driver rte_vhost_pmd = {
	.pci_drv = {
		.name = "rte_vhost_pmd",
		.drv_flags = RTE_PCI_DRV_DETACHABLE,
	},
};

static struct rte_pci_id id_table;

static inline struct pmd_internal *
find_internal_resource(char *ifname)
{
	int found = 0;
	struct pmd_internal *internal;

	if (ifname == NULL)
		return NULL;

	pthread_mutex_lock(&internal_list_lock);

	TAILQ_FOREACH(internal, &internals_list, next) {
		if (!strcmp(internal->iface_name, ifname)) {
			found = 1;
			break;
		}
	}

	pthread_mutex_unlock(&internal_list_lock);

	if (!found)
		return NULL;

	return internal;
}

static int
new_device(struct virtio_net *dev)
{
	struct rte_eth_dev *eth_dev;
	struct pmd_internal *internal;
	struct vhost_queue *vq;
	unsigned i;

	if (dev == NULL) {
		RTE_LOG(INFO, PMD, "invalid argument\n");
		return -1;
	}

	internal = find_internal_resource(dev->ifname);
	if (internal == NULL) {
		RTE_LOG(INFO, PMD, "invalid device name\n");
		return -1;
	}

	eth_dev = rte_eth_dev_allocated(internal->dev_name);
	if (eth_dev == NULL) {
		RTE_LOG(INFO, PMD, "failuer to find ethdev\n");
		return -1;
	}

	internal = eth_dev->data->dev_private;

	for (i = 0; i < internal->nb_rx_queues; i++) {
		vq = &internal->rx_vhost_queues[i];
		vq->device = dev;
		vq->internal = internal;
	}
	for (i = 0; i < internal->nb_tx_queues; i++) {
		vq = &internal->tx_vhost_queues[i];
		vq->device = dev;
		vq->internal = internal;
	}

	dev->flags |= VIRTIO_DEV_RUNNING;
	dev->priv = eth_dev;

	eth_dev->data->dev_link.link_status = 1;
	rte_atomic16_set(&internal->xfer, 1);

	RTE_LOG(INFO, PMD, "New connection established\n");

	return 0;
}

static void
destroy_device(volatile struct virtio_net *dev) {
	struct rte_eth_dev *eth_dev;
	struct pmd_internal *internal;
	struct vhost_queue *vq;
	unsigned i;

	if (dev == NULL) {
		RTE_LOG(INFO, PMD, "invalid argument\n");
		return;
	}

	eth_dev = (struct rte_eth_dev *)dev->priv;
	if (eth_dev == NULL) {
		RTE_LOG(INFO, PMD, "failuer to find a ethdev\n");
		return;
	}

	internal = eth_dev->data->dev_private;

	/* Wait until rx/tx_pkt_burst stops accesing vhost device */
	rte_atomic16_set(&internal->xfer, 0);
	for (i = 0; i < internal->nb_rx_queues; i++) {
		vq = &internal->rx_vhost_queues[i];
		while (rte_atomic16_read(&vq->rx_executing))
			rte_pause();
	}
	for (i = 0; i < internal->nb_tx_queues; i++) {
		vq = &internal->tx_vhost_queues[i];
		while (rte_atomic16_read(&vq->tx_executing))
			rte_pause();
	}

	eth_dev->data->dev_link.link_status = 0;

	dev->priv = NULL;
	dev->flags &= ~VIRTIO_DEV_RUNNING;

	for (i = 0; i < internal->nb_rx_queues; i++) {
		vq = &internal->rx_vhost_queues[i];
		vq->device = NULL;
	}
	for (i = 0; i < internal->nb_tx_queues; i++) {
		vq = &internal->tx_vhost_queues[i];
		vq->device = NULL;
	}

	RTE_LOG(INFO, PMD, "Connection closed\n"); }

static void *vhost_driver_session(void *param __rte_unused) {
	static struct virtio_net_device_ops *vhost_ops;

	vhost_ops = rte_zmalloc(NULL, sizeof(*vhost_ops), 0);
	if (vhost_ops == NULL)
		rte_panic("Can't allocate memory\n");

	/* set vhost arguments */
	vhost_ops->new_device = new_device;
	vhost_ops->destroy_device = destroy_device;
	if (rte_vhost_driver_callback_register(vhost_ops) < 0)
		rte_panic("Can't register callbacks\n");

	/* start event handling */
	rte_vhost_driver_session_start();

	rte_free(vhost_ops);
	pthread_exit(0);
}

static pthread_once_t once_cont = PTHREAD_ONCE_INIT; static pthread_t 
session_th;

static void vhost_driver_session_start(void) {
	int ret;

	ret = pthread_create(&session_th, NULL, vhost_driver_session, NULL);
	if (ret)
		rte_panic("Can't create a thread\n"); }

static int
eth_dev_vhost_create(const char *name, int index,
		     char *iface_name,
		     const unsigned numa_node)
{
	struct rte_eth_dev_data *data = NULL;
	struct rte_pci_device *pci_dev = NULL;
	struct pmd_internal *internal = NULL;
	struct rte_eth_dev *eth_dev = NULL;
	struct ether_addr *eth_addr = NULL;
	uint16_t nb_rx_queues = 1;
	uint16_t nb_tx_queues = 1;

	RTE_LOG(INFO, PMD, "Creating VHOST-USER backend name %s index %d iface %s on numa %u\n",
		name, index, iface_name, numa_node);

	/* now do all data allocation - for eth_dev structure, dummy pci driver
	 * and internal (private) data
	 */
	data = rte_zmalloc_socket(name, sizeof(*data), 0, numa_node);
	if (data == NULL)
		goto error;

	pci_dev = rte_zmalloc_socket(name, sizeof(*pci_dev), 0, numa_node);
	if (pci_dev == NULL)
		goto error;

	internal = rte_zmalloc_socket(name, sizeof(*internal), 0, numa_node);
	if (internal == NULL)
		goto error;

	eth_addr = rte_zmalloc_socket(name, sizeof(*eth_addr), 0, numa_node);
	if (eth_addr == NULL)
		goto error;
	*eth_addr = base_eth_addr;
	eth_addr->addr_bytes[5] = index;

	/* reserve an ethdev entry */
	eth_dev = rte_eth_dev_allocate(name, RTE_ETH_DEV_VIRTUAL);
	if (eth_dev == NULL)
		goto error;

	/* now put it all together
	 * - store queue data in internal,
	 * - store numa_node info in pci_driver
	 * - point eth_dev_data to internal and pci_driver
	 * - and point eth_dev structure to new eth_dev_data structure
	 */
	internal->nb_rx_queues = nb_rx_queues;
	internal->nb_tx_queues = nb_tx_queues;
	internal->dev_name = strdup(name);
	if (internal->dev_name == NULL)
		goto error;
	internal->iface_name = strdup(iface_name);
	if (internal->iface_name == NULL)
		goto error;

	pthread_mutex_lock(&internal_list_lock);
	TAILQ_INSERT_TAIL(&internals_list, internal, next);
	pthread_mutex_unlock(&internal_list_lock);

	rte_vhost_pmd.pci_drv.name = drivername;
	rte_vhost_pmd.pci_drv.id_table = &id_table;

	pci_dev->numa_node = numa_node;
	pci_dev->driver = &rte_vhost_pmd.pci_drv;

	data->dev_private = internal;
	data->port_id = eth_dev->data->port_id;
	memmove(data->name, eth_dev->data->name, sizeof(data->name));
	data->nb_rx_queues = (uint16_t)nb_rx_queues;
	data->nb_tx_queues = (uint16_t)nb_tx_queues;
	data->dev_link = pmd_link;
	data->mac_addrs = eth_addr;

	/* We'll replace the 'data' originally allocated by eth_dev. So the
	 * vhost PMD resources won't be shared between multi processes.
	 */
	eth_dev->data = data;
	eth_dev->driver = &rte_vhost_pmd;
	eth_dev->dev_ops = &ops;
	eth_dev->pci_dev = pci_dev;

	/* finally assign rx and tx ops */
	eth_dev->rx_pkt_burst = eth_vhost_rx;
	eth_dev->tx_pkt_burst = eth_vhost_tx;

	/* start vhost driver session. It should be called only once */
	pthread_once(&once_cont, vhost_driver_session_start);

	return data->port_id;

error:
	rte_free(data);
	rte_free(pci_dev);
	rte_free(internal);
	rte_free(eth_addr);

	return -1;
}

static int
rte_eth_from_vhost_create(const char *name, int index,
		    char *iface_name, const unsigned numa_node, 
			struct rte_mempool *mb_pool)
{
	struct rte_eth_dev_data *data = NULL;
	struct rte_pci_device *pci_dev = NULL;
	struct pmd_internal *internal = NULL;
	struct rte_eth_dev *eth_dev = NULL;
	struct ether_addr *eth_addr = NULL;
	uint16_t nb_rx_queues = 1;
	uint16_t nb_tx_queues = 1;
	
	unsigned i;
	
	RTE_LOG(INFO, PMD, "Creating VHOST-USER backend name %s index %d iface %s on numa %u\n",
		name, index, iface_name, numa_node);

	/* now do all data allocation - for eth_dev structure, dummy pci driver
	 * and internal (private) data
	 */
	data = rte_zmalloc_socket(name, sizeof(*data), 0, numa_node);
	if (data == NULL)
		goto error;

	data->rx_queues = rte_zmalloc_socket(name, sizeof(void *) * nb_rx_queues,
			0, numa_node);
	if (data->rx_queues == NULL) {
		PMD_DEBUG_TRACE("ENOMEM for rx queues\n");
		goto error;
	}

	data->tx_queues = rte_zmalloc_socket(name, sizeof(void *) * nb_tx_queues,
			0, numa_node);
	if (data->tx_queues == NULL) {
		PMD_DEBUG_TRACE("ENOMEM for TX queues\n");
		goto error;
	}	
	
	pci_dev = rte_zmalloc_socket(name, sizeof(*pci_dev), 0, numa_node);
	if (pci_dev == NULL)
		goto error;

	internal = rte_zmalloc_socket(name, sizeof(*internal), 0, numa_node);
	if (internal == NULL)
		goto error;

	eth_addr = rte_zmalloc_socket(name, sizeof(*eth_addr), 0, numa_node);
	if (eth_addr == NULL)
		goto error;
	*eth_addr = base_eth_addr;
	eth_addr->addr_bytes[5] = index;

	/* reserve an ethdev entry */
	eth_dev = rte_eth_dev_allocate(name, RTE_ETH_DEV_VIRTUAL);
	if (eth_dev == NULL)
		goto error;

	/* now put it all together
	 * - store queue data in internal,
	 * - store numa_node info in pci_driver
	 * - point eth_dev_data to internal and pci_driver
	 * - and point eth_dev structure to new eth_dev_data structure
	 */
	internal->nb_rx_queues = nb_rx_queues;
	internal->nb_tx_queues = nb_tx_queues;
	for (i = 0; i < nb_rx_queues; i++) {
		internal->rx_vhost_queues[i].mb_pool = mb_pool;
		data->rx_queues[i] = &internal->rx_vhost_queues[i];
	}
	for (i = 0; i < nb_tx_queues; i++) {
		internal->tx_vhost_queues[i].mb_pool = mb_pool;
		data->tx_queues[i] = &internal->tx_vhost_queues[i];
	}	
	
	internal->dev_name = strdup(name);
	if (internal->dev_name == NULL)
		goto error;
	internal->iface_name = strdup(iface_name);
	if (internal->iface_name == NULL)
		goto error;

	pthread_mutex_lock(&internal_list_lock);
	TAILQ_INSERT_TAIL(&internals_list, internal, next);
	pthread_mutex_unlock(&internal_list_lock);

	rte_vhost_pmd.pci_drv.name = drivername;
	rte_vhost_pmd.pci_drv.id_table = &id_table;

	pci_dev->numa_node = numa_node;
	pci_dev->driver = &rte_vhost_pmd.pci_drv;

	data->dev_private = internal;
	data->port_id = eth_dev->data->port_id;
	memmove(data->name, eth_dev->data->name, sizeof(data->name));
	data->nb_rx_queues = (uint16_t)nb_rx_queues;
	data->nb_tx_queues = (uint16_t)nb_tx_queues;
	data->dev_link = pmd_link;
	data->mac_addrs = eth_addr;

	/* We'll replace the 'data' originally allocated by eth_dev. So the
	 * vhost PMD resources won't be shared between multi processes.
	 */
	eth_dev->data = data;
	eth_dev->driver = &rte_vhost_pmd;
	eth_dev->dev_ops = &ops;
	eth_dev->pci_dev = pci_dev;

	/* finally assign rx and tx ops */
	eth_dev->rx_pkt_burst = eth_vhost_rx;
	eth_dev->tx_pkt_burst = eth_vhost_tx;

	/* start vhost driver session. It should be called only once */
	pthread_once(&once_cont, vhost_driver_session_start);

	return data->port_id;

error:
	rte_free(data);
	rte_free(pci_dev);
	rte_free(internal);
	rte_free(eth_addr);

	return -1;
}

static inline int
open_iface(const char *key __rte_unused, const char *value, void 
*extra_args) {
	const char **iface_name = extra_args;

	if (value == NULL)
		return -1;

	*iface_name = value;

	return 0;
}

int
rte_eth_from_vhost(const char *name, int index,
		     char *iface_name,
		     const unsigned numa_node, 
			 struct rte_mempool *mb_pool)
{
	int port_id = rte_eth_from_vhost_create(name, index,
		           iface_name, numa_node, mb_pool);

	if ( port_id >= 0 )
	{
		int ret = rte_vhost_driver_register(iface_name);	
		if (ret < 0)
			return ret;
	}	
				   
	return port_id;
}

static int
rte_pmd_vhost_devinit(const char *name, const char *params) {
	struct rte_kvargs *kvlist = NULL;
	int ret = 0;
	int index;
	char *iface_name;

	RTE_LOG(INFO, PMD, "Initializing pmd_vhost for %s\n", name);

	kvlist = rte_kvargs_parse(params, valid_arguments);
	if (kvlist == NULL)
		return -1;

	if (strlen(name) < strlen("eth_vhost"))
		return -1;

	index = strtol(name + strlen("eth_vhost"), NULL, 0);
	if (errno == ERANGE)
		return -1;

	if (rte_kvargs_count(kvlist, ETH_VHOST_IFACE_ARG) == 1) {
		ret = rte_kvargs_process(kvlist, ETH_VHOST_IFACE_ARG,
					 &open_iface, &iface_name);
		if (ret < 0)
			goto out_free;

		eth_dev_vhost_create(name, index, iface_name, rte_socket_id());
	}

out_free:
	rte_kvargs_free(kvlist);
	return ret;
}

static int
rte_pmd_vhost_devuninit(const char *name) {
	struct rte_eth_dev *eth_dev = NULL;
	struct pmd_internal *internal;

	RTE_LOG(INFO, PMD, "Un-Initializing pmd_vhost for %s\n", name);

	if (name == NULL)
		return -EINVAL;

	/* find an ethdev entry */
	eth_dev = rte_eth_dev_allocated(name);
	if (eth_dev == NULL)
		return -ENODEV;

	internal = eth_dev->data->dev_private;

	pthread_mutex_lock(&internal_list_lock);
	TAILQ_REMOVE(&internals_list, internal, next);
	pthread_mutex_unlock(&internal_list_lock);

	eth_dev_stop(eth_dev);

	if ((internal) && (internal->dev_name))
		free(internal->dev_name);
	if ((internal) && (internal->iface_name))
		free(internal->iface_name);
	rte_free(eth_dev->data->dev_private);
	rte_free(eth_dev->data);
	rte_free(eth_dev->pci_dev);

	rte_eth_dev_release_port(eth_dev);
	return 0;
}

static struct rte_driver pmd_vhost_drv = {
	.name = "eth_vhost",
	.type = PMD_VDEV,
	.init = rte_pmd_vhost_devinit,
	.uninit = rte_pmd_vhost_devuninit,
};

PMD_REGISTER_DRIVER(pmd_vhost_drv);

