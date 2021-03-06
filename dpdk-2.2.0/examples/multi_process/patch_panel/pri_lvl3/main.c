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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdarg.h>
#include <inttypes.h>
#include <sys/queue.h>
#include <errno.h>
#include <netinet/ip.h>

#include <sys/socket.h>
#include <sys/fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <time.h> 
#include <sys/un.h>
#include <pthread.h> 

#include <rte_common.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_tailq.h>
#include <rte_eal.h>
#include <rte_byteorder.h>
#include <rte_launch.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_atomic.h>
#include <rte_ring.h>
#include <rte_log.h>
#include <rte_debug.h>
#include <rte_mempool.h>
#include <rte_memcpy.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_eth_ring.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_ethdev.h>
#include <rte_byteorder.h>
#include <rte_malloc.h>
#include <rte_fbk_hash.h>
#include <rte_string_fns.h>

#include "common.h"
#include "args.h"
#include "init.h"

#define RTE_LOGTYPE_PP RTE_LOGTYPE_USER1

/*
 * When doing reads from the NIC or the client queues,
 * use this batch size
 */
#define PACKET_READ_SIZE 32
#define MAX_CLIENT 99
#define MAX_PARAMETER 10
#define MAX_PKT_BURST 32
#define MAX_CLIENT 99
#define MAX_PARAMETER 10
#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */
#define RTE_MP_RX_DESC_DEFAULT 512
#define RTE_MP_TX_DESC_DEFAULT 512

/* Command. */
typedef enum {
	STOP = 0,
	START = 1,
	ADD = 2,
	DEL = 3,
	READY = 4,
	FWDCPY = 5,
	FORWARD = 6,
	RXTX,
} cmd_type;
volatile cmd_type cmd = STOP;

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
 * Local buffers to put packets in, used to send packets in bursts to the
 * clients
 */
struct client_rx_buf {
	struct rte_mbuf *buffer[PACKET_READ_SIZE];
	uint16_t count;
};

struct rte_mempool *host_mp;
static uint8_t client_id = MAX_CLIENT;

static void
forward(void)
{
	uint16_t nb_rx;
	uint16_t nb_tx;
	int i;
	int in_port;
	int out_port;
	
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
				/*RTE_LOG(DEBUG, APP, "Fwd: %d to %d\n", in_port, out_port );*/
				
				/* Get burst of RX packets, from first port of pair. */
				struct rte_mbuf *bufs[MAX_PKT_BURST];
				/*first port rx, second port tx*/
				nb_rx = ports_fwd_array[in_port].rx_func(in_port, 0,
									bufs, MAX_PKT_BURST);
				if (unlikely(nb_rx == 0))
					continue;

				/* Send burst of TX packets, to second port of pair. */
				nb_tx = ports_fwd_array[out_port].tx_func(out_port, 0,
									bufs, nb_rx);
									
				/* Free any unsent packets. */
				if (unlikely(nb_tx < nb_rx)) {
					uint16_t buf;
					for (buf = nb_tx; buf < nb_rx; buf++)
						rte_pktmbuf_free(bufs[buf]);
				}
			}
		}
	}
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

int
main(int argc, char *argv[])
{
	unsigned lcore_id;
	int sock = SOCK_RESET, connected = 0, t;
	char str[MSG_SIZE];	
	int i;
	
	/* initialise the system */
	if (init(argc, argv) < 0 )
		return -1;
	
	/* initialize port forward array*/
	forward_array_init();	

	/* update port_forward_array with active port */
	for (i=0; i < RTE_MAX_ETHPORTS; i++)
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
	
	RTE_LOG(INFO, APP, "Finished Process Init.\n");

	lcore_id = 0; 
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		rte_eal_remote_launch(main_loop, NULL, lcore_id);
	}	
	
    memset(str, '\0', MSG_SIZE);
	while (1)
	{
		if (connected == 0)
		{
			if (sock < 0 )
			{
				RTE_LOG(INFO, APP, "Creating socket...\n");
				if ((sock = socket (AF_INET, SOCK_STREAM, 0)) <0) 
				{
					perror("ERROR: socket error");
					exit(1);
			    }
				//Creation of the socket
				memset(&servaddr, 0, sizeof(servaddr));
				servaddr.sin_family = AF_INET;
				servaddr.sin_addr.s_addr= inet_addr(server_ip);
				servaddr.sin_port =  htons(port); //convert to big-endian order				

			}

			RTE_LOG(INFO, APP, "Trying to connect ... socket %d\n", sock);
			if (connect(sock, (struct sockaddr *) &servaddr, sizeof(servaddr))<0) 	
			{
				perror("ERROR: Connection Error");
				connected = 0;
				sleep (1);
				continue;
			}
			else
			{
				RTE_LOG(INFO, APP, "Connected\n");
				connected = 1;
			}

		}
		
		memset(str,'\0',sizeof(str));
		if ((t=recv(sock, str, MSG_SIZE, 0)) > 0) 
		{
			RTE_LOG(DEBUG, APP, "Received string: %s\n", str);
			/* tokenize the user commands from controller */
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
			else if (strncmp(token_list[0], "add", 3) == 0)
			{
				RTE_LOG(DEBUG, APP, "add\n");
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
				if (strncmp(token_list[1], "pool", 4) == 0)
				{
					host_mp = rte_mempool_lookup("MProc_pktmbuf_pool");
					if (host_mp == NULL)
						rte_exit(EXIT_FAILURE, "Cannot get mempool for mbufs\n");
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
		
		} 
		else 
		{
			RTE_LOG(DEBUG, APP, "Receive count t: %d\n", t);
			if (t < 0)
			{
				perror("ERROR: Receive Fail");
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
			perror("ERROR: send failed");
			connected = 0;
			continue;
		}
		else
		{
			RTE_LOG(INFO, APP, "To Server: %s\n", str);
		}

	}
	
    printf("main end.\n");
	close(sock);
	return 0;

}
