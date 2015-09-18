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
#include <rte_string_fns.h>

#include "common.h"

#define RTE_LOGTYPE_L2FWD RTE_LOGTYPE_USER1

/* Number of packets to attempt to read from queue */
#define PKT_READ_SIZE  ((uint16_t)32)

#define MAX_PKT_BURST 32
#define MAX_CLIENT 99
#define MAX_PARAMETER 10
#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */

/* our client id number - tells us which rx queue to read, and NIC TX
 * queue to write to. */
static uint8_t client_id = MAX_CLIENT;

/* Command. */
typedef enum {
	STOP = 0,
	START = 1,
	ADD = 2,
	DEL = 3,
	READY,
} cmd_type;
volatile cmd_type cmd = STOP;

struct mbuf_queue {
#define MBQ_CAPACITY 32
	struct rte_mbuf *bufs[MBQ_CAPACITY];
	uint16_t top;
};

/* data structure to store port and fuction pointer.*/
unsigned loop_pos = 0;
unsigned save_pos = 0;
static unsigned rx_ports[RTE_MAX_ETHPORTS];
static uint16_t (*rx_funcs[RTE_MAX_ETHPORTS])(uint8_t, uint16_t, struct rte_mbuf **, uint16_t);
static unsigned tx_ports[RTE_MAX_ETHPORTS];
static uint16_t (*tx_funcs[RTE_MAX_ETHPORTS])(uint8_t, uint16_t, struct rte_mbuf **, uint16_t);
static unsigned rx_rings[RTE_MAX_ETHPORTS];
static unsigned tx_rings[RTE_MAX_ETHPORTS];


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

	while ((opt = getopt_long(argc, argvopt, "n:", lgopts,
		&option_index)) != EOF){
		switch (opt){
			case 'n':
				if (parse_client_num(optarg) != 0){
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


/* main processing loop */
static void
nfv_loop(void)
{
	unsigned lcore_id;
	lcore_id = rte_lcore_id();
	unsigned curr;
	RTE_LOG(INFO, APP, "entering main loop on lcore %u\n", lcore_id);

	
	while (1) 
	{
		if (unlikely(cmd != START))
		{
			sleep(1);
			/*RTE_LOG(INFO, APP, "Idling\n");*/
			continue;
		}
	
		curr = loop_pos;
		
		/* Get burst of RX packets, from first port of pair. */
		struct rte_mbuf *bufs[MAX_PKT_BURST];
		const uint16_t nb_rx = rx_funcs[curr](rx_ports[curr], 0,
				bufs, MAX_PKT_BURST);

		if (unlikely(nb_rx == 0))
			continue;

		/* Send burst of TX packets, to second port of pair. */
		const uint16_t nb_tx = tx_funcs[curr](tx_ports[curr], 0,
				bufs, nb_rx);

		/* Free any unsent packets. */
		if (unlikely(nb_tx < nb_rx)) {
			uint16_t buf;
			for (buf = nb_tx; buf < nb_rx; buf++)
				rte_pktmbuf_free(bufs[buf]);
		}
	}
}
/*
static int add_port (char *cmd)
{
	char *token_list[MAX_PARAMETER] = {NULL};
	int i = 0;				
	token_list[i] = strtok(cmd, " ");
	while(token_list[i] != NULL) 
	{
		printf("token %d = %s\n", i, token_list[i]);
		i++;
		token_list[i] = strtok(NULL, " ");
	}	
	return 0;
}
*/
/* leading to nfv processing loop */
static int
main_loop(__attribute__((unused)) void *dummy)
{
	nfv_loop();
	return 0;
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
	int retval;
	
	if ((retval = rte_eal_init(argc, argv)) < 0)
		return -1;
	argc -= retval;
	argv += retval;

	if (parse_app_args(argc, argv) < 0)
		rte_exit(EXIT_FAILURE, "Invalid command-line arguments\n"); 
	
/*	
	struct rte_ring *ring1 = rte_ring_lookup(get_rx_queue_name(client_id));
	if (ring1 == NULL)
		rte_exit(EXIT_FAILURE, "Cannot get RX ring - is server process running?\n");
	
	int port1 = rte_eth_from_rings("PORT1", &ring1, 1, &ring1, 1, rte_socket_id());
	RTE_LOG(INFO, APP, "Ring port1 Number: %d\n", port1);
*/
	
	/* Check that there is an even number of ports to send/receive on. */
	nb_ports = rte_eth_dev_count();
	if (nb_ports == 0)
		rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");
	if (nb_ports > RTE_MAX_ETHPORTS)
		nb_ports = RTE_MAX_ETHPORTS;
	RTE_LOG(INFO, APP, "Number of Ports: %d\n", nb_ports);
	
	if (rte_lcore_count() < 2)
		RTE_LOG(INFO, APP, "Too few lcores enabled. Need more than 1\n");
	
	RTE_LOG(INFO, APP, "Finished Process Init, Launching threads\n");
	
	lcore_id = 0; 
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		rte_eal_remote_launch(main_loop, NULL, lcore_id);
	}	
	
	RTE_LOG(INFO, APP, "My ID %d start handling messsage\n", client_id);
	RTE_LOG(INFO, APP, "[Press Ctrl-C to quit ...]\n");
	
    int t;
    struct sockaddr_un remote;
    char str[MSG_SIZE];
    char client_name [MSG_SIZE];
		
	int sock = SOCK_RESET, len = 0, connected = 0;

	while(1)
	{

	
		if (connected == 0)
		{
			if ( sock < 0 )
			{
				RTE_LOG(INFO, APP, "Creating socket...\n");
				if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) 
				{
					perror("socket error");
					exit(1);
			    }

				remote.sun_family = AF_UNIX;
				strcpy(remote.sun_path, SOCK_PATH);
				len = strlen(remote.sun_path) + sizeof(remote.sun_family);
			}

		
			RTE_LOG(INFO, APP, "Trying to connect ... socket %d\n", sock);
			if (connect(sock, (struct sockaddr *)&remote, len) < 0) 
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

		}
		

		memset(str,'\0',sizeof(str));
		if ((t=recv(sock, str, MSG_SIZE, 0)) > 0) 
		{
			if (strncmp(str, "status", 6) == 0)
			{
				RTE_LOG(DEBUG, APP, "status\n");
				memset(str,'\0',sizeof(client_name));
				
				if (cmd == START)
					sprintf(str, "Client ID %d Running\n", client_id);
				else
					sprintf(str, "Client ID %d Idling\n", client_id);
			}
			if (strncmp(str, "start", 5) == 0)
			{
				RTE_LOG(DEBUG, APP, "start\n");
				cmd = START;
			}
			else if (strncmp(str, "stop", 4) == 0)
			{
				RTE_LOG(DEBUG, APP, "stop\n"); 
				cmd = STOP;
			}
			else if (strncmp(str, "add", 3) == 0)
			{
				RTE_LOG(DEBUG, APP, "add\n");
				char *token_list[MAX_PARAMETER] = {NULL};
				int i = 0;				
				token_list[i] = strtok(str, " ");
				while(token_list[i] != NULL) 
				{
					RTE_LOG(DEBUG, APP, "token %d = %s\n", i, token_list[i]);
					i++;
					token_list[i] = strtok(NULL, " ");
				}
				if (strncmp(token_list[1], "rx", 2) == 0)
				{
					if (strncmp(token_list[2], "ring", 4) == 0 )
					{
						rx_rings[save_pos] = atoi(token_list[3]);
						/* look up ring, based on user's provided id*/ 
						struct rte_ring *ring = rte_ring_lookup(get_rx_queue_name(rx_rings[save_pos]));
						if (ring == NULL)
							rte_exit(EXIT_FAILURE, "Cannot get RX ring - is server process running?\n");
						/* create ring pmd*/
						rx_ports[save_pos] = rte_eth_from_ring(ring);
					}
					else 
						rx_ports[save_pos] = atoi(token_list[2]);
					
					rx_funcs[save_pos] = &rte_eth_rx_burst;
					RTE_LOG(DEBUG, APP, "RX ring id %d\n", rx_rings[save_pos]); 
					RTE_LOG(DEBUG, APP, "RX port id %d\n", rx_ports[save_pos]);
					
				}
				if (strncmp(token_list[1], "tx", 2) == 0)
				{
					if (strncmp(token_list[2], "ring", 4) == 0 )
					{
						tx_rings[save_pos] = atoi(token_list[3]);
						/* look up ring, based on user's provided id*/ 
						struct rte_ring *ring = rte_ring_lookup(get_rx_queue_name(tx_rings[save_pos]));
						if (ring == NULL)
							rte_exit(EXIT_FAILURE, "Cannot get RX ring - is server process running?\n");
						/* create ring pmd*/
						tx_ports[save_pos] = rte_eth_from_ring(ring);
					}
					else
						tx_ports[save_pos] = atoi(token_list[2]);
					
					tx_funcs[save_pos] = &rte_eth_tx_burst;
					RTE_LOG(DEBUG, APP, "TX ring id %d\n", tx_rings[save_pos]);					
					RTE_LOG(DEBUG, APP, "TX port id %d\n", tx_ports[save_pos]);
				}
			}
			else if (strncmp(str, "del", 3) == 0)
			{
				RTE_LOG(DEBUG, APP, "del\n"); 
				cmd = STOP;
				
				char *token_list[MAX_PARAMETER] = {NULL};
				int i = 0;				
				token_list[i] = strtok(str, " ");
				while(token_list[i] != NULL) 
				{
					RTE_LOG(DEBUG, APP, "token %d = %s\n", i, token_list[i]);
					i++;
					token_list[i] = strtok(NULL, " ");
				}
				if (strncmp(token_list[1], "rx", 2) == 0)
				{
					RTE_LOG(DEBUG, APP, "Del RX port id %d\n", atoi(token_list[2]));
					rx_ports[save_pos] = RTE_MAX_ETHPORTS + 1;
					rx_funcs[save_pos] = NULL;					
				}
				if (strncmp(token_list[1], "tx", 2) == 0)
				{
					RTE_LOG(DEBUG, APP, "Del RX port id %d\n", atoi(token_list[2]));
					tx_ports[save_pos] = RTE_MAX_ETHPORTS + 1;;
					tx_funcs[save_pos] = NULL;
				}
			}
			else if (strncmp(str, "save", 4) == 0)
			{
				RTE_LOG(DEBUG, APP, "save\n");
				char *token_list[MAX_PARAMETER] = {NULL};
				int i = 0;				
				token_list[i] = strtok(str, " ");
				while(token_list[i] != NULL) 
				{
					RTE_LOG(DEBUG, APP, "token %d = %s\n", i, token_list[i]);
					i++;
					token_list[i] = strtok(NULL, " ");
				}
				
				unsigned input_pos = atoi(token_list[1]);
				if (input_pos == loop_pos)
				{
					sprintf(str, "Save == loop position: cleint %d\n", client_id);
				}
				else
				{
					save_pos = input_pos;
					sprintf(str, "Save changed: %d for client %d\n", save_pos, client_id);
				}
			}
			
			else if (strncmp(str, "loop", 4) == 0)
			{
				RTE_LOG(DEBUG, APP, "save\n");
				char *token_list[MAX_PARAMETER] = {NULL};
				int i = 0;				
				token_list[i] = strtok(str, " ");
				while(token_list[i] != NULL) 
				{
					RTE_LOG(DEBUG, APP, "token %d = %s\n", i, token_list[i]);
					i++;
					token_list[i] = strtok(NULL, " ");
				}
				
				loop_pos = atoi(token_list[1]);

				sprintf(str, "Loop changed: %d for client %d\n", loop_pos, client_id);
			}			
			RTE_LOG(DEBUG, APP, "Received string: %s\n", str);
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
