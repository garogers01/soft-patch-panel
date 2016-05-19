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

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/queue.h>
#include <errno.h>
#include <stdarg.h>
#include <inttypes.h>

#include <rte_common.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_tailq.h>
#include <rte_eal.h>
#include <rte_byteorder.h>
#include <rte_atomic.h>
#include <rte_launch.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_debug.h>
#include <rte_ring.h>
#include <rte_log.h>
#include <rte_mempool.h>
#include <rte_memcpy.h>
#include <rte_mbuf.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_eth_ring.h>
#include <rte_malloc.h>
#include <rte_fbk_hash.h>
#include <rte_string_fns.h>
#include <rte_cycles.h>
#include <rte_ivshmem.h>

#include "args.h"
#include "init.h"
#include "common.h"

#define MBUFS_PER_RING 1536
#define MBUFS_PER_PORT 1536
#define MBUF_CACHE_SIZE 512
#define MBUF_OVERHEAD (sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#define RX_MBUF_DATA_SIZE 2048
#define MBUF_SIZE (RX_MBUF_DATA_SIZE + MBUF_OVERHEAD)

#define RTE_MP_RX_DESC_DEFAULT 512
#define RTE_MP_TX_DESC_DEFAULT 512
#define CLIENT_QUEUE_RINGSIZE 128

#define NO_FLAGS 0

#define IVSHMEN_METADATA_NAME "pri_lvl2"
#define CMDLINE_OPT_FWD_CONF "fwd-conf"
#define QEMU_CMD_FMT "/tmp/ivshmem_qemu_cmdline_%s"

/* The mbuf pool for packet rx */
struct rte_mempool *pktmbuf_pool;

/* array of info/queues for clients */
struct client *clients = NULL;

/* the port details */
struct port_info *ports;

/**
 * Initialise the mbuf pool for packet reception for the NIC, and any other
 * buffer pools needed by the app - currently none.
 */
static int
init_mbuf_pools(void)
{
	const unsigned num_mbufs = (num_rings * MBUFS_PER_RING);

	/* don't pass single-producer/single-consumer flags to mbuf create as it
	 * seems faster to use a cache instead */
	printf("Creating mbuf pool '%s' [%u mbufs] ...\n",
			HSM_POOL_NAME, num_mbufs);

	if (rte_eal_process_type() == RTE_PROC_SECONDARY)
	{
		pktmbuf_pool = rte_mempool_lookup(HSM_POOL_NAME);
		if (pktmbuf_pool == NULL)
			rte_exit(EXIT_FAILURE, "Cannot get mempool for mbufs\n");
	}
	else
	{
		pktmbuf_pool = rte_mempool_create(HSM_POOL_NAME, num_mbufs,
				MBUF_SIZE, MBUF_CACHE_SIZE,
				sizeof(struct rte_pktmbuf_pool_private), rte_pktmbuf_pool_init,
				NULL, rte_pktmbuf_init, NULL, rte_socket_id(), NO_FLAGS );
				

	}

	return (pktmbuf_pool == NULL); /* 0  on success */
}

/**
 * Initialise an individual port:
 * - configure number of rx and tx rings
 * - set up each rx ring, to pull from the main mbuf pool
 * - set up each tx ring
 * - start the port and report its status to stdout
 */
int
init_port(uint8_t port_num)
{
	/* for port configuration all features are off by default */\
	const struct rte_eth_conf port_conf = {
		.rxmode = {
			.mq_mode = ETH_MQ_RX_RSS
		}
	};
	const uint16_t rx_rings = 1, tx_rings = 1;
	const uint16_t rx_ring_size = RTE_MP_RX_DESC_DEFAULT;
	const uint16_t tx_ring_size = RTE_MP_TX_DESC_DEFAULT;

	uint16_t q;
	int retval;

	printf("Port %u init ... ", (unsigned)port_num);
	fflush(stdout);
	
	
	/* Standard DPDK port initialisation - config port, then set up
	 * rx and tx rings */
	if ((retval = rte_eth_dev_configure(port_num, rx_rings, tx_rings,
		&port_conf)) != 0)
		return retval;

	for (q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(port_num, q, rx_ring_size,
				rte_eth_dev_socket_id(port_num),
				NULL, pktmbuf_pool);
		if (retval < 0) return retval;
	}

	for ( q = 0; q < tx_rings; q ++ ) {
		retval = rte_eth_tx_queue_setup(port_num, q, tx_ring_size,
				rte_eth_dev_socket_id(port_num),
				NULL);
		if (retval < 0) return retval;
	}

	rte_eth_promiscuous_enable(port_num);

	retval  = rte_eth_dev_start(port_num);
	
	if (retval < 0) return retval;

	printf( "Port %d Init done\n", port_num);

	return 0;
}

/**
 * Set up the DPDK rings which will be used to pass packets, via
 * pointers, between the multi-process server and client processes.
 * Each client needs one RX queue.
 */
static int
init_shm_rings(void)
{
	unsigned i;
	unsigned socket_id;
	const char * q_name;
	const unsigned ringsize = CLIENT_QUEUE_RINGSIZE;
	int retval;

	clients = rte_malloc("client details",
		sizeof(*clients) * num_rings, 0);
	if (clients == NULL)
		rte_exit(EXIT_FAILURE, "Cannot allocate memory for client program details\n");

	for (i = 0; i < num_rings; i++) {
		/* Create an RX queue for each client */
		socket_id = rte_socket_id();
		q_name = get_ring_name(i);
		if (rte_eal_process_type() == RTE_PROC_SECONDARY)
		{
			clients[i].rx_q = rte_ring_lookup(get_ring_name(i));
		}
		else
		{
			clients[i].rx_q = rte_ring_create(q_name,
					ringsize, socket_id,
					RING_F_SP_ENQ | RING_F_SC_DEQ ); /* single prod, single cons */
		}
		if (clients[i].rx_q == NULL)
			rte_exit(EXIT_FAILURE, "Cannot create rx ring queue for client %u\n", i);
		
	}
	if (retval < 0) return retval;
	
	return 0;
}

static int
print_to_file(const char *cmdline, const char *config_name)
{
	FILE *file;
	char path[PATH_MAX];

	snprintf(path, sizeof(path), QEMU_CMD_FMT, config_name);
	file = fopen(path, "w");
	if (file == NULL) {
		RTE_LOG(ERR, APP, "Could not open '%s' \n", path);
		return -1;
	}

	RTE_LOG(DEBUG, APP, "QEMU command line for config '%s': %s \n",
			config_name, cmdline);

	fprintf(file, "%s\n", cmdline);
	fclose(file);
	return 0;
}

static int
generate_ivshmem_cmdline(const char *config_name)
{
	char cmdline[PATH_MAX];
	if (rte_ivshmem_metadata_cmdline_generate(cmdline, sizeof(cmdline),
			config_name) < 0)
		return -1;

	if (print_to_file(cmdline, config_name) < 0)
		return -1;

	rte_ivshmem_metadata_dump(stdout, config_name);
	return 0;
}

/**
 * Main init function for the multi-process server app,
 * calls subfunctions to do each stage of the initialisation.
 */
int
init(int argc, char *argv[])
{
	int retval;
	uint8_t i, total_ports;

	/* init EAL, parsing EAL args */
	retval = rte_eal_init(argc, argv);
	if (retval < 0)
		return -1;
	argc -= retval;
	argv += retval;
	
	/* get total number of ports */
	total_ports = 0;

	/* parse additional, application arguments */
	retval = parse_app_args(total_ports, argc, argv);
	if (retval != 0)
		return -1;

	/* initialise mbuf pools */
	retval = init_mbuf_pools();
	if (retval != 0)
		rte_exit(EXIT_FAILURE, "Cannot create needed mbuf pools\n");

	
	init_shm_rings();
	
	if (rte_eal_process_type() == RTE_PROC_PRIMARY)
	{
		/* create metadata, output cmdline */
		if (rte_ivshmem_metadata_create(IVSHMEN_METADATA_NAME) < 0)
			rte_exit(EXIT_FAILURE, "Cannot create IVSHMEM metadata\n");	
		if (rte_ivshmem_metadata_add_mempool(pktmbuf_pool, IVSHMEN_METADATA_NAME))
			rte_exit(EXIT_FAILURE, "Cannot add mbuf mempool to IVSHMEM metadata\n");				
		
		for (i = 0; i < num_rings; i++) 
		{
			if (rte_ivshmem_metadata_add_ring(clients[i].rx_q,
					IVSHMEN_METADATA_NAME) < 0)
				rte_exit(EXIT_FAILURE, "Cannot add ring client %d to IVSHMEM metadata\n", i);
		}
		generate_ivshmem_cmdline(IVSHMEN_METADATA_NAME);
	}
	
	
	return 0;
}
