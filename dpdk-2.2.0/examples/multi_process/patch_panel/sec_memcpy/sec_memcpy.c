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
#include <inttypes.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/queue.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h> 

#include <rte_common.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_tailq.h>
#include <rte_eal.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_log.h>
#include <rte_cycles.h>
#include <rte_per_lcore.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_ring.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_debug.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_eth_ring.h>
#include <rte_eth_vhost.h>
#include <rte_string_fns.h>

#include "common.h"

#define RTE_LOGTYPE_NFV RTE_LOGTYPE_USER1

/* Number of packets to attempt to read from queue */
#define PKT_READ_SIZE  ((uint16_t)32)

#define MAX_PKT_BURST 32
#define MAX_CLIENT 99
#define MAX_PARAMETER 10
#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */
#define RTE_MP_RX_DESC_DEFAULT 512
#define RTE_MP_TX_DESC_DEFAULT 512

/* our client id number - tells us which rx queue to read, and NIC TX
 * queue to write to. */
static uint8_t client_id = MAX_CLIENT;
struct sockaddr_in servaddr;
char *server_ip;
int tcpport;

/* Command. */
typedef enum {
	STOP = 0,
	START = 1,
	ADD = 2,
	DEL = 3,
	READY = 4,
	LOOPBACK = 5,
	FORWARD = 6,
	RXTX,
} cmd_type;
volatile cmd_type cmd = STOP;

struct mbuf_queue {
#define MBQ_CAPACITY 32
	struct rte_mbuf *bufs[MBQ_CAPACITY];
	uint16_t top;
};

/* data structure to store port and fuction pointer.*/
unsigned exec_pos = 0;
unsigned save_pos = 0;
static unsigned rx_ports[RTE_MAX_ETHPORTS];
static uint16_t (*rx_funcs[RTE_MAX_ETHPORTS])(uint8_t, uint16_t, struct rte_mbuf **, uint16_t);
static unsigned tx_ports[RTE_MAX_ETHPORTS];
static uint16_t (*tx_funcs[RTE_MAX_ETHPORTS])(uint8_t, uint16_t, struct rte_mbuf **, uint16_t);
static struct rte_mempool *tx_pools[RTE_MAX_ETHPORTS];

static unsigned rx_rings[RTE_MAX_ETHPORTS];
static unsigned tx_rings[RTE_MAX_ETHPORTS];

struct port
{
	int status;
	port_type type;
	uint16_t (*rx_func)(uint8_t, uint16_t, struct rte_mbuf **, uint16_t);
	uint16_t (*tx_func)(uint8_t, uint16_t, struct rte_mbuf **, uint16_t);
	int out_port_id;
};

struct port ports_fwd_array[RTE_MAX_ETHPORTS];
/*
 * print a usage message
 */
static void
usage(const char *progname)
{
	printf("Usage: %s [EAL args] -- -n <client_id>\n\n", progname);
}

/*
 * Convert the client id number from a string to an int.
 */
static int
parse_client_num(const char *client)
{
	char *end = NULL;
	unsigned long temp;

	if (client == NULL || *client == '\0')
		return -1;

	temp = strtoul(client, &end, 10);
	if (end == NULL || *end != '\0')
		return -1;

	client_id = (uint8_t)temp;
	return 0;
}

static int
parse_server(char *server_port)
{
	const char delim[2] = ":";
	char *token;

	if (server_port == NULL || *server_port == '\0')
		return -1;

	server_ip = strtok(server_port, delim);
	printf( "server ip %s\n", server_ip );
	
	token = strtok(NULL, delim);
	printf( "token %s\n", token );
	if (token == NULL || *token == '\0')
		return -1;	
	
	printf( "token %s\n", token );
	tcpport = atoi(token);
	return 0;
}

/*
 * Parse the application arguments to the client app.
 */
static int
parse_app_args(int argc, char *argv[])
{
	int option_index, opt;
	char **argvopt = argv;
	const char *progname = NULL;
	static struct option lgopts[] = { /* no long options */
		{NULL, 0, 0, 0 }
	};
	progname = argv[0];

	while ((opt = getopt_long(argc, argvopt, "n:s:", lgopts,
		&option_index)) != EOF){
		switch (opt){
			case 'n':
				if (parse_client_num(optarg) != 0){
					usage(progname);
					return -1;
				}
				break;
			case 's':
				if (parse_server(optarg) != 0){
					usage(progname);
					return -1;
				}
				break;				
			default:
				usage(progname);
				return -1;
		}
	}
	
	return 0;
}

static int 
copy_mbufs(struct rte_mempool *pktmbuf_pool, uint8_t nb_mbuf, struct rte_mbuf **rxm, struct rte_mbuf **txm)
{
	uint16_t i;
	int ret = 1;

	
	if (rte_mempool_get_bulk(pktmbuf_pool, (void *)txm, nb_mbuf) !=0)
	{
		//RTE_LOG(ERR, APP, "pool: %s get buf failed\n", pktmbuf_pool->name);
		rte_mempool_put_bulk(rxm[0]->pool, (void *)rxm, nb_mbuf);
		return -1;
	}
		
	for (i=0; i<nb_mbuf; i++) 
	{
		/*
		txm[i] = rte_pktmbuf_alloc(pktmbuf_pool);
		if (unlikely(txm[i] == NULL)) {
			rte_pktmbuf_free(txm[i]);
			RTE_LOG(ERR, APP, "rte_pktmbuf_alloc() failed (%u)\n", i);
			return -1;
		}		
		*/	
		txm[i]->data_off = rxm[i]->data_off;
		txm[i]->data_len = rxm[i]->data_len;
		txm[i]->port = rxm[i]->port;
		txm[i]->vlan_tci = rxm[i]->vlan_tci;
		txm[i]->tx_offload = rxm[i]->tx_offload;
		txm[i]->hash = rxm[i]->hash;

		txm[i]->next = NULL;
		txm[i]->pkt_len = rxm[i]->pkt_len;
		txm[i]->nb_segs = rxm[i]->nb_segs;
		txm[i]->ol_flags = rxm[i]->ol_flags;
		txm[i]->packet_type = rxm[i]->packet_type;

		rte_memcpy(rte_pktmbuf_mtod(txm[i], char *),
					rte_pktmbuf_mtod(rxm[i], char *),
		   			rxm[i]->data_len);
					
		//rte_pktmbuf_dump(stdout, txm[i], txm[i]->data_len);
		//rte_pktmbuf_free(rxm[i]);
	}
	rte_mempool_put_bulk(rxm[0]->pool, (void *)rxm, nb_mbuf);
	
	return ret;
}

static void
forward(void)
{
	uint16_t nb_rx;
	uint16_t nb_tx;
	int i;
	int in_port;
	int out_port;

	struct rte_mbuf *bufs[MAX_PKT_BURST];	
	struct rte_mbuf *m[MAX_PKT_BURST];

	/* Go through every possible port numbers*/
	for (i = 0; i < RTE_MAX_ETHPORTS; i++)
	{
		if (ports_fwd_array[i].status >= 0)
		{
			/* if status active, i count is in port*/
			in_port = i;
			if ( ports_fwd_array[i].out_port_id >= 0)
			{
				
				out_port = ports_fwd_array[i].out_port_id;
				/* Get burst of RX packets, from first port of pair. */
				
				/*first port rx, second port tx*/
				nb_rx = ports_fwd_array[in_port].rx_func(in_port, 0,
									bufs, MAX_PKT_BURST);
				if (unlikely(nb_rx == 0))
					continue;
				
				if (unlikely ( copy_mbufs(tx_pools[out_port], nb_rx, bufs, m) < 0 ))
				{
					//RTE_LOG(DEBUG, APP, "copy mbufs fails %d -> %d\n", in_port, out_port);
					continue;
				}
	
				uint16_t buf =0;
			
				/* Send burst of TX packets, to second port of pair. */
				nb_tx = ports_fwd_array[out_port].tx_func(out_port, 0,
									m, nb_rx);
				/*nb_tx = ports_fwd_array[out_port].tx_func(out_port, 0,
									bufs, nb_rx);					*/
						
				/* Free any unsent packets. */
				if (unlikely(nb_tx < nb_rx)) {
					
					for (buf = nb_tx; buf < nb_rx; buf++)
						rte_pktmbuf_free(m[buf]);
				}
				
				//RTE_LOG(DEBUG, APP, "Fwd: port %d - %d pkts to port %d - %d\n", in_port, nb_rx, out_port, nb_tx );
			}
		}
	}
}

static void
rxtx(void)
{
	unsigned curr;

	curr = exec_pos;
		
	/* Get burst of RX packets, from first port of pair. */
	struct rte_mbuf *bufs[MAX_PKT_BURST];
	const uint16_t nb_rx = rx_funcs[curr](rx_ports[curr], 0,
			bufs, MAX_PKT_BURST);

	if (unlikely(nb_rx == 0))
		goto exit;

	/* Send burst of TX packets, to second port of pair. */
	const uint16_t nb_tx = tx_funcs[curr](tx_ports[curr], 0,
				bufs, nb_rx);

	/* Free any unsent packets. */
	if (unlikely(nb_tx < nb_rx)) {
		uint16_t buf;
		for (buf = nb_tx; buf < nb_rx; buf++)
			rte_pktmbuf_free(bufs[buf]);
	}

exit:
	;
}

/* main processing loop */
static void
nfv_loop(void)
{
	unsigned lcore_id;
	lcore_id = rte_lcore_id();

	RTE_LOG(INFO, APP, "entering main loop on lcore %u\n", lcore_id);

	
	while (1) 
	{
		if (unlikely(cmd == STOP))
		{
			sleep(1);
			/*RTE_LOG(INFO, APP, "Idling\n");*/
			continue;
		}
		else if (cmd == LOOPBACK || cmd == RXTX )
		{
			rxtx();
		}
		else if (cmd == FORWARD)
		{
			forward();
		}
	}
}

/* leading to nfv processing loop */
static int
main_loop(__attribute__((unused)) void *dummy)
{
	nfv_loop();
	return 0;
}

/* initialize forward array with default value*/
static void
forward_array_init(void)
{
	unsigned i;
	
	/* initialize port forward array*/
	for (i=0; i< RTE_MAX_ETHPORTS; i++)
	{
		ports_fwd_array[i].status = -99;
		ports_fwd_array[i].type = UNDEF;
		ports_fwd_array[i].out_port_id = -99;
	}	
}

static void
forward_array_reset(void)
{
	unsigned i;
	
	/* initialize port forward array*/
	for (i=0; i< RTE_MAX_ETHPORTS; i++)
	{
		if (ports_fwd_array[i].status > -1)
		{
				ports_fwd_array[i].out_port_id = -99;
				RTE_LOG(INFO, APP, "Port ID %d\n", i);
				RTE_LOG(INFO, APP, "out_port_id %d\n", ports_fwd_array[i].out_port_id);
		}
	}	
}

/* print forward array*/
static void
forward_array_print(void)
{
	unsigned i;
	
	/* every elements value*/
	for (i=0; i< RTE_MAX_ETHPORTS; i++)
	{
		RTE_LOG(INFO, APP, "Port ID %d\n", i);
		RTE_LOG(INFO, APP, "Status %d\n", ports_fwd_array[i].status);

		switch(ports_fwd_array[i].type) {
			case PHY :
				RTE_LOG(INFO, APP, "Type: PHY\n");
				break;
			case RING :
				RTE_LOG(INFO, APP, "Type: RING\n");
				break;
			case VHOST :
				RTE_LOG(INFO, APP, "Type: VHOST\n");
				break;
			case UNDEF :
				RTE_LOG(INFO, APP, "Type: UDF\n");
				break;				
		}
		RTE_LOG(INFO, APP, "Out Port ID %d\n", ports_fwd_array[i].out_port_id);
	}	
}

/* print forward array active port*/
static void
print_active_ports(char *str)
{
	unsigned i;
	
	sprintf(str, "%d\n", client_id);
	/* every elements value*/
	for (i=0; i< RTE_MAX_ETHPORTS; i++)
	{
		if (ports_fwd_array[i].status >= 0)
		{
			RTE_LOG(INFO, APP, "Port ID %d\n", i);
			RTE_LOG(INFO, APP, "Status %d\n", ports_fwd_array[i].status);
		
			sprintf(str + strlen(str), "port id: %d,", i);
			if (ports_fwd_array[i].status >= 0)
			{
					sprintf(str + strlen(str), "on,");
			}
			else
			{
				sprintf(str + strlen(str), "off,");
			}
						
			switch(ports_fwd_array[i].type) {
				case PHY :
					RTE_LOG(INFO, APP, "Type: PHY\n");
					sprintf(str + strlen(str), "PHY,");
					break;
				case RING :
					RTE_LOG(INFO, APP, "Type: RING\n");
					sprintf(str + strlen(str), "RING,");
					break;
				case VHOST :
					RTE_LOG(INFO, APP, "Type: VHOST\n");
					sprintf(str + strlen(str), "VHOST,");
					break;
				case UNDEF :
					RTE_LOG(INFO, APP, "Type: UDF\n");
					sprintf(str + strlen(str), "UDF,");
					break;				
			}
			RTE_LOG(INFO, APP, "Out Port ID %d\n", ports_fwd_array[i].out_port_id);	
			sprintf(str + strlen(str), "outport: %d\n", ports_fwd_array[i].out_port_id);
		}	
	}	
}
/*
 * Application main function - loops through
 * receiving and processing packets. Never returns
 */
int
main(int argc, char *argv[])
{
	unsigned lcore_id;
	unsigned nb_ports;
	int i;
	int retval;
		
	if ((retval = rte_eal_init(argc, argv)) < 0)
		return -1;
	argc -= retval;
	argv += retval;

	if (parse_app_args(argc, argv) < 0)
		rte_exit(EXIT_FAILURE, "Invalid command-line arguments\n"); 
	
	/* initialize port forward array*/
	forward_array_init();
	
	/* Check that there is an even number of ports to send/receive on. */
	nb_ports = rte_eth_dev_count();
	if (nb_ports == 0)
		printf("Number of Ports: %d\n", nb_ports);
	if (nb_ports > RTE_MAX_ETHPORTS)
		nb_ports = RTE_MAX_ETHPORTS;
	
	RTE_LOG(INFO, APP, "Number of Ports: %d\n", nb_ports);
	
	/* update port_forward_array with active port */
	for (i=0; i < (int) nb_ports; i++)
	{
		if (rte_eth_dev_is_valid_port(i))
		{
			if (rte_eth_dev_get_device_type(i) == RTE_ETH_DEV_PCI)
			{
				/* Update ports_fwd_array with phy port*/
				ports_fwd_array[i].status = i;
				ports_fwd_array[i].type = PHY;						
			}
		}
	}

	lcore_id = 0; 
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		rte_eal_remote_launch(main_loop, NULL, lcore_id);
	}		

	
	RTE_LOG(INFO, APP, "My ID %d start handling messsage\n", client_id);
	RTE_LOG(INFO, APP, "[Press Ctrl-C to quit ...]\n");
	
    int t;
    char str[MSG_SIZE];
    char client_name [MSG_SIZE];
		
	int sock = SOCK_RESET, connected = 0;
	
	/* send and receice msg loop */
	while(1)
	{
		if (connected == 0)
		{
			if ( sock < 0 )
			{
				RTE_LOG(INFO, APP, "Creating socket...\n");
				if ((sock = socket (AF_INET, SOCK_STREAM, 0)) <0) 
				{
					perror("socket error");
					exit(1);
			    }
				
				/*Create of the tcp socket*/
				memset(&servaddr, 0, sizeof(servaddr));
				servaddr.sin_family = AF_INET;
				servaddr.sin_addr.s_addr= inet_addr(server_ip);
				servaddr.sin_port =  htons(tcpport); //convert to big-endian order				

			}

			RTE_LOG(INFO, APP, "Trying to connect ... socket %d\n", sock);
			if (connect(sock, (struct sockaddr *) &servaddr, sizeof(servaddr))<0) 	
			{
				perror("ERR: Connection Error");
				connected = 0;
				sleep (1);
				continue;
			}
			else
			{
				RTE_LOG(INFO, APP, "Connected\n");
				connected = 1;
			}
/*
			memset(client_name,'\0',sizeof(client_name));
			sprintf(client_name, "%d\n", client_id);
			if (send(sock, client_name, strlen(client_name), 0) == -1) 
			{
				perror("ERR: Send Fail");
				connected = 0;
				continue;
			}
			if ((t=recv(sock, str, MSG_SIZE, 0)) > 0) 
			{	
				str[t] = '\0';
				RTE_LOG(INFO, APP, "Server Command: %s\n", str);
			} 
			else 
			{
				if (t < 0) 
					perror("ERR: Receive Failed");
				else
				{
					RTE_LOG(INFO, APP, "Server connection close\n");
					close(sock);
					sock = SOCK_RESET;
					connected = 0;
					continue;
				}
			}
*/
		}
		
		memset(str,'\0',sizeof(str));
		if ((t=recv(sock, str, MSG_SIZE, 0)) > 0) 
		{
			RTE_LOG(DEBUG, APP, "Received string: %s\n", str);
			/* tokenize user command from controller*/
			char *token_list[MAX_PARAMETER] = {NULL};
			i = 0;				
			token_list[i] = strtok(str, " ");
			while(token_list[i] != NULL) 
			{
				RTE_LOG(DEBUG, APP, "token %d = %s\n", i, token_list[i]);
				i++;
				token_list[i] = strtok(NULL, " ");
			}			
			if (!strcmp(token_list[0], "status"))
			{
				RTE_LOG(DEBUG, APP, "status\n");
				memset(str,'\0',sizeof(client_name));
				if (cmd == START)
				{
					sprintf(str, "Client ID %d Running\n", client_id);
				}
				else
				{
					sprintf(str, "Client ID %d Idling\n", client_id);
				}
				print_active_ports(str);
				forward_array_print();
			}
			if (!strcmp(token_list[0], "exit"))
			{
				RTE_LOG(DEBUG, APP, "exit\n");
				RTE_LOG(DEBUG, APP, "stop\n"); 
				cmd = STOP;
				
				break;	
			}			
			if (!strcmp(token_list[0], "start"))
			{
				RTE_LOG(DEBUG, APP, "start\n");
				cmd = START;
			}
			else if (!strcmp(token_list[0], "stop"))
			{
				RTE_LOG(DEBUG, APP, "stop\n"); 
				cmd = STOP;
			}
			else if (!strcmp(token_list[0], "forward"))
			{
				RTE_LOG(DEBUG, APP, "forward\n"); 
				cmd = FORWARD;
			}
			else if (!strcmp(token_list[0], "add"))
			{
				RTE_LOG(DEBUG, APP, "add\n");

				if (!strcmp(token_list[1], "vhost"))
				{
					struct rte_mempool *mp = rte_mempool_lookup(PKTMBUF_POOL_NAME);
					if (mp == NULL)
						rte_exit(EXIT_FAILURE, "Cannot get mempool for mbufs\n");
										
					int index = atoi(token_list[2]);
					//eth_vhost0 index 0 iface /tmp/sock0 on numa 0
					int vhost_port_id = -1;
					const unsigned  socket_id = rte_socket_id();
					const char *name = get_vhost_backend_name(index);
					char *iface = get_vhost_iface_name(index);
					vhost_port_id = rte_eth_from_vhost(name, index, iface, socket_id, mp);
					
					/* Update ports_fwd_array with vhost port*/
					ports_fwd_array[vhost_port_id].status = vhost_port_id;
					ports_fwd_array[vhost_port_id].type = VHOST;
					RTE_LOG(DEBUG, APP, "vhost port id %d\n", vhost_port_id); 
				}
				if (!strcmp(token_list[1], "ring"))
				{
					uint16_t ring_id = atoi(token_list[2]);
					struct rte_ring *ring;
					/* look up ring, based on user's provided id*/ 
					if ( ring_id >= 255 )
					{
						RTE_LOG(DEBUG, APP, "ring name %s\n", get_ring_name(ring_id >> 8));
						ring = rte_ring_lookup(get_ring_name(ring_id >> 8));
					}
					else
					{
						RTE_LOG(DEBUG, APP, "ring name %s\n", get_rx_queue_name(ring_id));
						ring = rte_ring_lookup(get_rx_queue_name(ring_id));
					}
					if (ring == NULL)
						rte_exit(EXIT_FAILURE, "Cannot get RX ring - is server process running?\n");
					/* create ring pmd*/
					uint16_t ring_port_id = rte_eth_from_ring_s0(ring);	
					/* Update ports_fwd_array with vhost port*/
					ports_fwd_array[ring_port_id].status = ring_port_id;
					ports_fwd_array[ring_port_id].type = RING;
					RTE_LOG(DEBUG, APP, "ring port id %d\n", ring_port_id); 
				}				
				if (!strcmp(token_list[1], "rx"))
				{
					if (!strcmp(token_list[2], "ring"))
					{
						rx_rings[save_pos] = atoi(token_list[3]);
						/* look up ring, based on user's provided id*/ 
						struct rte_ring *ring = rte_ring_lookup(get_rx_queue_name(rx_rings[save_pos]));
						if (ring == NULL)
							rte_exit(EXIT_FAILURE, "Cannot get RX ring - is server process running?\n");
						/* create ring pmd*/
						rx_ports[save_pos] = rte_eth_from_ring(ring);
						RTE_LOG(DEBUG, APP, "RX ring id %d\n", rx_rings[save_pos]); 
					}
					else 
					{
						rx_ports[save_pos] = atoi(token_list[2]);
					}
					rx_funcs[save_pos] = &rte_eth_rx_burst;
					RTE_LOG(DEBUG, APP, "RX port id %d\n", rx_ports[save_pos]);
				}
				if (!strcmp(token_list[1], "tx"))
				{
					if (!strcmp(token_list[2], "ring"))
					{
						tx_rings[save_pos] = atoi(token_list[3]);
						/* look up ring, based on user's provided id*/ 
						struct rte_ring *ring = rte_ring_lookup(get_rx_queue_name(tx_rings[save_pos]));
						if (ring == NULL)
							rte_exit(EXIT_FAILURE, "Cannot get RX ring - is server process running?\n");
						/* create ring pmd*/
						tx_ports[save_pos] = rte_eth_from_ring(ring);
						RTE_LOG(DEBUG, APP, "TX ring id %d\n", tx_rings[save_pos]);					
					}
					else if (!strcmp(token_list[2], "pool"))
					{	
						struct rte_mempool *mp = rte_mempool_lookup(token_list[3]);
						uint16_t tx_port_id = atoi(token_list[4]); 
						if (mp == NULL)
							RTE_LOG(DEBUG, APP, "Cannot get mempool for mbufs %s\n", token_list[3]);
						else
						{
							tx_pools[tx_port_id] = mp;
							RTE_LOG(DEBUG, APP, "TX port id %d using %s\n", tx_port_id, token_list[3] );	
						}
					}
					else
					{
						tx_ports[save_pos] = atoi(token_list[2]);
					}
					tx_funcs[save_pos] = &rte_eth_tx_burst;
					RTE_LOG(DEBUG, APP, "TX port id %d\n", tx_ports[save_pos]);					
				}
			}
			else if (!strcmp(token_list[0], "patch"))
			{
				RTE_LOG(DEBUG, APP, "patch\n");
				
				if (strncmp(token_list[1], "reset", 5) == 0)
				{
					/* reset forward array*/
					forward_array_reset();
				}
				else 
				{
					/* Populate in port data */ 
					int in_port = atoi(token_list[1]);
					ports_fwd_array[in_port].status = in_port; 
					ports_fwd_array[in_port].rx_func = &rte_eth_rx_burst;
					ports_fwd_array[in_port].tx_func = &rte_eth_tx_burst;
					int out_port = atoi(token_list[2]);
					ports_fwd_array[in_port].out_port_id = out_port; 
					
					/* Populate out port data */ 
					ports_fwd_array[out_port].status = out_port;
					ports_fwd_array[out_port].rx_func = &rte_eth_rx_burst;
					ports_fwd_array[out_port].tx_func = &rte_eth_tx_burst;

					RTE_LOG(DEBUG, APP, "STATUS: in port %d status %d\n", in_port, ports_fwd_array[in_port].status );
					RTE_LOG(DEBUG, APP, "STATUS: in port %d patch out port id %d\n", in_port, ports_fwd_array[in_port].out_port_id );
					RTE_LOG(DEBUG, APP, "STATUS: outport %d status %d\n", out_port, ports_fwd_array[out_port].status );
				}
			}
			else if (!strcmp(token_list[0], "del"))
			{
				RTE_LOG(DEBUG, APP, "del\n"); 
				cmd = STOP;
				
				if (!strcmp(token_list[1], "rx"))
				{
					RTE_LOG(DEBUG, APP, "Del RX port id %d\n", atoi(token_list[2]));
					rx_ports[save_pos] = RTE_MAX_ETHPORTS + 1;
					rx_funcs[save_pos] = NULL;					
				}
				if (!strcmp(token_list[1], "tx"))
				{
					RTE_LOG(DEBUG, APP, "Del RX port id %d\n", atoi(token_list[2]));
					tx_ports[save_pos] = RTE_MAX_ETHPORTS + 1;;
					tx_funcs[save_pos] = NULL;
				}
			}
			else if (!strcmp(token_list[0], "save"))
			{
				RTE_LOG(DEBUG, APP, "save\n");				
				unsigned input_pos = atoi(token_list[1]);
				if (input_pos == exec_pos)
				{
					sprintf(str, "Save == loop position: cleint %d\n", client_id);
				}
				else
				{
					save_pos = input_pos;
					sprintf(str, "Save changed: %d for client %d\n", save_pos, client_id);
				}
			}
			else if (!strcmp(token_list[0], "exec"))
			{
				RTE_LOG(DEBUG, APP, "save\n");				
				exec_pos = atoi(token_list[1]);

				sprintf(str, "Loop changed: %d for client %d\n", exec_pos, client_id);
			}			
		} 
		else 
		{
			RTE_LOG(DEBUG, APP, "Receive count t: %d\n", t);
			if (t < 0)
			{
				perror("ERR: Receive Fail");
			}
			else 
			{
				RTE_LOG(INFO, APP, "Receive 0\n");			
			}
			
			RTE_LOG(INFO, APP, "Assume Server closed connection\n");			
			close(sock);
			sock = SOCK_RESET;
			connected = 0;
			continue;
		}

		/*Send the message back to client*/
		if (send(sock , str , MSG_SIZE, 0) == -1)
		{
			perror("ERR: send failed");
			connected = 0;
			continue;
		}
		else
		{
			RTE_LOG(INFO, APP, "To Server: %s\n", str);
		}

	}

    close(sock);
    return 0;
}
