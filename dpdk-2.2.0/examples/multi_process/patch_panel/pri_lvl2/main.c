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
#include <signal.h> //  our new library 
volatile sig_atomic_t on = 1;


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

/* Command. */
typedef enum {
	STOP = 0,
	START = 1,
	ADD = 2,
	DEL = 3,
	READY,
} cmd_type;
volatile cmd_type cmd = STOP;

struct thread_data
{
	int thread_id;
	int *socket;
	int  action;
	char command[MSG_SIZE];
	pthread_mutex_t mutex_cmd;
};

struct thread_data thread_data_array[MAX_CLIENTS];

/*
 * Local buffers to put packets in, used to send packets in bursts to the
 * clients
 */
struct client_rx_buf {
	struct rte_mbuf *buffer[PACKET_READ_SIZE];
	uint16_t count;
};

/* One buffer per client rx queue - dynamically allocate array */
static struct client_rx_buf *cl_rx_buf;


//static void turn_off(int sig){ // can be called asynchronously
//  on = 0; // set flag
//  printf ("terminated %d\n", sig);
//}

static const char *
get_printable_mac_addr(uint8_t port)
{
	static const char err_address[] = "00:00:00:00:00:00";
	static char addresses[RTE_MAX_ETHPORTS][sizeof(err_address)];

	if (unlikely(port >= RTE_MAX_ETHPORTS))
		return err_address;
	if (unlikely(addresses[port][0]=='\0')){
		struct ether_addr mac;
		rte_eth_macaddr_get(port, &mac);
		snprintf(addresses[port], sizeof(addresses[port]),
				"%02x:%02x:%02x:%02x:%02x:%02x\n",
				mac.addr_bytes[0], mac.addr_bytes[1], mac.addr_bytes[2],
				mac.addr_bytes[3], mac.addr_bytes[4], mac.addr_bytes[5]);
	}
	return addresses[port];
}

/*
 * This function displays the recorded statistics for each port
 * and for each client. It uses ANSI terminal codes to clear
 * screen when called. It is called from a single non-master
 * thread in the server process, when the process is run with more
 * than one lcore enabled.
 */
static void
do_stats_display(void)
{
	unsigned i, j;
	const char clr[] = { 27, '[', '2', 'J', '\0' };
	const char topLeft[] = { 27, '[', '1', ';', '1', 'H','\0' };
	uint64_t port_tx[RTE_MAX_ETHPORTS], port_tx_drop[RTE_MAX_ETHPORTS];
	uint64_t client_tx[MAX_CLIENTS], client_tx_drop[MAX_CLIENTS];

	/* to get TX stats, we need to do some summing calculations */
	memset(port_tx, 0, sizeof(port_tx));
	memset(port_tx_drop, 0, sizeof(port_tx_drop));
	memset(client_tx, 0, sizeof(client_tx));
	memset(client_tx_drop, 0, sizeof(client_tx_drop));

	for (i = 0; i < num_rings; i++){
		const volatile struct tx_stats *tx = &ports->tx_stats[i];
		for (j = 0; j < ports->num_ports; j++){
			/* assign to local variables here, save re-reading volatile vars */
			const uint64_t tx_val = tx->tx[ports->id[j]];
			const uint64_t drop_val = tx->tx_drop[ports->id[j]];
			port_tx[j] += tx_val;
			port_tx_drop[j] += drop_val;
			client_tx[i] += tx_val;
			client_tx_drop[i] += drop_val;
		}
	}

	/* Clear screen and move to top left */
	printf("%s%s", clr, topLeft);

	printf("PORTS\n");
	printf("-----\n");
	for (i = 0; i < ports->num_ports; i++)
		printf("Port %u: '%s'\t", (unsigned)ports->id[i],
				get_printable_mac_addr(ports->id[i]));
	printf("\n\n");
	for (i = 0; i < ports->num_ports; i++){
		printf("Port %u - rx: %9"PRIu64"\t"
				"tx: %9"PRIu64"\n",
				(unsigned)ports->id[i], ports->rx_stats.rx[i],
				port_tx[i]);
	}

	printf("\nCLIENTS\n");
	printf("-------\n");
	for (i = 0; i < num_rings; i++){
		const unsigned long long rx = clients[i].stats.rx;
		const unsigned long long rx_drop = clients[i].stats.rx_drop;
		printf("Client %2u - rx: %9llu, rx_drop: %9llu\n"
				"            tx: %9"PRIu64", tx_drop: %9"PRIu64"\n",
				i, rx, rx_drop, client_tx[i], client_tx_drop[i]);
	}

	printf("\n");
}

/*
 * The function called from each non-master lcore used by the process.
 * The test_and_set function is used to randomly pick a single lcore on which
 * the code to display the statistics will run. Otherwise, the code just
 * repeatedly sleeps.
 */
static int
sleep_lcore(__attribute__((unused)) void *dummy)
{
	/* Used to pick a display thread - static, so zero-initialised */
	static rte_atomic32_t display_stats;

	/* Only one core should display stats */
	if (rte_atomic32_test_and_set(&display_stats)) {
		const unsigned sleeptime = 1;
		RTE_LOG(INFO, APP, "Core %u displaying statistics\n", rte_lcore_id());
		
		/* Longer initial pause so above printf is seen */
		sleep(sleeptime * 3);

		/* Loop forever: sleep always returns 0 or <= param */
		while (sleep(sleeptime) <= sleeptime)
			do_stats_display();
	}
	return 0;
}

/*
 * Function to set all the client statistic values to zero.
 * Called at program startup.
 */
static void
clear_stats(void)
{
	unsigned i;

	for (i = 0; i < num_rings; i++)
		clients[i].stats.rx = clients[i].stats.rx_drop = 0;
}


	static void
meminfo_display(void)
{
	printf("----------- MEMORY_SEGMENTS -----------\n");
	rte_dump_physmem_layout(stdout);
	printf("--------- END_MEMORY_SEGMENTS ---------\n");

	printf("------------ MEMORY_ZONES -------------\n");
	rte_memzone_dump(stdout);
	printf("---------- END_MEMORY_ZONES -----------\n");

	printf("------------- TAIL_QUEUES -------------\n");
	rte_dump_tailq(stdout);
	printf("---------- END_TAIL_QUEUES ------------\n");
}

int
main(int argc, char *argv[])
{
	int sock = SOCK_RESET, connected = 0, t;
	char str[MSG_SIZE];

  
	/* initialise the system */
	if (init(argc, argv) < 0 )
		return -1;
	
	RTE_LOG(INFO, APP, "Finished Process Init.\n");

	cl_rx_buf = calloc(num_rings, sizeof(cl_rx_buf[0]));

	/* clear statistics */
	clear_stats();
	

	meminfo_display();

	/* put all other cores to sleep bar master */
	rte_eal_mp_remote_launch(sleep_lcore, NULL, SKIP_MASTER);
	
    memset(str, '\0', MSG_SIZE);
	while (on)
	{
		printf ("while loop running\n");
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
			int i = 0;				
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
				memset(str,'\0',sizeof(str));
				
				if (cmd == START)
				{
					sprintf(str, "Server Running\n");
				}
				else
				{
					sprintf(str, "Server Idling\n");					
				}	
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
			else if (!strcmp(token_list[0], "add"))
			{
			
				RTE_LOG(DEBUG, APP, "add\n");
				if (!strcmp(token_list[1], "ring"))
				{
					int ring_id = atoi(token_list[2]);
					/* look up ring, based on user's provided id*/ 
					struct rte_ring *ring = rte_ring_lookup(get_rx_queue_name(ring_id));
					if (ring == NULL)
						rte_exit(EXIT_FAILURE, "Cannot get RX ring - is server process running?\n");
					/* create ring pmd*/
					int ring_port_id = rte_eth_from_ring(ring);	
					RTE_LOG(DEBUG, APP, "ring port id %d\n", ring_port_id); 
				}				
			}
			else if (!strcmp(token_list[0], "del"))
			{
				RTE_LOG(DEBUG, APP, "del\n"); 
				cmd = STOP;
				
				if (!strcmp(token_list[1], "ring"))
				{
					RTE_LOG(DEBUG, APP, "Del ring id %d\n", atoi(token_list[2]));
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
	
	/* exit */
	close(sock);
	sock = SOCK_RESET;
    printf("ivshmem_py exit.\n");
	return 0;
}
